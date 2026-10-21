#include "assistant.h"

#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <freertos/stream_buffer.h>

#include <memory>
#include <vector>

#include "http_util.h"
#include "settings.h"

namespace {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t MIC_CHUNK = 512;                // amostras por buffer do microfone (32 ms)
constexpr size_t RING_BYTES = 48 * 1024;         // ~1,5 s de folga enquanto o TLS conecta
constexpr uint32_t MAX_LISTEN_MS = 30000;
constexpr uint32_t MIN_AUDIO_BYTES = SAMPLE_RATE * 2 * 3 / 10;  // 0,3 s
constexpr int MIC_GAIN = 4;
constexpr size_t SPK_SAMPLES = 2048;              // 128 ms por buffer de reproducao
constexpr size_t MAX_HISTORY = 10;                // mensagens (5 trocas)
constexpr size_t MAX_HISTORY_CHARS = 1500;

const char SYSTEM_PROMPT[] =
    "Voce e o Claude, falando por um pequeno aparelho de voz (M5Stack AtomS3 com alto-falante) "
    "que mostra o texto numa tela de 128x128 pixels. Sua resposta sera lida em voz alta: escreva "
    "texto corrido, sem markdown, listas, titulos, emojis ou blocos de codigo. Seja breve: uma a "
    "tres frases, a menos que peçam detalhes. Responda no idioma em que falarem com voce; o padrao "
    "e portugues do Brasil. A pergunta vem de reconhecimento de voz e pode ter erros de transcricao: "
    "interprete com bom senso.";

struct Turn {
  const char *role;
  String text;
};

SemaphoreHandle_t mtx;
AssistantView view;
std::vector<Turn> history;
volatile bool busy = false;
volatile bool capturing = false;
volatile bool cancelRequested = false;
uint32_t listenStart = 0;
StreamBufferHandle_t ring = nullptr;
int16_t micBufs[2][MIC_CHUNK];

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

void setPhase(Phase p) {
  Lock l;
  view.phase = p;
  view.since = millis();
}

void fail(const String &msg) {
  Lock l;
  view.error = msg;
  view.phase = Phase::Error;
  view.since = millis();
  log_w("assistente: %s", msg.c_str());
}

String apiError(const String &body, int status) {
  JsonDocument doc;
  if (!deserializeJson(doc, body)) {
    const char *m = doc["error"]["message"] | (const char *)nullptr;
    if (m) return String(m);
  }
  return "HTTP " + String(status);
}

// ------------------------------------------------------------ microfone

// Chamado pela tarefa de captura do M5Unified a cada buffer cheio:
// aplica ganho, mede o nivel, joga no ring e ja reenfileira o mesmo buffer.
void onMicBuffer(void *, void *data, size_t length) {
  auto *s = static_cast<int16_t *>(data);
  int32_t peak = 0;
  for (size_t i = 0; i < length; i++) {
    int32_t v = s[i] * MIC_GAIN;
    v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
    s[i] = v;
    peak = max(peak, abs(v));
  }
  view.level = view.level * 0.6f + (peak / 32768.0f) * 0.4f;  // so leitura no UI
  if (ring) xStreamBufferSend(ring, s, length * sizeof(int16_t), 0);
  if (capturing) M5.Mic.record(s, length);
}

bool startMic() {
  M5.Speaker.end();
  if (!M5.Mic.begin()) return false;
  xStreamBufferReset(ring);
  capturing = true;
  listenStart = millis();
  return M5.Mic.record(micBufs[0], MIC_CHUNK, SAMPLE_RATE) && M5.Mic.record(micBufs[1], MIC_CHUNK, SAMPLE_RATE);
}

// ------------------------------------------------------------ Google STT

bool transcribe(String &out, String &err) {
  std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  if (!httpsConnect(*client, "speech.googleapis.com")) {
    capturing = false;
    M5.Mic.end();
    err = "Sem conexao com o Google";
    return false;
  }
  client->print("POST /v1/speech:recognize?key=" + settings.googleKey +
                " HTTP/1.1\r\nHost: speech.googleapis.com\r\nContent-Type: application/json\r\n"
                "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  writeChunk(*client, "{\"config\":{\"encoding\":\"LINEAR16\",\"sampleRateHertz\":16000,\"languageCode\":\"" +
                          settings.language + "\",\"enableAutomaticPunctuation\":true},\"audio\":{\"content\":\"");

  // Envia o audio em base64 enquanto o usuario ainda fala.
  Base64Encoder enc;
  static uint8_t raw[1536];
  String b64;
  size_t total = 0;
  bool micEnded = false;
  uint32_t stoppedAt = 0;
  while (true) {
    if (capturing && millis() - listenStart > MAX_LISTEN_MS) capturing = false;
    if (!capturing && !micEnded) {
      if (!stoppedAt) stoppedAt = millis();
      if (millis() - stoppedAt > 120) {  // deixa os ultimos buffers terminarem
        M5.Mic.end();
        micEnded = true;
      }
    }
    size_t n = xStreamBufferReceive(ring, raw, sizeof(raw), pdMS_TO_TICKS(40));
    if (n) {
      b64 = "";
      enc.feed(raw, n, b64);
      if (!writeChunk(*client, b64)) {
        capturing = false;
        M5.Mic.end();
        err = "Conexao caiu enviando o audio";
        return false;
      }
      total += n;
    } else if (micEnded && xStreamBufferIsEmpty(ring)) {
      break;
    }
  }
  b64 = "";
  enc.finish(b64);
  writeChunk(*client, b64);
  writeChunk(*client, "\"}}");
  endChunks(*client);

  if (total < MIN_AUDIO_BYTES) {
    err = "Fala muito curta";
    return false;
  }
  setPhase(Phase::Transcribing);
  HttpResponse r(*client);
  int status = r.begin(20000);
  String body = r.readAll(8192);
  if (status != 200) {
    err = "Google STT: " + apiError(body, status);
    return false;
  }
  JsonDocument doc;
  deserializeJson(doc, body);
  out = "";
  for (JsonVariant res : doc["results"].as<JsonArray>()) out += res["alternatives"][0]["transcript"].as<String>();
  out.trim();
  return true;
}

// ------------------------------------------------------------ Claude

bool askClaude(const String &question, String &err) {
  JsonDocument body;
  body["model"] = settings.model;
  body["max_tokens"] = 1024;
  body["stream"] = true;
  body["system"] = SYSTEM_PROMPT;
  body["output_config"]["effort"] = "low";
  // Haiku nao tem fallback no servidor; Opus e Sonnet tem.
  bool fallbacks = settings.model.startsWith("claude-opus") || settings.model.startsWith("claude-sonnet");
  if (fallbacks) body["fallbacks"] = "default";
  JsonArray msgs = body["messages"].to<JsonArray>();
  {
    Lock l;
    for (auto &t : history) {
      JsonObject m = msgs.add<JsonObject>();
      m["role"] = t.role;
      m["content"] = t.text;
    }
  }
  JsonObject user = msgs.add<JsonObject>();
  user["role"] = "user";
  user["content"] = question;
  String payload;
  serializeJson(body, payload);
  body.clear();

  std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  if (!httpsConnect(*client, "api.anthropic.com")) {
    err = "Sem conexao com a API do Claude";
    return false;
  }
  String head = "POST /v1/messages HTTP/1.1\r\nHost: api.anthropic.com\r\nContent-Type: application/json\r\n"
                "anthropic-version: 2023-06-01\r\nx-api-key: " + settings.anthropicKey + "\r\n";
  if (fallbacks) head += "anthropic-beta: server-side-fallback-2026-07-01\r\n";
  head += "Content-Length: " + String(payload.length()) + "\r\nConnection: close\r\n\r\n";
  client->print(head);
  client->print(payload);
  payload = "";

  HttpResponse r(*client);
  int status = r.begin(60000);
  if (status != 200) {
    err = "Claude: " + apiError(r.readAll(4096), status);
    return false;
  }

  JsonDocument filter;
  filter["type"] = true;
  filter["delta"]["type"] = true;
  filter["delta"]["text"] = true;
  filter["delta"]["stop_reason"] = true;
  filter["content_block"]["type"] = true;
  filter["error"]["message"] = true;

  String line, stopReason;
  while (r.readLine(line, 8192)) {
    if (!line.startsWith("data:")) continue;
    JsonDocument ev;
    if (deserializeJson(ev, line.c_str() + 5, DeserializationOption::Filter(filter))) continue;
    const char *type = ev["type"] | "";
    if (!strcmp(type, "content_block_delta") && !strcmp(ev["delta"]["type"] | "", "text_delta")) {
      Lock l;
      view.answer += ev["delta"]["text"].as<const char *>();
    } else if (!strcmp(type, "content_block_start") && !strcmp(ev["content_block"]["type"] | "", "fallback")) {
      Lock l;  // o modelo original recusou no meio: descarta o parcial
      view.answer = "";
    } else if (!strcmp(type, "message_delta")) {
      stopReason = ev["delta"]["stop_reason"] | "";
    } else if (!strcmp(type, "error")) {
      err = String("Claude: ") + (ev["error"]["message"] | "erro no stream");
      return false;
    } else if (!strcmp(type, "message_stop")) {
      break;
    }
  }

  Lock l;
  if (stopReason == "refusal") view.answer = "Desculpe, nao posso ajudar com isso.";
  view.answer.trim();
  if (!view.answer.length()) {
    err = "Resposta vazia do Claude";
    return false;
  }
  history.push_back({"user", question.substring(0, MAX_HISTORY_CHARS)});
  history.push_back({"assistant", view.answer.substring(0, MAX_HISTORY_CHARS)});
  while (history.size() > MAX_HISTORY) history.erase(history.begin(), history.begin() + 2);
  view.turns = history.size();
  return true;
}

// ------------------------------------------------------------ Google TTS

bool speak(const String &text, String &err) {
  JsonDocument body;
  body["input"]["text"] = text.substring(0, 4500);
  body["voice"]["languageCode"] = settings.language;
  if (settings.voice.length()) body["voice"]["name"] = settings.voice;
  body["audioConfig"]["audioEncoding"] = "LINEAR16";
  body["audioConfig"]["sampleRateHertz"] = SAMPLE_RATE;
  String payload;
  serializeJson(body, payload);

  std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  if (!httpsConnect(*client, "texttospeech.googleapis.com")) {
    err = "Sem conexao com o Google TTS";
    return false;
  }
  client->print("POST /v1/text:synthesize?key=" + settings.googleKey +
                " HTTP/1.1\r\nHost: texttospeech.googleapis.com\r\nContent-Type: application/json\r\n"
                "Content-Length: " + String(payload.length()) + "\r\nConnection: close\r\n\r\n");
  client->print(payload);

  HttpResponse r(*client);
  int status = r.begin(20000);
  if (status != 200) {
    err = "Google TTS: " + apiError(r.readAll(4096), status);
    return false;
  }

  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(settings.volume);

  // Procura "audioContent": "<base64>" no corpo e decodifica direto para o alto-falante.
  static int16_t spk[3][SPK_SAMPLES];
  static const char KEY[] = "\"audioContent\"";
  size_t keyPos = 0;
  enum { SeekKey, SeekQuote, Data, Done } st = SeekKey;
  Base64Decoder dec;
  size_t decoded = 0, fill = 0;
  int bufIdx = 0;
  uint8_t pending = 0;
  bool havePending = false;
  float expected = r.contentLength() > 0 ? (r.contentLength() - 40) * 0.75f : 0;
  uint8_t chunk[512];

  auto flush = [&](size_t samples) {
    while (M5.Speaker.isPlaying(0) >= 2 && !cancelRequested) delay(2);
    if (cancelRequested) return;
    M5.Speaker.playRaw(spk[bufIdx], samples, SAMPLE_RATE, false, 1, 0);
    bufIdx = (bufIdx + 1) % 3;
    fill = 0;
  };

  int n;
  while (st != Done && !cancelRequested && (n = r.read(chunk, sizeof(chunk))) > 0) {
    for (int i = 0; i < n && st != Done; i++) {
      char c = chunk[i];
      if (st == SeekKey) {
        keyPos = (c == KEY[keyPos]) ? keyPos + 1 : (c == KEY[0] ? 1 : 0);
        if (keyPos == sizeof(KEY) - 1) st = SeekQuote;
      } else if (st == SeekQuote) {
        if (c == '"') st = Data;
      } else if (c == '"') {
        st = Done;
      } else {
        uint8_t b;
        if (!dec.feed(c, &b)) continue;
        if (decoded++ < 44) continue;  // cabecalho WAV
        if (!havePending) {
          pending = b;
          havePending = true;
          continue;
        }
        havePending = false;
        spk[bufIdx][fill++] = (int16_t)(pending | (b << 8));
        if (fill == SPK_SAMPLES) {
          flush(SPK_SAMPLES);
          if (expected > 0) view.progress = min(1.0f, decoded / expected);
        }
      }
    }
  }
  if (fill && !cancelRequested) flush(fill);
  while (M5.Speaker.isPlaying() && !cancelRequested) delay(10);
  M5.Speaker.stop();
  M5.Speaker.end();
  view.progress = 1;
  return true;
}

// ------------------------------------------------------------ tarefa

struct Job {
  bool voice;
  String text;
};

void runJob(Job &job) {
  String question = job.text, err;
  if (job.voice) {
    if (!transcribe(question, err)) return fail(err);
    if (!question.length()) return fail("Nao entendi. Tente de novo.");
  }
  {
    Lock l;
    view.question = question;
    view.answer = "";
    view.error = "";
    view.seq++;
  }
  setPhase(Phase::Thinking);
  if (!askClaude(question, err)) return fail(err);
  if (settings.speak && !cancelRequested) {
    setPhase(Phase::Speaking);
    view.progress = 0;
    if (!speak(view.answer, err)) log_w("tts: %s", err.c_str());  // o texto ja esta na tela
  }
  setPhase(Phase::Reply);
}

void jobTask(void *arg) {
  auto *job = static_cast<Job *>(arg);
  runJob(*job);
  delete job;
  log_i("heap livre: %u, minimo: %u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  busy = false;
  vTaskDelete(nullptr);
}

bool ready(String &whyNot) {
  if (busy) whyNot = "Ocupado";
  else if (!settings.hasKeys()) whyNot = "Configure as chaves";
  else if (WiFi.status() != WL_CONNECTED) whyNot = "Sem Wi-Fi";
  else return true;
  return false;
}

bool spawn(bool voice, const String &text) {
  busy = true;
  cancelRequested = false;
  auto *job = new Job{voice, text};
  // Nucleo 0 (junto do Wi-Fi); o loop/tela fica livre no nucleo 1.
  if (xTaskCreatePinnedToCore(jobTask, "assistant", 16384, job, 2, nullptr, 0) != pdPASS) {
    delete job;
    busy = false;
    return false;
  }
  return true;
}

}  // namespace

void assistantBegin() {
  mtx = xSemaphoreCreateMutex();
  ring = xStreamBufferCreate(RING_BYTES, 1);
  M5.Mic.setBufferReleaseCallback(nullptr, onMicBuffer);
}

AssistantView assistantView() {
  Lock l;
  return view;
}

bool assistantBusy() { return busy; }

bool assistantListen(String &whyNot) {
  if (!ready(whyNot)) return false;
  if (!startMic()) {
    whyNot = "Microfone nao encontrado";
    return false;
  }
  {
    Lock l;
    view.error = "";
    view.level = 0;
  }
  setPhase(Phase::Listening);
  if (!spawn(true, "")) {
    capturing = false;
    M5.Mic.end();
    whyNot = "Sem memoria";
    return false;
  }
  return true;
}

void assistantRelease() { capturing = false; }

bool assistantAsk(const String &text, String &whyNot) {
  if (!ready(whyNot)) return false;
  return spawn(false, text) || (whyNot = "Sem memoria", false);
}

void assistantCancel() {
  cancelRequested = true;
  capturing = false;
}

void assistantDismiss() {
  if (busy) return;
  setPhase(Phase::Idle);
}

void assistantReset() {
  if (busy) return;
  Lock l;
  history.clear();
  view = AssistantView{};
  view.since = millis();
}
