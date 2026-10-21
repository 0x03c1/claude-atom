#pragma once
// Desenho na tela 128x128 (sempre num sprite, empurrado de uma vez por quadro).

#include <Arduino.h>

#include "assistant.h"
#include "net.h"

// Estado do Claude Code recebido pela USB (modo painel).
enum class CcState { Idle, Thinking, Tool, Waiting, Done, Error };

struct CompanionScreen {
  CcState state = CcState::Idle;
  String title;
  String msg;
  String project;
  uint32_t since = 0;
};

struct PermissionView {
  String tool;
  String msg;
  String project;
  uint32_t start = 0;
  uint32_t ttlMs = 0;
};

void uiBegin();
void uiSetBrightness(uint8_t level);
void uiClear();
void uiPush();

void uiDrawCompanion(const CompanionScreen &s);
void uiDrawPermission(const PermissionView &p);
void uiDrawFlash(bool allowed);
// page: pagina escolhida pelo usuario quando a resposta ja terminou.
void uiDrawAssistant(const AssistantView &v, const NetInfo &net, bool keysOk, int page);
void uiDrawToast(const String &text);

// Quantas paginas a resposta atual ocupa (para o clique avancar).
int uiAnswerPages();
