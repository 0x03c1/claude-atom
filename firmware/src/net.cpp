#include "net.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "assistant.h"
#include "settings.h"
#include "web_pages.h"

namespace {

constexpr const char *HOSTNAME = "claude-atom";
constexpr uint32_t CONNECT_TIMEOUT_MS = 25000;
constexpr uint32_t PORTAL_RETRY_MS = 30000;
const char *MODELS[][2] = {
    {"claude-opus-5-5", "Claude Opus 5.5 (padrao)"},
    {"claude-sonnet-5-5", "Claude Sonnet 5.5"},
    {"claude-haiku-5-5", "Claude Haiku 5.5 (mais rapido e barato)"},
};

WebServer server(80);
DNSServer dns;
NetInfo info;
uint32_t connectStart = 0;
uint32_t lastPortalRetry = 0;
uint32_t rebootAt = 0;
String scannedNets;
bool mdnsStarted = false;

String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length());
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

const char *phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Listening: return "listening";
    case Phase::Transcribing: return "transcribing";
    case Phase::Thinking: return "thinking";
    case Phase::Speaking: return "speaking";
    case Phase::Reply: return "reply";
    case Phase::Error: return "error";
  }
  return "idle";
}

void sendConfigPage() {
  String page = FPSTR(CONFIG_HTML);
  String models;
  for (auto &m : MODELS) {
    models += "<option value=\"" + String(m[0]) + "\"" + (settings.model == m[0] ? " selected" : "") + ">" + m[1] +
              "</option>";
  }
  page.replace("%SSID%", htmlEscape(settings.ssid));
  page.replace("%NETS%", scannedNets);
  page.replace("%AKEY%", settings.anthropicKey.length() ? "salva &check;" : "obrigatoria");
  page.replace("%GKEY%", settings.googleKey.length() ? "salva &check;" : "obrigatoria");
  page.replace("%MODELS%", models);
  page.replace("%VOICE%", htmlEscape(settings.voice));
  page.replace("%LANG%", htmlEscape(settings.language));
  page.replace("%VOLUME%", String(settings.volume));
  page.replace("%SPEAK%", settings.speak ? "checked" : "");
  server.send(200, "text/html; charset=utf-8", page);
}

void handleConfigPost() {
  auto keep = [](const char *field, String &target) {
    String v = server.arg(field);
    v.trim();
    if (v.length()) target = v;
  };
  keep("ssid", settings.ssid);
  keep("pass", settings.pass);
  keep("anthropic", settings.anthropicKey);
  keep("google", settings.googleKey);
  keep("lang", settings.language);
  String model = server.arg("model");
  for (auto &m : MODELS)
    if (model == m[0]) settings.model = model;
  settings.voice = server.arg("voice");
  settings.voice.trim();
  if (server.hasArg("volume")) settings.volume = constrain(server.arg("volume").toInt(), 0, 255);
  settings.speak = server.hasArg("speak");
  settingsSave();
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><meta name=viewport content='width=device-width'>"
              "<body style='font:18px system-ui;padding:24px'>Salvo! O Claude Atom vai reiniciar e conectar em <b>" +
                  htmlEscape(settings.ssid) + "</b>.<br><br>Depois acesse <b>http://claude-atom.local</b></body>");
  rebootAt = millis() + 1500;
}

void handleState() {
  AssistantView v = assistantView();
  JsonDocument doc;
  doc["phase"] = phaseName(v.phase);
  doc["q"] = v.question;
  doc["a"] = v.answer;
  doc["err"] = v.error;
  doc["seq"] = v.seq;
  doc["since"] = v.since;
  doc["turns"] = v.turns;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAsk() {
  String text = server.arg("plain");
  text.trim();
  if (!text.length()) return server.send(400, "text/plain; charset=utf-8", "Pergunta vazia");
  String why;
  if (!assistantAsk(text.substring(0, 2000), why)) return server.send(409, "text/plain; charset=utf-8", why);
  server.send(202, "text/plain", "ok");
}

void setupRoutes(bool portal) {
  server.on("/", HTTP_GET, [portal] {
    if (portal) sendConfigPage();
    else server.send_P(200, "text/html; charset=utf-8", CHAT_HTML);
  });
  server.on("/config", HTTP_GET, sendConfigPage);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/ask", HTTP_POST, handleAsk);
  server.on("/api/reset", HTTP_POST, [] {
    assistantReset();
    server.send(200, "text/plain", "ok");
  });
  server.onNotFound([portal] {
    if (portal) {  // portal cativo: qualquer URL leva a configuracao
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "nao encontrado");
    }
  });
  server.begin();
}

void scanNetworks() {
  int n = WiFi.scanNetworks();
  scannedNets = "";
  for (int i = 0; i < n; i++) scannedNets += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
}

void startPortal() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32], pass[12];
  snprintf(name, sizeof(name), "Claude-Atom-%02X%02X", mac[4], mac[5]);
  snprintf(pass, sizeof(pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
  info.apName = name;
  info.apPass = pass;
  info.state = NetState::Portal;

  WiFi.mode(WIFI_AP_STA);
  scanNetworks();
  WiFi.softAP(name, pass);
  dns.start(53, "*", WiFi.softAPIP());
  setupRoutes(true);
  lastPortalRetry = millis();
  log_i("portal %s senha %s", name, pass);
}

void startStation() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
  info.ssid = settings.ssid;
  info.state = NetState::Connecting;
  connectStart = millis();
}

void onOnline() {
  info.state = NetState::Online;
  info.ip = WiFi.localIP().toString();
  if (!mdnsStarted && MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
  log_i("online em %s (%s)", info.ssid.c_str(), info.ip.c_str());
}

}  // namespace

void netBegin(bool forcePortal) {
  if (forcePortal || !settings.hasWifi()) {
    startPortal();
    return;
  }
  startStation();
  setupRoutes(false);
}

void netLoop() {
  if (rebootAt && millis() > rebootAt) ESP.restart();
  server.handleClient();

  bool connected = WiFi.status() == WL_CONNECTED;
  switch (info.state) {
    case NetState::Portal:
      dns.processNextRequest();
      // Continua tentando a rede salva quando ninguem esta usando o portal.
      if (settings.hasWifi() && WiFi.softAPgetStationNum() == 0 && millis() - lastPortalRetry > PORTAL_RETRY_MS) {
        lastPortalRetry = millis();
        WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
      }
      if (connected && WiFi.softAPgetStationNum() == 0) ESP.restart();  // volta ao modo normal
      break;
    case NetState::Connecting:
      if (connected) onOnline();
      else if (millis() - connectStart > CONNECT_TIMEOUT_MS) {
        server.stop();
        startPortal();
      }
      break;
    case NetState::Online:
      if (!connected) info.state = NetState::Offline;
      break;
    case NetState::Offline:
      if (connected) onOnline();
      break;
  }
}

NetInfo netInfo() { return info; }
