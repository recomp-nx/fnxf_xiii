// fnxf_xiii (2026-10-25): the boot menu on the Switch (boot_menu_switch.cpp): language, frame interval, resolution,
// anti-aliasing and upscaler, kept in fnxf_xiii_settings.ini next to the NRO.
#pragma once

#include <filesystem>

#include <switch.h>

namespace renut::nxs {

// Before the shader pre-pass and the renderer's swapchain (FnxfXiiiApp::OnPreSetup). Shows the menu when the
// settings file does not exist (the first start) or A is held at start (0.5 s), then applies the
// settings (the options they set, over fnxf_xiii.toml's). Closing the software from HOME in the menu exits.
// user_data_root: the runtime's (fnxf_xiii 2026-10-28: the achievement lists read its achievements/ folder).
void RunBootMenu(const std::filesystem::path &user_data_root);

// fnxf_xiii (2026-11-07): whether the pre-pass and boot menu screens should be 1920x1080: docked (Reverse-NX's mode when
// it rules) on a 1080p display resolution, as the game's output (render/graphics_options.cpp; fnxf_xiii_diag_output 1:
// never, 2: always). Asks the applet at most twice a second.
bool ScreenWants1080p();

// fnxf_xiii (2026-11-07): libnx's framebuffer on the default window at 1280x720 or 1920x1080 (the window's size set to
// it while it is open, 1280x720 again after: the renderer's swapchain sets its own).
struct ScreenFramebuffer {
  Framebuffer fb{};
  bool open = false;
  bool hd = false;
  bool Open(bool want_1080p);
  void Close();
};

}  // namespace renut::nxs
