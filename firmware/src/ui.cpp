#include "ui.h"

#include <M5Unified.h>

#include <vector>

namespace {

constexpr uint32_t COL_CLAUDE = 0xD97757;
constexpr uint32_t COL_BG = 0x141413;
constexpr uint32_t COL_TEXT = 0xF0EEE6;
constexpr uint32_t COL_DIM = 0x8A8780;
constexpr uint32_t COL_OK = 0x7BC47F;
constexpr uint32_t COL_WARN = 0xF2B84B;
constexpr uint32_t COL_ERR = 0xE5534B;
constexpr uint32_t COL_INFO = 0x6A9BCC;
constexpr uint32_t COL_BLACK = 0x000000;

// efont tem os acentos do portugues (Latin-1); as fontes GFX so tem ASCII.
const lgfx::IFont *BODY_FONT = &fonts::efontCN_12;
const lgfx::IFont *SMALL_FONT = &fonts::efontCN_10;
const lgfx::IFont *TITLE_FONT = &fonts::efontCN_14_b;
constexpr int BODY_LINE_H = 14;
constexpr int BODY_LINES = 7;  // y 16..114

M5Canvas canvas(&M5.Display);

// Cache das linhas da resposta (re-quebra so quando o texto muda).
std::vector<String> answerLines;
String answerCache;

size_t utf8Next(const String &s, size_t i) {
  i++;
  while (i < s.length() && (s[i] & 0xC0) == 0x80) i++;
  return i;
}

// Quebra por palavras respeitando UTF-8; palavras maiores que a linha sao cortadas.
std::vector<String> wrap(const String &text, int w) {
  std::vector<String> lines;
  size_t pos = 0, n = text.length();
  while (pos < n) {
    size_t end = pos, lastSpace = 0;
    bool hasSpace = false;
    while (end < n && text[end] != '\n') {
      size_t next = utf8Next(text, end);
      if (canvas.textWidth(text.substring(pos, next)) > w) break;
      if (text[end] == ' ') {
        lastSpace = end;
        hasSpace = true;
      }
      end = next;
    }
    if (end == pos && end < n && text[end] != '\n') end = utf8Next(text, end);
    size_t next = end;
    if (end < n && text[end] == '\n') {
      next = end + 1;
    } else if (end < n && hasSpace && lastSpace > pos) {
      end = lastSpace;
      next = lastSpace + 1;
    }
    lines.push_back(text.substring(pos, end));
    pos = next;
    while (pos < n && text[pos] == ' ') pos++;
  }
  return lines;
}

void drawLines(const std::vector<String> &lines, size_t first, int x, int y, int lineH, int maxLines,
               uint32_t color, bool ellipsis) {
  canvas.setTextColor(color);
  canvas.setTextDatum(top_left);
  for (int i = 0; i < maxLines && first + i < lines.size(); i++) {
    String line = lines[first + i];
    if (ellipsis && i == maxLines - 1 && first + i + 1 < lines.size()) line += "...";
    canvas.drawString(line, x, y + i * lineH);
  }
}

void drawWrapped(const String &text, int x, int y, int w, int lineH, int maxLines, uint32_t color) {
  drawLines(wrap(text, w), 0, x, y, lineH, maxLines, color, true);
}

String fit(const String &text, int w) {
  if (canvas.textWidth(text) <= w) return text;
  String t = text;
  while (t.length() > 1 && canvas.textWidth(t + "...") > w) t.remove(t.length() - 1);
  while (t.length() && (t[t.length() - 1] & 0xC0) == 0x80) t.remove(t.length() - 1);  // nao cortar no meio de um char
  if (t.length() && (uint8_t)t[t.length() - 1] >= 0xC0) t.remove(t.length() - 1);
  return t + "...";
}

// A "faisca" do Claude: raios afinando do centro para a ponta.
void drawSpark(float cx, float cy, float radius, float rotation, float wobble, uint32_t color) {
  constexpr int RAYS = 12;
  for (int i = 0; i < RAYS; i++) {
    float a = rotation + i * (2 * PI / RAYS);
    float len = radius * (0.72f + 0.28f * sinf(wobble + i * 1.7f));
    float ix = cx + cosf(a) * radius * 0.18f;
    float iy = cy + sinf(a) * radius * 0.18f;
    canvas.drawWedgeLine(ix, iy, cx + cosf(a) * len, cy + sinf(a) * len, radius * 0.11f, radius * 0.035f, color);
  }
  canvas.fillSmoothCircle(cx, cy, radius * 0.16f, color);
}

void drawTopLabel(const String &text, uint32_t color = COL_DIM) {
  if (!text.length()) return;
  canvas.setFont(SMALL_FONT);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(color);
  canvas.drawString(fit(text, 124), 2, 1);
}

void drawTitle(const String &title, uint32_t color, int y = 68) {
  canvas.setFont(TITLE_FONT);
  canvas.setTextDatum(top_center);
  canvas.setTextColor(color);
  canvas.drawString(fit(title, 124), 64, y);
}

void drawMsg(const String &msg, uint32_t color = COL_DIM, int y = 88, int lines = 3) {
  canvas.setFont(SMALL_FONT);
  drawWrapped(msg, 3, y, 122, 12, lines, color);
}

}  // namespace

