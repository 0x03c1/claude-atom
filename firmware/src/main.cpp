// Claude Atom: M5Stack AtomS3 + Atomic Echo Base como assistente de voz do Claude
// e painel fisico do Claude Code.
//
// Dois modos, ao mesmo tempo:
//  - Assistente (Wi-Fi): segure o botao e fale; o audio vai ao Google Speech-to-Text,
//    o texto ao Claude, e a resposta volta falada pelo Google Text-to-Speech.
//  - Painel (USB): com host/bridge.py rodando no computador, mostra o que o Claude Code
//    esta fazendo e responde pedidos de permissao pelo botao.
//
// Protocolo USB (JSON por linha), host -> device:
//   {"t":"ping"}
//   {"t":"state","s":"idle|thinking|tool|waiting|done|error","title":"..","m":"..","p":"projeto"}
//   {"t":"perm","id":"..","tool":"Bash","m":"rm -rf build","p":"projeto","ttl":50}
//   {"t":"cancel","id":".."}
//   {"t":"config","ssid":..,"pass":..,"anthropic":..,"google":..,"model":..,"voice":..,"volume":..}
//   {"t":"info"}
// device -> host:
//   {"t":"hello","fw":".."}   {"t":"pong","fw":".."}   {"t":"info",...}
//   {"t":"decision","id":"..","d":"allow|deny|ask"}
//
// Botao:
//   pedido de permissao: 1 clique = permitir, segurar = negar, 2 cliques = terminal
//   resto do tempo:      segurar = falar, clique = rolar/fechar resposta ou brilho,
//                        2 cliques = nova conversa
// Segurar o botao ao ligar abre o portal de configuracao.

#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "assistant.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

