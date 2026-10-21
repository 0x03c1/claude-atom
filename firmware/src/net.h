#pragma once
// Wi-Fi, portal de configuracao (modo AP) e servidor web (chat + /config + API).

#include <Arduino.h>

enum class NetState { Connecting, Online, Offline, Portal };

struct NetInfo {
  NetState state = NetState::Connecting;
  String ssid;
  String ip;
  String apName;  // so no modo portal
  String apPass;
};

// forcePortal: abre o portal mesmo com Wi-Fi configurado (botao segurado no boot).
void netBegin(bool forcePortal);
void netLoop();
NetInfo netInfo();
