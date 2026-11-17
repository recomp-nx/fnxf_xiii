// fnxf_xiii (2026-10-25): changes to entries of the game's WPD files (.xgr, ...) as their data arrives in guest
// memory: a language pack's font tables, UI layouts, texture headers (fnxf_xiii_wpd_patch.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace fnxf_xiii {

// Called with an entry's data once all of it has arrived (each time its file is loaded).
using WpdPatchFn = std::function<void(uint8_t *data, uint32_t size)>;

// An entry, by its name and type ("wfnt16t", "wfl"). Before the game's files are read (OnPreSetup).
void WpdPatchRegister(const std::string &name, const std::string &ext, WpdPatchFn fn);
bool WpdPatchActive();
// Guest memory just written from a file (a read, fnxf_xiii_texture_sources.cpp) or by an inflate (fnxf_xiii_native_zlib.cpp).
void WpdPatchNoteWrite(uint32_t guest_va, uint32_t bytes);

}  // namespace fnxf_xiii
