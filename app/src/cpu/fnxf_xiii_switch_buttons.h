// fnxf_xiii (2026-10-25, fnxf_xiii_switch_buttons): the game's button pictures as the Switch's (fnxf_xiii_switch_buttons.cpp).
#pragma once

#include <cstdint>

namespace fnxf_xiii {

bool SwitchButtonsActive();
// Once the config is loaded (FnxfXiiiApp::OnPreSetup), before the game's files are read: the controller picture
// (tex/controller.png and .ini in the NRO's romfs, when there).
void SwitchButtonsInit();
// render/textures.cpp Upload, before a one-level texture's data is read (Renut's format, GpuTextureFormat).
void SwitchButtonsPatch(uint8_t *data, uint32_t format, uint32_t width, uint32_t height, bool tiled);

}  // namespace fnxf_xiii