void uiBegin() {
  M5.Display.setRotation(0);
  canvas.setColorDepth(16);
  canvas.createSprite(128, 128);
}

void uiSetBrightness(uint8_t level) { M5.Display.setBrightness(level); }
void uiClear() { canvas.fillScreen(COL_BG); }
void uiPush() { canvas.pushSprite(0, 0); }

// ---------------------------------------------------------------- modo painel

void uiDrawCompanion(const CompanionScreen &s) {
  float t = millis() / 1000.0f;
  uint32_t color = COL_CLAUDE;
  float rot = 0, wobble = t * 0.8f, radius = 24;
  switch (s.state) {
    case CcState::Idle:
      radius = 22 + 2 * sinf(t * 1.6f);
      rot = t * 0.15f;
      break;
    case CcState::Thinking:
      rot = t * 2.2f;
      wobble = t * 4.0f;
      break;
    case CcState::Tool:
      rot = t * 1.1f;
      wobble = t * 6.0f;
      break;
    case CcState::Waiting:
      color = COL_INFO;
      radius = 21 + 4 * fabsf(sinf(t * 2.5f));
      break;
    case CcState::Done:
      color = COL_OK;
      break;
    case CcState::Error:
      color = COL_ERR;
      wobble = t * 9.0f;
      break;
  }
  drawTopLabel(s.project);
  drawSpark(64, 40, radius, rot, wobble, color);
  drawTitle(s.title, s.state == CcState::Tool ? COL_CLAUDE : COL_TEXT);
  drawMsg(s.msg);
}

void uiDrawPermission(const PermissionView &p) {
  uint32_t now = millis();
  uint32_t frame = (sinf(now / 1000.0f * 6) > 0) ? COL_WARN : COL_CLAUDE;
  canvas.drawRect(0, 0, 128, 128, frame);
  canvas.drawRect(1, 1, 126, 126, frame);

  canvas.fillRect(0, 0, 128, 16, COL_WARN);
  canvas.setFont(SMALL_FONT);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(COL_BLACK);
  canvas.drawString(p.project.length() ? fit("PERMITIR? " + p.project, 124) : String("PERMITIR?"), 64, 8);

  canvas.setFont(TITLE_FONT);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(COL_CLAUDE);
  canvas.drawString(fit(p.tool, 120), 4, 19);

  canvas.setFont(SMALL_FONT);
  drawWrapped(p.msg, 4, 37, 120, 11, 6, COL_TEXT);

  canvas.setTextDatum(top_left);
  canvas.setTextColor(COL_OK);
  canvas.drawString("1x SIM", 4, 103);
  canvas.setTextColor(COL_ERR);
  canvas.setTextDatum(top_right);
  canvas.drawString("segure NÃO", 124, 103);
  canvas.setTextColor(COL_DIM);
  canvas.setTextDatum(top_center);
  canvas.drawString("2x: no terminal", 64, 113);

  float left = 1.0f - float(now - p.start) / float(p.ttlMs);
  if (left < 0) left = 0;
  canvas.fillRect(2, 125, int(124 * left), 2, COL_WARN);
}

void uiDrawFlash(bool allowed) {
  canvas.fillScreen(allowed ? COL_OK : COL_ERR);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(COL_BLACK);
  canvas.drawString(allowed ? "SIM" : "NAO", 64, 58);
  canvas.setFont(SMALL_FONT);
  canvas.drawString(allowed ? "permitido" : "negado", 64, 88);
}

// ---------------------------------------------------------------- assistente

int uiAnswerPages() {
  if (answerLines.size() <= (size_t)BODY_LINES) return 1;
  return (answerLines.size() + BODY_LINES - 1) / BODY_LINES;
}

