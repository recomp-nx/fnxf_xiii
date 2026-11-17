// fnxf_xiii (2026-10-25, fnxf_xiii_lang): a language pack, sdmc:/switch/fnxf_xiii/lang/<fnxf_xiii_lang>.zip, read once.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fnxf_xiii {

// language.ini:
//   [language]
//   name = Polski          the language's name, in itself (the boot menu's Language row)
//   base = english         the game's language under the pack: its voices, and its text where the pack has none
//                          (english, german, french, spanish, italian)
//   credits = ...          who made it (logged)
//   [port]
//   menu_title = ...       the port's own screens' text (boot menu, shader pre-pass, "Compiling shaders..."); the
//                          base language's where a key is missing (keys: the Polish pack's language.ini has
//                          them all).
struct LangPack {
  std::string name;                                          // fnxf_xiii_lang, e.g. "pl" (the zip's name)
  std::string display_name, base = "english", credits;       // language.ini
  std::map<std::string, std::string> port;                   // language.ini [port]
  std::map<std::string, std::vector<uint8_t>> files;         // by path in the zip ('/' separated)
  const std::vector<uint8_t> *Find(const std::string &path) const {
    auto it = files.find(path);
    return it == files.end() ? nullptr : &it->second;
  }
};

// The pack chosen by fnxf_xiii_lang, or null (none chosen, or it could not be read: logged). Loaded at the first call.
const LangPack *GetLangPack();

// The packs in sdmc:/switch/fnxf_xiii/lang (each zip's language.ini only; no files), by name. For the boot menu.
std::vector<LangPack> ListLangPacks();

// A PNG as RGBA bytes (fnxf_xiii_glyphs.cpp: stb_image). False when it is not one.
bool DecodePng(const std::vector<uint8_t> &png, int &width, int &height, std::vector<uint8_t> &rgba);

// fnxf_xiii (2026-10-28): one file of the pack 'name' (its zip's name), read alone (the boot menu: before a pack is
// chosen); empty when the pack or the file is not there.
std::vector<uint8_t> ReadPackFile(const std::string &name, const char *path);

// The chosen pack's [port] text for 'key', or 'fallback' (no pack, or no such key).
const char *PortText(const char *key, const char *fallback);

}  // namespace fnxf_xiii
