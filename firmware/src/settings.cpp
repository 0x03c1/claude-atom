#include "settings.h"

#include <Preferences.h>

Settings settings;

namespace {
constexpr const char *NS = "claude-atom";
}

void settingsLoad() {
  Preferences p;
  p.begin(NS, true);
  settings.ssid = p.getString("ssid", "");
  settings.pass = p.getString("pass", "");
  settings.anthropicKey = p.getString("anthropic", "");
  settings.googleKey = p.getString("google", "");
  settings.model = p.getString("model", settings.model);
  settings.voice = p.getString("voice", "");
  settings.language = p.getString("lang", settings.language);
  settings.volume = p.getUChar("volume", settings.volume);
  settings.speak = p.getBool("speak", settings.speak);
  p.end();
}

void settingsSave() {
  Preferences p;
  p.begin(NS, false);
  p.putString("ssid", settings.ssid);
  p.putString("pass", settings.pass);
  p.putString("anthropic", settings.anthropicKey);
  p.putString("google", settings.googleKey);
  p.putString("model", settings.model);
  p.putString("voice", settings.voice);
  p.putString("lang", settings.language);
  p.putUChar("volume", settings.volume);
  p.putBool("speak", settings.speak);
  p.end();
}
