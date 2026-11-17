// fnxf_xiii (2026-10-25, fnxf_xiii_diag_texture_sources): where each texture Renut uploads came from in the game's files
// (fnxf_xiii_texture_sources.cpp).
#pragma once

#include <cstdint>

namespace fnxf_xiii {

bool TextureSourcesOn();
// fnxf_xiii (2026-11-16, fnxf_xiii_diag_load_watch): the damaged loading screens.
bool LoadWatchOn();
void LoadWatchTick(uint64_t frame);
void LoadWatchBeforeInflate(uint32_t strm, uint32_t next_in, uint32_t avail_in, uint32_t next_out);  // render thread, once a frame
void LoadWatchCheckUpload(uint32_t texture_va, uint32_t base, uint32_t bytes, uint32_t width, uint32_t height,
                          uint32_t format, bool tiled, uint64_t frame, const uint8_t *data);
// Once the config is loaded (FnxfXiiiApp::OnPreSetup): starts recording the game's file reads.
void TextureSourcesInit();
// fnxf_xiii_native_zlib.cpp: after each inflate call ('total_in' and 'total_out' the stream's counts before it).
void TextureSourcesNoteInflate(uint32_t strm, uint32_t next_in, uint32_t used_in, uint32_t total_in, uint32_t next_out,
                               uint32_t made_out, uint32_t total_out);
// fnxf_xiii_native_zlib.cpp: a stream initialised or ended.
void TextureSourcesForgetStream(uint32_t strm);
// render/textures.cpp Upload: logs the texture's source, once per texture.
void TextureSourcesNoteUpload(uint32_t texture_va, uint32_t base, uint32_t bytes, uint32_t width, uint32_t height,
                              uint32_t depth, uint32_t format, uint32_t mip_levels, bool tiled, uint64_t frame);

}  // namespace fnxf_xiii