namespace {

constexpr const char *FW_VERSION = "claude-atom/2.0";
constexpr uint32_t HOST_TIMEOUT_MS = 10000;
constexpr uint32_t DONE_TO_IDLE_MS = 45000;
constexpr uint32_t REPLY_TO_IDLE_MS = 90000;
constexpr uint32_t ERROR_TO_IDLE_MS = 8000;
constexpr uint32_t RESULT_FLASH_MS = 1200;
constexpr uint32_t TOAST_MS = 2000;
constexpr uint32_t FRAME_MS = 33;
constexpr size_t MAX_LINE = 2048;
constexpr uint8_t BRIGHTNESS[] = {40, 110, 255};

struct Permission {
  bool active = false;
  String id;
  PermissionView view;
};

CompanionScreen companion;
Permission perm;
bool flashActive = false, flashAllowed = false;
uint32_t flashUntil = 0;
String toast;
uint32_t toastUntil = 0;
String rxLine;
uint32_t lastHostMs = 0;
bool hostSeen = false;
uint32_t lastFrameMs = 0;
uint8_t brightnessIdx = 1;
int answerPage = 0;
uint32_t answerSeq = 0;
bool listening = false;
bool listenAfterCancel = false;

bool hostConnected() { return hostSeen && millis() - lastHostMs < HOST_TIMEOUT_MS; }

void showToast(const String &text) {
  toast = text;
  toastUntil = millis() + TOAST_MS;
}

void beep(uint16_t freq, uint32_t ms) {
  if (assistantBusy()) return;  // microfone/alto-falante em uso
  if (!M5.Speaker.isEnabled()) return;
  M5.Speaker.begin();
  M5.Speaker.setVolume(settings.volume / 2);
  M5.Speaker.tone(freq, ms);
}

// ---------------------------------------------------------------- protocolo USB

void sendJson(JsonDocument &doc) {
  serializeJson(doc, Serial);
  Serial.print('\n');
}

void sendHello(const char *type) {
  JsonDocument doc;
  doc["t"] = type;
  doc["fw"] = FW_VERSION;
  sendJson(doc);
}

void sendInfo() {
  NetInfo net = netInfo();
  JsonDocument doc;
  doc["t"] = "info";
  doc["fw"] = FW_VERSION;
  doc["wifi"] = (int)net.state;
  doc["ssid"] = net.ssid;
  doc["ip"] = net.ip;
  doc["ap"] = net.apName;
  doc["model"] = settings.model;
  doc["anthropic"] = settings.anthropicKey.length() > 0;
  doc["google"] = settings.googleKey.length() > 0;
  doc["heap"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  M5Canvas probe(&M5.Display);  // confere se a fonte tem os acentos
  probe.setFont(&fonts::efontCN_12);
  doc["font_c"] = probe.textWidth("c");
  doc["font_cedil"] = probe.textWidth("ç");
  doc["font_atil"] = probe.textWidth("ã");
  doc["font_quote"] = probe.textWidth("“");
  sendJson(doc);
}

void sendDecision(const char *decision) {
  JsonDocument doc;
  doc["t"] = "decision";
  doc["id"] = perm.id;
  doc["d"] = decision;
  sendJson(doc);
}

CcState parseState(const char *s) {
  if (!strcmp(s, "thinking")) return CcState::Thinking;
  if (!strcmp(s, "tool")) return CcState::Tool;
  if (!strcmp(s, "waiting")) return CcState::Waiting;
  if (!strcmp(s, "done")) return CcState::Done;
  if (!strcmp(s, "error")) return CcState::Error;
  return CcState::Idle;
}

const char *defaultTitle(CcState s) {
  switch (s) {
    case CcState::Idle: return "Claude Code";
    case CcState::Thinking: return "Pensando";
    case CcState::Tool: return "Trabalhando";
    case CcState::Waiting: return "Sua vez";
    case CcState::Done: return "Pronto";
    case CcState::Error: return "Erro";
  }
  return "";
}

void setCompanion(CcState s, const String &title, const String &msg, const String &project) {
  companion.state = s;
  companion.title = title.length() ? title : String(defaultTitle(s));
  companion.msg = msg;
  if (project.length()) companion.project = project;
  companion.since = millis();
}

void applyConfig(JsonDocument &doc) {
  auto take = [&](const char *key, String &target) {
    if (doc[key].is<const char *>()) target = doc[key].as<String>();
  };
  take("ssid", settings.ssid);
  take("pass", settings.pass);
  take("anthropic", settings.anthropicKey);
  take("google", settings.googleKey);
  take("model", settings.model);
  take("voice", settings.voice);
  take("lang", settings.language);
  if (doc["volume"].is<int>()) settings.volume = constrain(doc["volume"].as<int>(), 0, 255);
  if (doc["speak"].is<bool>()) settings.speak = doc["speak"];
  settingsSave();
  JsonDocument ack;
  ack["t"] = "config_ok";
  sendJson(ack);
  Serial.flush();
  delay(300);
  ESP.restart();
}

void handleLine(const String &line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  lastHostMs = millis();
  hostSeen = true;

  const char *t = doc["t"] | "";
  if (!strcmp(t, "ping")) {
    sendHello("pong");
  } else if (!strcmp(t, "state")) {
    setCompanion(parseState(doc["s"] | "idle"), doc["title"] | "", doc["m"] | "", doc["p"] | "");
  } else if (!strcmp(t, "perm")) {
    perm.active = true;
    perm.id = doc["id"] | "";
    perm.view.tool = doc["tool"] | "?";
    perm.view.msg = doc["m"] | "";
    perm.view.project = doc["p"] | "";
    perm.view.start = millis();
    perm.view.ttlMs = (doc["ttl"] | 50) * 1000UL;
    flashActive = false;
    uiSetBrightness(255);
    beep(1760, 90);
  } else if (!strcmp(t, "cancel")) {
    if (perm.active && perm.id == (doc["id"] | "")) perm.active = false;
  } else if (!strcmp(t, "config")) {
    applyConfig(doc);
  } else if (!strcmp(t, "info")) {
    sendInfo();
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (rxLine.length()) handleLine(rxLine);
      rxLine = "";
    } else if (c != '\r' && rxLine.length() < MAX_LINE) {
      rxLine += c;
    }
  }
}

// ---------------------------------------------------------------- botao

void resolvePermission(const char *decision) {
  sendDecision(decision);
  perm.active = false;
  uiSetBrightness(BRIGHTNESS[brightnessIdx]);
  if (!strcmp(decision, "ask")) return;
  flashActive = true;
  flashAllowed = !strcmp(decision, "allow");
  flashUntil = millis() + RESULT_FLASH_MS;
}

void startListening() {
  String why;
  if (assistantListen(why)) {
    listening = true;
    answerPage = 0;
  } else {
    showToast(why);
  }
}

void handleButton() {
  auto &b = M5.BtnA;
  if (perm.active) {
    if (b.wasHold()) resolvePermission("deny");
    else if (b.wasSingleClicked()) resolvePermission("allow");
    else if (b.wasDoubleClicked()) resolvePermission("ask");
    return;
  }

  AssistantView v = assistantView();
  if (b.wasHold()) {
    if (v.phase == Phase::Speaking) {  // interrompe a fala e ja escuta de novo
      assistantCancel();
      listenAfterCancel = true;
    } else {
      startListening();
    }
  }
  if (listenAfterCancel && !assistantBusy()) {
    listenAfterCancel = false;
    if (b.isPressed()) startListening();
  }
  if (listening && b.wasReleased()) {
    listening = false;
    assistantRelease();
  }
  if (b.wasSingleClicked()) {
    if (v.phase == Phase::Reply) {
      if (++answerPage >= uiAnswerPages()) {
        answerPage = 0;
        assistantDismiss();
      }
    } else if (v.phase == Phase::Error) {
      assistantDismiss();
    } else if (v.phase == Phase::Speaking) {
      assistantCancel();
    } else {
      brightnessIdx = (brightnessIdx + 1) % sizeof(BRIGHTNESS);
      uiSetBrightness(BRIGHTNESS[brightnessIdx]);
    }
  }
  if (b.wasDoubleClicked() && !assistantBusy()) {
    assistantReset();
    showToast("Nova conversa");
  }
}

void tickTimers() {
  uint32_t now = millis();
  if (perm.active && now - perm.view.start > perm.view.ttlMs) {
    perm.active = false;
    uiSetBrightness(BRIGHTNESS[brightnessIdx]);
  }
  if (flashActive && (int32_t)(now - flashUntil) >= 0) flashActive = false;
  if (companion.state == CcState::Done && now - companion.since > DONE_TO_IDLE_MS)
    setCompanion(CcState::Idle, "", "Pronto pra codar", "");
  if (perm.active && !hostConnected()) perm.active = false;

  AssistantView v = assistantView();
  if (v.seq != answerSeq) {
    answerSeq = v.seq;
    answerPage = 0;
  }
  if (v.phase == Phase::Reply && now - v.since > REPLY_TO_IDLE_MS) assistantDismiss();
  if (v.phase == Phase::Error && now - v.since > ERROR_TO_IDLE_MS) assistantDismiss();
}

// ---------------------------------------------------------------- tela

void render() {
  uint32_t now = millis();
  if (now - lastFrameMs < FRAME_MS) return;
  lastFrameMs = now;

  uiClear();
  AssistantView v = assistantView();
  if (flashActive) {
    uiDrawFlash(flashAllowed);
  } else if (perm.active) {
    uiDrawPermission(perm.view);
  } else if (v.phase != Phase::Idle || !hostConnected()) {
    uiDrawAssistant(v, netInfo(), settings.hasKeys(), answerPage);
  } else {
    uiDrawCompanion(companion);
  }
  if (toastUntil && (int32_t)(now - toastUntil) < 0) uiDrawToast(toast);
  uiPush();
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.external_speaker.atomic_echo = true;  // Atomic Echo Base: microfone + alto-falante
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // nunca travar se ninguem estiver lendo a USB

  settingsLoad();
  uiBegin();
  uiSetBrightness(BRIGHTNESS[brightnessIdx]);
  M5.BtnA.setHoldThresh(450);
  M5.Speaker.end();

  M5.update();
  bool forcePortal = M5.BtnA.isPressed();
  assistantBegin();
  netBegin(forcePortal);
  setCompanion(CcState::Idle, "", "Pronto pra codar", "");
  sendHello("hello");
}

void loop() {
  M5.update();
  pollSerial();
  handleButton();
  netLoop();
  tickTimers();
  render();
  delay(2);
}
