// fnxf_xiii (2026-10-25): the language of the port's own screens (the boot menu, the shader pre-pass, the in-game
// "Compiling shaders..." notice): 0 English, 1 German, 2 French, 3 Spanish, 4 Italian, the boot menu's order.
#pragma once

#include <cstdint>

#include <switch.h>

#include <rex/cvar.h>
#include <rex/system/flags.h>  // user_language

namespace renut::nxs {

constexpr int kUiLanguageCount = 5;

// The game's language (user_language, the Xbox 360 language the boot menu sets: 1, 3, 4, 5, 6); any other: English.
inline int UiLanguage() {
  switch (REXCVAR_GET(user_language)) {
  case 3: return 1;
  case 4: return 2;
  case 5: return 3;
  case 6: return 4;
  default: return 0;
  }
}

// The console's system language, for a screen shown before the settings are read (the applet-mode screen, before
// main); any language the game has no text for: English.
inline int SystemUiLanguage() {
  int index = 0;
  if (R_FAILED(setInitialize()))
    return 0;
  u64 code = 0;
  SetLanguage language = SetLanguage_ENUS;
  if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language))) {
    switch (language) {
    case SetLanguage_DE: index = 1; break;
    case SetLanguage_FR: case SetLanguage_FRCA: index = 2; break;
    case SetLanguage_ES: case SetLanguage_ES419: index = 3; break;
    case SetLanguage_IT: index = 4; break;
    default: break;
    }
  }
  setExit();
  return index;
}

}  // namespace renut::nxs
