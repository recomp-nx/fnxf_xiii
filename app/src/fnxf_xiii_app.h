// fnxf_xiii: an Xbox 360 game on the ReXGlue SDK, with reNut's Direct3D-level renderer
// (renut_engine/gpu, from fnxf_xiii / reNut by masterspike52) as the alternative to the SDK's emulated Xenos GPU.
//
//   fnxf_xiii_renderer = "native"  the SDK's GPU emulation (rexgpu-xenos: D3D12 / Vulkan on the PC) - the reference
//   fnxf_xiii_renderer = "renut"   Renut's renderer through plume (Vulkan on the PC, deko3d on the Switch)
#pragma once

#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/rex_app.h>
#include <rex/ui/window.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-26): sdk/src/system/xobject.cpp
#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchStackWatermarks(bool on);  // fnxf_xiii (2026-10-26): sdk/src/core/threading_posix.cpp
#endif

#include "cpu/fnxf_xiii_enhancements.h"
#include "fnxf_xiii_romfs.h"  // fnxf_xiii (2026-11-09): Preload
#include "cpu/fnxf_xiii_glyphs.h"  // fnxf_xiii (2026-10-25): GlyphsInit
#include "cpu/fnxf_xiii_handheld_gpu.h"  // fnxf_xiii (2026-10-25): HandheldGpuInit
#include "cpu/fnxf_xiii_texts.h"  // fnxf_xiii (2026-10-25): TextsInit
#include "cpu/fnxf_xiii_images.h"  // fnxf_xiii (2026-10-25): ImagesInit
#include "cpu/fnxf_xiii_layouts.h"  // fnxf_xiii (2026-10-25): LayoutsInit
#include "cpu/fnxf_xiii_switch_buttons.h"  // fnxf_xiii (2026-10-25): SwitchButtonsInit
#include "cpu/fnxf_xiii_texture_sources.h"
#include "profiler/profiler.h"
#include "pgo_switch.h"
#if FNXF_XIII_RENUT_RENDERER
#include "renut_engine/gpu/fnxf_xiii_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#if REX_PLATFORM_SWITCH
#include "renut_engine/gpu/nx_shaders/shader_prepass_switch.h"
#include "renut_engine/gpu/nx_shaders/boot_menu_switch.h"
#include "renut_engine/gpu/nx_shaders/thread_alloc.h"  // fnxf_xiii (2026-10-26): HeapSitesEnable
#endif
#endif

class FnxfXiiiApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<FnxfXiiiApp>(new FnxfXiiiApp(ctx, "fnxf_xiii", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    fnxf_xiii::romfs::Preload();  // fnxf_xiii (2026-11-09): the NRO's romfs into RAM, the NRO file closed
#if REX_PLATFORM_SWITCH && FNXF_XIII_RENUT_RENDERER
    // fnxf_xiii (2026-10-26): the memory report's heap sites (renut::nxs::HeapSitesReport), from here on: before the
    // shader store, the DKSH cache and the rest of the start-up's big blocks.
    if (REXCVAR_GET(log_diagnostics)) {
      renut::nxs::HeapSitesEnable();
      RexSwitchStackWatermarks(true);  // fnxf_xiii (2026-10-26): the report's host stack depths
    }
#endif
#if FNXF_XIII_PGO_GENERATE
    fnxf_xiii::StartPgoDumper();
#endif
    fnxf_xiii::ApplyMasterSwitches();
    fnxf_xiii::HandheldGpuInit();  // fnxf_xiii (2026-10-25): fnxf_xiii_handheld_gpu_460
#if FNXF_XIII_RENUT_RENDERER
    if (fnxf_xiii::renut_gpu::Active()) {
      // No SDK graphics system and no GPU plugin: the renderer is the app's.
      renut::gpu::ConfigureRuntime(config);
#if REX_PLATFORM_SWITCH
      // fnxf_xiii (2026-10-25): the boot menu (language, frame interval, resolution, anti-aliasing, upscaler): at the
      // first start (no fnxf_xiii_settings.ini) or with the left stick held pressed in; its settings applied at every
      // start. Before the shader pre-pass: when the shaders have to be compiled again (a new build), the menu does
      // not wait for it (the pre-pass reads none of the menu's settings).
      renut::nxs::RunBootMenu(user_data_root());  // fnxf_xiii (2026-10-28): the achievements
      // fnxf_xiii (2026-10-21): the game's shaders, found in its files, translated and compiled on the console before
      // the window exists (the pre-pass draws on it itself); at once when they all are already.
      renut::nxs::RunSwitchShaderPrepass("sdmc:/switch/fnxf_xiii/assets");
#if FNXF_XIII_PGO_GENERATE
      // fnxf_xiii (2026-10-26): the counts up to here (the pre-pass, when it compiled) as a part of their own.
      fnxf_xiii::PgoDump("prepass");
#endif
#endif
    }
#endif
    // fnxf_xiii (2026-10-25): the language pack (fnxf_xiii_lang, which the boot menu sets: so after it): its characters,
    // then its texts (written with them), its pictures and layouts, then the file reads watched (for its changes to
    // the game's files: fnxf_xiii_wpd_patch.cpp; and fnxf_xiii_diag_texture_sources).
    fnxf_xiii::GlyphsInit();
    fnxf_xiii::TextsInit();
    fnxf_xiii::ImagesInit();
    fnxf_xiii::LayoutsInit();
    fnxf_xiii::SwitchButtonsInit();  // fnxf_xiii (2026-10-25): the controller picture
    fnxf_xiii::TextureSourcesInit();
  }

#if FNXF_XIII_RENUT_RENDERER
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (fnxf_xiii::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
#endif

  void OnPostSetup() override {
    fnxf_xiii::LogEnhancements();
    fnxf_xiii::profiler::Start();  // fnxf_xiii (2026-10-24): the Switch CPU profiler, when fnxf_xiii_cpu_profiler asks for it
  }

  void OnShutdown() override {
#if FNXF_XIII_PGO_GENERATE
    fnxf_xiii::PgoDump();
#endif
#if FNXF_XIII_RENUT_RENDERER
    if (fnxf_xiii::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
#endif
  }

  // The game's files. Switch: always sdmc:/switch/fnxf_xiii/assets. PC: game_data_root from fnxf_xiii.toml, else an
  // "assets" folder next to the executable.
  void OnConfigurePaths(rex::PathConfig& paths) override {
#if REX_PLATFORM_SWITCH
    // The rooted form: libstdc++ takes "sdmc:/..." for a relative path and std::filesystem::absolute put the current
    // directory in front of it (Runtime::SetupVfs threw: "sdmc:/switch/fnxf_xiii/sdmc:/switch/fnxf_xiii/assets").
    // newlib resolves "/switch/..." on the SD card.
    paths.game_data_root = "/switch/fnxf_xiii/assets";
    // fnxf_xiii (2026-10-23): the user data (saves, achievements, the runtime's cache) in a "User" folder next to the
    // NRO, not in "fnxf_xiii" (the SDK's default: the app's name under the user folder, which on the Switch is the NRO's
    // folder). A user_data_root / cache_root in fnxf_xiii.toml still wins (rex_app.cpp applies it after this).
    paths.user_data_root = "/switch/fnxf_xiii/User";
    paths.cache_root = "/switch/fnxf_xiii/User/cache";
#else
    if (!paths.game_data_root.empty()) return;
    const auto folder = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    if (!folder.empty() && std::filesystem::is_directory(folder / "assets", ec))
      paths.game_data_root = folder / "assets";
#endif
  }

  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
#if FNXF_XIII_RENUT_RENDERER
    // The window exists here: Renut's renderer comes up before the game starts.
    if (fnxf_xiii::renut_gpu::Active())
      renut::gpu::StartRenderer();
#endif
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
};