static void drawAnswer(const AssistantView &v, int page) {
  canvas.setFont(BODY_FONT);
  if (v.answer != answerCache) {
    answerCache = v.answer;
    answerLines = wrap(v.answer, 122);
  }
  // cabecalho
  canvas.fillRect(0, 0, 128, 14, 0x262624);
  canvas.setFont(SMALL_FONT);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(COL_CLAUDE);
  canvas.drawString("Claude", 3, 1);
  canvas.setTextDatum(top_right);
  canvas.setTextColor(COL_DIM);
  const char *tag = v.phase == Phase::Speaking ? "falando" : v.phase == Phase::Thinking ? "escrevendo" : "";
  canvas.drawString(tag, 125, 1);

  size_t total = answerLines.size();
  size_t first = 0;
  if (total > (size_t)BODY_LINES) {
    size_t maxFirst = total - BODY_LINES;
    if (v.phase == Phase::Thinking) first = maxFirst;  // acompanha o texto chegando
    else if (v.phase == Phase::Speaking) first = (size_t)(v.progress * maxFirst + 0.5f);
    else first = min((size_t)page * BODY_LINES, maxFirst);
  }
  canvas.setFont(BODY_FONT);
  drawLines(answerLines, first, 3, 17, BODY_LINE_H, BODY_LINES, COL_TEXT, false);

  // barra de rolagem
  if (total > (size_t)BODY_LINES) {
    int barH = max(8, (int)(108 * BODY_LINES / total));
    int barY = 16 + (int)((108 - barH) * first / (total - BODY_LINES));
    canvas.fillRect(126, barY, 2, barH, COL_DIM);
  }
  if (v.phase == Phase::Reply) {
    canvas.setFont(SMALL_FONT);
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(COL_DIM);
    canvas.drawString(uiAnswerPages() > 1 ? "clique: rolar  segure: falar" : "segure para falar", 64, 128);
  }
}

void uiDrawAssistant(const AssistantView &v, const NetInfo &net, bool keysOk, int page) {
  float t = millis() / 1000.0f;

  if (net.state == NetState::Portal) {
    drawTopLabel("Configuração");
    drawSpark(64, 34, 20, t * 0.2f, t, COL_DIM);
    drawTitle("Configurar", COL_TEXT, 60);
    canvas.setFont(SMALL_FONT);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(COL_DIM);
    canvas.drawString("Wi-Fi:", 3, 80);
    canvas.drawString("Senha:", 3, 93);
    canvas.drawString("Abra:", 3, 106);
    canvas.setTextColor(COL_TEXT);
    canvas.drawString(fit(net.apName, 84), 40, 80);
    canvas.drawString(net.apPass, 40, 93);
    canvas.drawString("192.168.4.1", 40, 106);
    return;
  }

  bool showAnswer = v.answer.length() &&
                    (v.phase == Phase::Thinking || v.phase == Phase::Speaking || v.phase == Phase::Reply);
  if (showAnswer) return drawAnswer(v, page);

  switch (v.phase) {
    case Phase::Listening: {
      float r = 16 + v.level * 40;
      drawTopLabel("Ouvindo", COL_INFO);
      drawSpark(64, 42, min(r, 30.0f), t * 1.5f, t * 8, COL_INFO);
      drawTitle("Ouvindo...", COL_TEXT);
      uint32_t secs = (millis() - v.since) / 1000;
      drawMsg("Solte para enviar - " + String(secs) + "s");
      break;
    }
    case Phase::Transcribing:
      drawSpark(64, 42, 24, t * 3, t * 5, COL_INFO);
      drawTitle("Entendendo...", COL_TEXT);
      break;
    case Phase::Thinking:
      drawSpark(64, 40, 24, t * 2.2f, t * 4, COL_CLAUDE);
      drawTitle("Pensando", COL_TEXT);
      drawMsg("\"" + v.question + "\"");
      break;
    case Phase::Error:
      drawTopLabel("Erro", COL_ERR);
      drawSpark(64, 36, 20, 0, t * 9, COL_ERR);
      drawTitle("Ops", COL_TEXT, 60);
      drawMsg(v.error, COL_TEXT, 80, 4);
      break;
    default: {  // Idle (ou Reply/Speaking sem texto)
      String top;
      if (net.state == NetState::Online) top = "Wi-Fi: " + net.ssid;
      else if (net.state == NetState::Connecting) top = "Conectando ao Wi-Fi...";
      else top = "Sem Wi-Fi";
      drawTopLabel(top, net.state == NetState::Online ? COL_DIM : COL_WARN);
      drawSpark(64, 42, 22 + 2 * sinf(t * 1.6f), t * 0.15f, t * 0.8f, COL_CLAUDE);
      drawTitle("Claude", COL_TEXT, 70);
      String msg = !keysOk                          ? "Configure em claude-atom.local"
                   : net.state != NetState::Online ? "Aguardando a rede..."
                                                    : "Segure o botão para falar";
      drawMsg(msg, COL_DIM, 90, 2);
      if (net.state == NetState::Online) {
        canvas.setFont(SMALL_FONT);
        canvas.setTextDatum(bottom_center);
        canvas.setTextColor(0x5A5955);
        canvas.drawString(net.ip, 64, 128);
      }
    }
  }
}

void uiDrawToast(const String &text) {
  canvas.setFont(SMALL_FONT);
  int w = min(124, (int)canvas.textWidth(text) + 12);
  canvas.fillRoundRect(64 - w / 2, 100, w, 20, 6, 0x3A3936);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(COL_TEXT);
  canvas.drawString(fit(text, 116), 64, 110);
}
