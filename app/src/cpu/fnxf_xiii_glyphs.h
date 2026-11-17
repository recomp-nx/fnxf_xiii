// fnxf_xiii (2026-10-25): a language pack's own characters drawn by the game's fonts (fnxf_xiii_glyphs.cpp).
#pragma once

#include <cstdint>

namespace fnxf_xiii {

// Once the config is loaded (FnxfXiiiApp::OnPreSetup): reads the pack's glyphs/glyphs.ini and its PNGs.
void GlyphsInit();
bool GlyphsActive();
// render/textures.cpp Upload, before a DXT1 texture's base level is read: its tiled data in guest memory.
void GlyphsPatchAtlas(uint8_t *data, uint32_t width, uint32_t height);
// The game's code (0x21-0xFF) a character of the pack is written with: a replaced character's, or -1 (not replaced).
int GlyphCode(char32_t c);

}  // namespace fnxf_xiii
