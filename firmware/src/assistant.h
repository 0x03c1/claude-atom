#pragma once
// Assistente de voz: microfone -> Google Speech-to-Text -> Claude -> Google Text-to-Speech.
// Todo o trabalho de rede roda numa tarefa FreeRTOS; o loop principal so le o estado.

#include <Arduino.h>

enum class Phase { Idle, Listening, Transcribing, Thinking, Speaking, Reply, Error };

struct AssistantView {
  Phase phase = Phase::Idle;
  String question;
  String answer;
  String error;
  float level = 0;     // nivel do microfone (0..1) enquanto ouve
  float progress = 0;  // progresso da fala (0..1), quando conhecido
  uint32_t seq = 0;    // muda a cada nova pergunta/resposta
  uint32_t since = 0;  // millis() da ultima troca de fase
  int turns = 0;       // mensagens no historico
};

void assistantBegin();
AssistantView assistantView();
bool assistantBusy();

// Botao: comeca a ouvir ao segurar, envia ao soltar.
bool assistantListen(String &whyNot);
void assistantRelease();
// Pergunta digitada (pagina web).
bool assistantAsk(const String &text, String &whyNot);
// Interrompe a fala em andamento.
void assistantCancel();
void assistantDismiss();
void assistantReset();
