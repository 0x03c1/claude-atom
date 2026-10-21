#pragma once
// Configuracao persistente (NVS): Wi-Fi, chaves de API e preferencias de voz.

#include <Arduino.h>

struct Settings {
  String ssid;
  String pass;
  String anthropicKey;
  String googleKey;
  String model = "claude-opus-5-5";
  String voice;          // vazio = voz padrao do Google para pt-BR
  String language = "pt-BR";
  uint8_t volume = 180;  // 0-255
  bool speak = true;     // falar as respostas no alto-falante

  bool hasWifi() const { return ssid.length() > 0; }
  bool hasKeys() const { return anthropicKey.length() > 0 && googleKey.length() > 0; }
};

extern Settings settings;

void settingsLoad();
void settingsSave();
