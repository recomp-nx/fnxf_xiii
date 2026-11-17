/**
 * @file    gpu/render/draw.cpp
 */
#include "renut_engine/gpu/render/draw.h"

#include <optional>

#include "renut_engine/frame_timing.h"  // fnxf_xiii (2026-11-05, fnxf_xiii_diag_vsync_timing: the bookkeeping by part)

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <plume_render_interface.h>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/system/xmemory.h>
#include <rex/exception_handler.h>  // fnxf_xiii (2026-10-27): fnxf_xiii_diag_cpu_reads
#if defined(__SWITCH__)
#include <switch.h>
#endif

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/memory_report.h"  // fnxf_xiii (2026-10-28): HeapRoomBytes
#include "renut_engine/gpu/backend/guest_gpu_probe.h"  // fnxf_xiii (2026-10-30): the GPU over guest memory, tried
#include "renut_engine/gpu/backend/guest_gpu_memory.h"  // fnxf_xiii (2026-10-30): and made (fnxf_xiii_memexport_zero_copy)
#include "renut_engine/perf/frame_stats.h"  // fnxf_xiii (2026-10-28): the [burst] line  // fnxf_xiii (2026-10-28): the [burst] line
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/nx_shaders/shader_store.h"  // fnxf_xiii (2026-10-31): the frame dump's shaders
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/line_compare.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/shader_library.h"  // fnxf_xiii: ShaderBoolSpecMask
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/graphics_options.h"  // fnxf_xiii (2026-11-07): Output1080p
#include "renut_engine/gpu/render/dynamic_resolution.h"  // fnxf_xiii (2026-11-07)
#include "renut_engine/gpu/render/textures.h"
#include "cpu/fnxf_xiii_texture_sources.h"  // fnxf_xiii (2026-11-16): LoadWatchTick
#include "renut_engine/gpu/render/vertex_format.h"
#include "renut_engine/gpu/backend/video_internal.h"
#include "renut_engine/gpu/fnxf_xiii_renut_tuning.h"
#include "renut_engine/gpu/fnxf_xiii_renut_select.h"  // fnxf_xiii (2026-10-28): HookTimeReport

// fnxf_xiii: counters for the heartbeat log (device.cpp StartVblankPump).
std::atomic<uint64_t> g_fnxf_xiii_draws{0}, g_fnxf_xiii_draws_dropped{0};
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // fnxf_xiii (2026-10-17): Deko3DBindMemExport
#endif
#include "renut_engine/gpu/fnxf_xiii_renut_record.h"  // fnxf_xiii (package 28b)
#include "renut_engine/gpu/render/fsr1.h"  // fnxf_xiii (2026-10-25)
#include "renut_engine/gpu/render/fsr3.h"  // fnxf_xiii (2026-11-06)
#include "renut_engine/frame_interval.h"  // fnxf_xiii (2026-11-08): MoviePlaying
#include <rex/cvar.h>

// fnxf_xiii (2026-10-17, a diagnostic for the deko3d renderer's missing river water): draws that read memexport results
// as vertices take them through the CPU path (wait for the export, read it back) instead of stream_convert_cs.
REXCVAR_DEFINE_INT32(fnxf_xiii_renut_upscale_filter, 2, "Game/Enhancements",
                     "deko3d renderer (2026-10-25): the pixel shader of the game's upscale (its 1024x576 picture to "
                     "1280x720). 0: the game's (9 fetches and 12 sines a pixel); 1: Renut's Lanczos-2 (metallic77's "
                     "libretro lanczos2 with going-digital's kernel fit: 5 fetches, sharper than the game's); 2: Renut's "
                     "fit of the game's own filter (Lanczos with a 1.5 window, 3x3 texels; polynomials instead of sines: "
                     "the game's picture within rounding); 3: AMD FSR 1 (EASU, then RCAS sharpening before the UI; "
                     "fnxf_xiii_fsr1_sharpness). 2026-10-25: 1, 2 and 3 are fixed to 1024x576 -> 1280x720 (the game's own "
                     "upscale draws with any other source size); 4: plain bilinear (one fetch a pixel, any size). The boot "
                     "menu offers 4 (Off), 1 and 3")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_final_reads, false, "Game/Diagnostics",
                    "Diagnostic (2026-11-07, a 1080p output's survey): every resolve from a colour target of 1280x720 "
                    "guest pixels (at a 1024x576 scene: the upscaled picture and what is drawn on it) counted by source, "
                    "destination texture and rectangle; a [final reads] table every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_camera_steps, false, "Game/Diagnostics",
                    "Diagnostic (2026-11-07, FSR 3 hitches; needs motion vectors: fnxf_xiii_fsr3 or "
                    "fnxf_xiii_motion_vectors): each frame's time since the last and how much one big scene draw's "
                    "world-view-projection changed, 60 frames a [camera steps] line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(fnxf_xiii_motion_vectors, 0, "Game/Enhancements",
                     "fnxf_xiii (2026-11-06, for FSR 3): the scene's motion vectors, per object (each draw's vertex shader "
                     "runs again with the same draw's constants of the previous frame). 0 off; 1 on; 2 on, and the "
                     "scene's surfaces show their motion as colour (a diagnostic: red / green the horizontal / vertical "
                     "motion, blue its direction; black still)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_diag_no_stream_convert, false, "Game/Renut",
                    "Diagnostic: exported streams reach their draws through the CPU (no stream_convert_cs)");
// fnxf_xiii (2026-10-17, a diagnostic for the depth of field that fnxf_xiii_gpu_blend_skip breaks): pixel shaders (their
// 16-digit hashes, comma separated) left out of the blend skip.
REXCVAR_DEFINE_STRING(fnxf_xiii_renut_diag_stream_dump_vs, "", "Game/Renut",
                      "Diagnostic: log the vertex streams of draws with this vertex shader (hex hash): guest and "
                      "converted words of the first vertices ([streamdump], first 16 binds)");
// fnxf_xiii (2026-10-27): 128, not 64: the game's copy kernel seeds three 32 MB windows a frame in its area loads (and
// grows the buffer past 64 MB doing it), so each such frame waited a GPU round trip (~200 ms, the stops after the
// event). A buffer the heap cannot give still makes the draw wait (DrawNeedsExportFlush).
REXCVAR_DEFINE_INT32(fnxf_xiii_memexport_slot_mb, 128, "fnxf_xiii",
                     "Export windows a frame may hold before a draw waits for the GPU instead of growing the buffers "
                     "(MB; the game's texture kernels write 32 MB windows while loading)");
// fnxf_xiii (2026-10-24): the memexport readback buffers CPU-cached (their lines invalidated before each read): they
// were read through the uncached mapping, the memcpy of CompleteMemExportsLocked most of the render thread's busy time
// in the loading hitches.
REXCVAR_DEFINE_BOOL(fnxf_xiii_deko3d_cached_readback, true, "Game/Renut",
                    "Memexport results read back through a CPU-cached mapping (false: uncached, as before)");
// fnxf_xiii (2026-10-24): on deko3d the memexport buffers are CPU-cached memory the CPU seeds and reads directly. Before,
// each window was seeded through the upload ring (the CPU's copy, then a GPU copy into the export buffer) and its
// results copied by the GPU into a readback buffer the CPU read: per 32 MB window, 64 MB more of the GPU's memory
// traffic each way and 32 MB of upload ring plus 32 MB of readback buffer (a texture-kernel burst of the game's: 300-650
// MB of large blocks, which a fragmented heap could not give: NO MEMORY, glitches).
REXCVAR_DEFINE_BOOL(fnxf_xiii_deko3d_direct_exports, true, "Game/Renut",
                    "deko3d: memexport results read by the CPU from the export buffer itself (no readback buffer or "
                    "copies)");
// fnxf_xiii (2026-10-24): the seeds written by the CPU into the export buffer too (no upload ring for them). Off: with it
// on (the first build of direct exports) the glitches were worse than without direct exports at all, and only this half
// is new to the GPU (the CPU's writes to memory the GPU caches; the results are read after the slot's fence, which
// flushes the GPU's caches, as the readback buffer was).
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_burst_log, false, "Game/Diagnostics",
                    "fnxf_xiii diagnostic (2026-10-28): the memexport bursts' log lines (area loads: [burst] frame "
                    "breakdowns, [export] copies and buffer decisions, [exporttex], [copykernel], 'Draw waited')");
// fnxf_xiii (2026-10-28): DeferMemExportsLocked.
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_deferred_results, true, "Game/Renut",
                    "Memexport results go to guest memory in the render thread's idle time after their frame, up to "
                    "their first use (false: all at once when the slot's fence is waited, as before)");
REXCVAR_DEFINE_BOOL(fnxf_xiii_colour_handover, true, "Game/Enhancements",
                    "deko3d renderer (2026-11-17): a resolve of a whole single-sampled colour target into "
                    "a texture of its size and format is not drawn: the texture samples the target's image, and takes it "
                    "(the target gets another) when the target is cleared whole or overwritten by a full-screen pass; "
                    "else the resolve is drawn then. The front buffer is shown from the target's image. Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(fnxf_xiii_colour_handover_fullscreen_vs, "7BEAFB9AAA3C0039,9E2FD35343CA3962,E60FDF42E1895532,6E8956D362E66708,76B5AD2FA528F84B,"
                      "02776E10AD747E06:0,E8A7DF8CEEFE03E3:0,23C89CC5289D4ED6:1", "Game/Enhancements",
                      "fnxf_xiii_colour_handover: the game's vertex shaders (hex hashes, comma separated; 'hash:n' for one that transforms the position by float constants n..n+3) that pass the "
                      "position through unchanged: their 4-vertex rectangles over the whole target overwrite it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_colour_handover_present, true, "Game/Enhancements",
                    "fnxf_xiii_colour_handover: the front buffer, when its resolve was left in the target's image, is shown "
                    "from that image (no copy)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_colour_handover_poison, false, "Game/Diagnostics",
                    "fnxf_xiii_colour_handover, a test: the image a target gets in place of the one a resolve destination "
                    "took is cleared magenta: magenta on screen is a place where the old content did matter")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_deko3d_direct_seeds, false, "Game/Renut",
                    "deko3d, with fnxf_xiii_deko3d_direct_exports: memexport seeds written by the CPU into the export "
                    "buffer (no upload ring for them)");
// fnxf_xiii (2026-10-29): the export buffer's write mask (shader_common_glsl.h memExportMark). Before, the CPU found
// what a kernel wrote by comparing each page of the results with a copy of the window's seed: every fresh window kept
// a 32 MB copy of its seed in the seed blocks (up to 192 MB a slot), a kept window copied itself into them on the GPU
// every frame (32 MB a window, 96 MB a frame in an area load), and the completion compared every written page twice
// (result against seed, guest memory against seed) before copying or merging it. With the mask a window has no seed
// copy: the completion copies the dwords the mask has, which keeps the game's writes to the rest of the page as the
// merge did. The windows are still seeded from guest memory (the GPU's readers of a window, and the kept windows,
// need them whole), only the pages guest memory changed when the window is kept.
REXCVAR_DEFINE_BOOL(fnxf_xiii_memexport_write_mask, true, "Game/Renut",
                    "deko3d, with fnxf_xiii_deko3d_direct_exports: the kernels mark the dwords they write in a mask of "
                    "the export buffer, and the results go to guest memory by the mask (no copy of each window's "
                    "seed, no compare). Off: the seed copies and compares, as before");
// fnxf_xiii (2026-10-29): a window seeded whole (a fresh buffer, a kept window that moved, a mismatch) is 32 MB of
// memcpy on the render thread, 15-20 ms each and three of them in an area load's first burst frame: the copy helpers
// (CopyHelpers, cores 0 and 2) take the pieces with it.
REXCVAR_DEFINE_BOOL(fnxf_xiii_memexport_parallel_seeds, true, "Game/Renut",
                    "A memexport window seeded whole from guest memory is copied by the render thread and the two copy "
                    "helpers together (false: the render thread alone, as before)");
REXCVAR_DEFINE_INT32(fnxf_xiii_renut_diag_draw_log_s, 0, "Game/Renut",
                     "Diagnostic: this many seconds after the first draw, log every draw of the next two frames "
                     "([drawlog]: shaders, counts, target, state; 0 = off)");
REXCVAR_DEFINE_INT32(fnxf_xiii_diag_load_draw_log, 0, "Game/Renut",
                     "Diagnostic (2026-11-08, the glitched first frame after a save is loaded): a [frames] line a frame "
                     "(its targets in order), every target built ([loadlog]), and when a frame's targets differ from "
                     "the frame before's every draw of this many frames ([drawlog] with the targets' addresses, "
                     "headers, scales, the output phase and FSR; 8 times at most); 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_early_fsr_stats, false, "Game/Renut",
                    "Diagnostic (2026-11-09): once a second, frames with the scene and those without FSR 3.1 before "
                    "the post-processing ([early] lines)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, fnxf_xiii_diag_log);
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-24): sdk/src/system/xobject.cpp
REXCVAR_DEFINE_INT32(fnxf_xiii_diag_ms_sample_mode, 0, "Game/Diagnostics",
                     "fnxf_xiii diagnostic (2026-10-22): one-sample resolves of a multisampled target: 0 as asked, 1 "
                     "always sample 0, 2 samples 0 and 1 swapped");
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_ms_average, false, "Game/Diagnostics",
                    "fnxf_xiii diagnostic (2026-10-22): a colour resolve of one sample of a multisampled target takes "
                    "the hardware resolve instead (the samples averaged; no sample fetch in a shader)");
REXCVAR_DEFINE_BOOL(fnxf_xiii_frame_dump, false, "Game/Renut",
                    "fnxf_xiii diagnostic: ZL + ZR pressed together dumps the next frame to frame_dump/frame_N next "
                    "to the executable: draws.txt (every draw's shaders, targets, state and textures) and every "
                    "texture a draw samples (GPU-written ones and, since 2026-10-27, the game's art), as read back "
                    "from the GPU, again whenever a resolve changed it (tex_<va>_d<draw>.bin). Slow while it writes");
REXCVAR_DEFINE_BOOL(fnxf_xiii_repeat_draw_fast, true, "Game/Renut",
                    "fnxf_xiii (2026-11-14): a "
                    "draw that only repeats the draw before it with other float constants (the same mesh placed "
                    "again) pushes its constants and issues the same draw call, without the rest of the draw's work. "
                    "The same commands in the same order. Never with motion vectors on (FSR 3: each draw has its own "
                    "previous constants). false: every draw goes the whole way");
REXCVAR_DEFINE_BOOL(fnxf_xiii_frame_dump_textures, true, "Game/Renut",
                    "fnxf_xiii_frame_dump diagnostic (2026-11-06): false: draws.txt and the shaders only, no texture "
                    "read back (small and quick: the draws' constants and state)");
REXCVAR_DEFINE_STRING(fnxf_xiii_frame_dump_probe, "", "Game/Renut",
                      "fnxf_xiii_frame_dump diagnostic (2026-10-31): \"x,y,w,h\" in the game's pixels: after every draw, that "
                      "rectangle of the draw's first colour target read back from the GPU into the frame's probe.bin (which "
                      "draw writes what there). Targets without multisampling only (anti-aliasing off). Very slow while it "
                      "dumps");
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_cpu_reads, false, "Game/Diagnostics",
                    "fnxf_xiii diagnostic (2026-10-27): does the game's CPU read what the game's GPU kernels write (the "
                    "memexport results copied back to guest memory)? A sample of the written pages (one in 32, at most "
                    "256 at a time: each is a split of the views' mappings) is unmapped in the guest's A, C and E views after each copy; the first CPU access "
                    "maps the page again and is counted (reads by fault, writes by the page found mapped again). "
                    "Reported every 10 s ([cpureads]). Slow while it runs");
// fnxf_xiii (2026-11-16, the crystal glare blinking: the game stops issuing its two glare sprites for a frame or two;
// does its visibility test read a resolve's result from guest memory, which Renut never writes there?).
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_resolve_reads, false, "Game/Diagnostics",
                    "fnxf_xiii diagnostic (2026-11-16): does the game's CPU read what a resolve wrote (Renut keeps a "
                    "resolve's result on the GPU only)? After every resolve, pages of its destination (all of a small "
                    "one, one in 8 of a large one, at most 32) are unmapped in the guest's A, C and E views; the first "
                    "CPU read maps the page again and is logged with the guest address and the host pc "
                    "([cpureads] every 10 s, the destination texture as its window). Slow while it runs");
REXCVAR_DEFINE_BOOL(fnxf_xiii_fence_waits_exports, true, "Game/Renut",
                    "fnxf_xiii (2026-11-16, the damaged loading screens): a D3D fence completes only once every pending "
                    "memexport result (the game's GPU copies of memory) is in guest memory. Off: as soon as the render "
                    "thread has read the work, as before (the game could then write memory a late GPU copy overwrote)");
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_glare, false, "Game/Diagnostics",
                    "fnxf_xiii diagnostic (2026-11-16, the crystal glare): the glare sprites (vs DA634FFC86F321E1 with "
                    "a 256x256 k_8_8_8_8 texture in slot 0) counted each frame; a [glare] log line whenever their "
                    "number changes, with each one's alpha (vertex constant c177.w) and the frames since the last "
                    "change");
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_card_writers, false, "Game/Diagnostics",
                    "fnxf_xiii diagnostic (2026-10-27): which code writes the game's memexport windows without marking "
                    "the store cards? One page in 256 of every fresh window is unmapped in the guest's A, C and E views "
                    "(at most 512 at a time); the first access maps it again, and a write is logged with its code "
                    "address and whether it marked the page's card. Reported every 10 s ([cardwriters])");
REXCVAR_DEFINE_STRING(fnxf_xiii_diag_watch, "", "Game/Diagnostics",
                      "fnxf_xiii diagnostic (2026-10-27, the glitched minimap): a guest physical range, \"14870000-14890000\" "
                      "(hex): every change of its bytes is logged with what made it (the memexport results' copy, or "
                      "anything else between frames) and its bytes written to watch_dump/ next to the executable; the "
                      "export draws whose windows hold it, and the textures uploaded from it, are logged");
REXCVAR_DEFINE_STRING(fnxf_xiii_renut_diag_blend_skip_exclude, "", "Game/Renut",
                      "Diagnostic: pixel shader hashes (hex, comma separated) fnxf_xiii_gpu_blend_skip leaves alone");

// fnxf_xiii (2026-10-20, from sa2-nx): see DrawStateLocked.
// fnxf_xiii (2026-11-14): ON by default again, at the user's request. Never measured in the FNXF games (user): the 36q
// profile quoted below came with the older tree.
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_chain_reuse, true, "Game/Enhancements",
                    "ON by default again since 2026-11-14. It was off since package 36r: the console's 36q profile (render thread) had DrawStateLocked 9.5 -> 12.6 % "
                    "and BindTargets +0.8 %: on N&B the reuse rarely hits, its checks cost more than they save "
                    "(configs/fnxf_xiii_renut_chain_reuse_on.toml). Renut's renderer (with fnxf_xiii_renut_snapshot_deltas): a draw of the delta chain takes the previous "
                    "draw's pipeline key when nothing the key is made from changed (the device's render state chunks, "
                    "the shaders, the declaration, the targets, the primitive) instead of making it again")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_chain_reuse_verify, false, "Game/Enhancements",
                    "Tests (fnxf_xiii_renut_chain_reuse): the key is made anyway and compared with the one taken; a "
                    "difference is logged and the made one used")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_inline_line_compare, false, "Game/Enhancements",
                    "OFF by default since package 36r: the console's 36q profile had PushSharedConstants 2.7 -> 4.9 % of "
                    "the render thread (memcmp stops at the first differing word; the whole-line compare does not). Renut's renderer on deko3d: the pushed shared constants' 64-byte lines are compared with what the "
                    "GPU holds by inline loads instead of a memcmp call per line (21 a draw). Off: memcmp")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_stream_mask, true, "Game/Enhancements",
                    "Renut's renderer: a draw's vertex streams are walked by the layout's mask of used streams instead "
                    "of a check of all 16")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// fnxf_xiii (2026-10-31): fetch_filters.h. A diagnostic switch (the console honours them; off: the fetch constants'
// filters alone, as before).
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_fetch_filters, true, "Game/Renut",
                    "Diagnostic: sample with the filters the shaders' texture fetch instructions give (as the console "
                    "does); false: the texture fetch constants' alone")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_texture_defaults_copy, true, "Game/Enhancements",
                    "Renut's renderer: a draw's default texture and sampler words are one copy of a block made once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_fetch_slots_skip, true, "Game/Enhancements",
                    "Renut's renderer on deko3d: a draw without vertex fetches skips the compare of the shared "
                    "constants' fetch slots when the last push had none either (both zero)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_fetch_data_unread, true, "Game/Enhancements",
                    "Renut's renderer on deko3d: a draw whose shaders fetch nothing by address gets fetch data address "
                    "and size 0 (they never read them) instead of a new allocation's, and no fetch binding")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_shared_partial_clear, true, "Game/Enhancements",
                    "Renut's renderer: a draw's shared constants are zeroed from the booleans on (848 of 1,360 bytes); "
                    "the texture and sampler words before them are all written by FillTextures. Off: all zeroed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_lean_diagnostics, true, "Game/Enhancements",
                    "Renut's renderer: the per-draw counts only the 10 s log lines read (the draw kinds, the fetch "
                    "alignment census) are kept only while those lines are logged (log_level info or lower), and the "
                    "draw and stream dumps' checks are made once instead of a call per draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, fnxf_xiii_renut_neon_compare);  // fnxf_xiii (2026-10-20, from sa2-nx): backend/queued.cpp

namespace plume {
void deko3dLog(const char *fmt, ...);  // fnxf_xiii (2026-10-27): plume_deko3d.cpp (deko3d.log)
}

namespace fnxf_xiii {
uint32_t RenderHeight();  // cpu/fnxf_xiii_render_resolution.cpp
}

namespace renut::gpu {

ChainHint g_chain_hint;  // fnxf_xiii (2026-10-20, from sa2-nx): draw.h
uint64_t g_chain_seq_counter = 1;  // fnxf_xiii (2026-10-20): NextChainSeq (draw.h); the first hint is 2
uint64_t NextChainSeq() { return ++g_chain_seq_counter; }

// fnxf_xiii (2026-10-28): the memexport bursts' log lines ([burst], [export], [exporttex], [copykernel], "Draw waited"),
// only with fnxf_xiii_diag_burst_log: the 2026-10-27/28 load-hitch work is done, and they were dozens of lines an area load.
// Warnings of a wrong result (a kept window that differs, NO MEMORY) are logged either way.
inline bool BurstLogOn() {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_burst_log);
  return on;
}
#define FNXF_XIII_BURST_LOG(log, ...)                                                                                     \
  do {                                                                                                                 \
    if (::renut::gpu::BurstLogOn())                                                                                    \
      log(__VA_ARGS__);                                                                                                \
  } while (0)

// fnxf_xiii (2026-10-28): a deferred completion of a slot's memexport results (DeferMemExportsLocked, further down).
int DeferredSlotLocked();
void FinishDeferredLocked(const char *why);
void FinishDeferredOverLocked(u32 base, u32 size);
// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_parallel_seeds): the first 'count' of g_seed_jobs (a window's seed in 2 MB
// pieces), copied by the render thread and the copy helpers together (CopyHelpers, with the completions).
void RunSeedJobs(size_t count);

namespace {
// fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy): the export windows in guest memory itself (defined with the
// export buffers' helpers, further down).
bool ZeroCopyOn();
plume::RenderBuffer *ExportBuffer(VideoState &s, u32 slot);

// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_lean_diagnostics): whether the per-draw diagnostic counts are kept (their lines are
// logged), once.
// fnxf_xiii (2026-10-20, package 36r): a byte read after the first call (the console's 36q profile: 0.4 % of the render
// thread in the call with its static's guard, three calls a draw).
int8_t g_diag_counts = 0;  // 1 on, -1 off, 0 not read yet
bool DiagCountsInit() {
  bool on = true;
  if (REXCVAR_GET(fnxf_xiii_renut_lean_diagnostics)) {
    auto *logger = ::rex::GetLoggerRaw(::rex::log::core());
    on = logger && logger->should_log(spdlog::level::info);
  }
  g_diag_counts = on ? 1 : -1;
  return on;
}
inline bool DiagCountsOn() {
  const int8_t v = g_diag_counts;
  return v ? v > 0 : DiagCountsInit();
}

// fnxf_xiii_renut_diag_blend_skip_exclude: the pixel shader's hash is in the list (read once).
bool BlendSkipExcluded(const void *pixel_shader) {
  static const std::unordered_set<uint64_t> excluded = [] {
    std::unordered_set<uint64_t> set;
    const std::string list = REXCVAR_GET(fnxf_xiii_renut_diag_blend_skip_exclude);
    size_t at = 0;
    while (at < list.size()) {
      const size_t comma = list.find(',', at);
      const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      if (!item.empty())
        set.insert(std::strtoull(item.c_str(), nullptr, 16));
      if (comma == std::string::npos)
        break;
      at = comma + 1;
    }
    return set;
  }();
  const auto *entry = static_cast<const ShaderCacheEntry *>(pixel_shader);
  return entry && !excluded.empty() && excluded.count(entry->hash);
}

constexpr u32 kMaxDrawVertices = 1u << 21;
constexpr u32 kMaxDrawIndices = 1u << 22;
constexpr u32 kMaxFetchCopyBytes = 32u << 20;
constexpr u32 kUpEndian = 2;  // Begin*Vertices fetch constants use 8in32

// Xenos primitive types (D3DPRIMITIVETYPE).
enum Primitive : u32 {
  kPointList = 1,
  kLineList = 2,
  kLineStrip = 3,
  kTriangleList = 4,
  kTriangleFan = 5,
  kTriangleStrip = 6,
  kRectangleList = 8,
  kLineLoop = 12,
  kQuadList = 13,
  kTrianglePatch = 17,  // tessellated draws only
  kQuadPatch = 18,
};

// SharedConstants as shader_common.h lays it out for RENUT_RECOMP (both the
// SPIR-V raw loads and the DXIL cbuffer packoffsets).
struct SharedConstants {
  u32 texture_indices[3][32];  // 0: [2D, 3D, cube][fetch constant]
  u32 sampler_indices[32];     // 384
  u32 booleans[8];             // 512: VS bools 0-127, PS bools 128-255
  // 544 / 568: screen-space draws (screen_space): what the Xenos viewport
  // transform left of scale and offset, per axis, in pixels (see
  // ComputeViewport).
  float screen_offset[2];      // 544
  float alpha_threshold;       // 552
  u32 alpha_test_func;         // 556: CompareFunction, 7 (always) when disabled
  uint64_t vertex_fetch_data;  // 560: device address of this draw's fetch copies (SPIR-V)
  float screen_scale[2];       // 568
  u32 vertex_fetch_slots[96];  // 576: per fetch index, copy offset | endian
  uint64_t memexport_address;  // 960: device address of the export buffer (SPIR-V)
  // 968: 1 when the vertex shader's positions are in pixels (the viewport
  // transform off) and it maps them to clip space itself; 0 when the host
  // viewport does it.
  u32 screen_space;
  // 972, fnxf_xiii (package 27; was padding): the byte size of the fetch data from vertex_fetch_data, the bound of
  // vfetchMemoryClamped (shader_common.h): the predicated-block pass runs bone fetches unconditionally in that form.
  u32 vertex_fetch_bytes;
  // 976: memexport windows: guest byte address, size, byte offset in the
  // export buffer. None yet, so memExport stores nothing.
  u32 memexport_streams[4][4];
  // 1040: tessellated draws: patch kind (0 none, 1 triangles, 2 quads),
  // segments per edge, first patch index; other draws: [3] what the host vertex
  // ID lacks of the guest vertex index (renutVertexRegisters).
  u32 tessellation[4];
  // 1056: per vertex input the shader reads with vfetchBound, its element as the
  // bound declaration describes it: stream fetch index, offset and stride in
  // dwords, D3DDECLTYPE (see DescribeBoundFetches).
  u32 bound_fetch[16][4];
  // 1312: point sprites (point_sprite_gs.hlsl): the render state's point width
  // and height, minimum and maximum size, in pixels (PA_SU_POINT_SIZE holds
  // half the size in 12.4 fixed point, PA_SU_POINT_MINMAX the size).
  float point_state[4];
  // 1328: the size in guest pixels of what clip space spans (the host
  // viewport, which is 'resolution_scale' times that), and padding.
  float viewport_size[2];
  float resolution_scale;  // 1336: of the render targets (ResolutionScale)
  // 1340, fnxf_xiii: FLT_MAX, the Xenos clamps' bound the SPIR-V shaders read here (shader_common.h; was padding).
  float flt_max;
  // 1344 (Renut upstream 574b488, Beta-Nut 4: "fixed an issue where jinjo bingo would show a black square instead of
  // the silhouette"; fnxf_xiii 2026-10-19): in guest pixels, what the pixel position is relative to: a draw predicated
  // to one tile has the tile's corner (ViewportState::pixel_origin).
  float pixel_origin[2];
  float pad_1352[1];  // fnxf_xiii: the fur's skin-once mode word (FurSkinLocked, pipeline.cpp constant 913)
  // 1356, fnxf_xiii (2026-11-07, dynamic resolution; was padding): the step between two levels' drawn fractions of the
  // textures resolved from the dynamic-resolution targets (shader_common_glsl.h g_DrsStep: their descriptor index has
  // the level's code at kDrsCodeShift, dynamic_resolution.h).
  float drs_step;
  // 1360, fnxf_xiii (2026-11-06, motion vectors for FSR 3; shader_common_glsl.h g_FnxfMotion / g_FnxfJitter,
  // nx_shaders/xenos_glsl.cpp MotionVectorPass): bit 0 the vertex shader runs with the previous frame's constants too
  // (UBO 4) and the pixel shader writes the motion; bit 1 shows it as the colour; bit 2 adds the jitter (clip space).
  u32 motion;
  u32 pad_1364;
  float jitter[2];  // 1368
};
static_assert(offsetof(SharedConstants, sampler_indices) == 384);
static_assert(offsetof(SharedConstants, booleans) == 512);
static_assert(offsetof(SharedConstants, screen_offset) == 544);
static_assert(offsetof(SharedConstants, screen_scale) == 568);
static_assert(offsetof(SharedConstants, screen_space) == 968);
static_assert(offsetof(SharedConstants, alpha_test_func) == 556);
static_assert(offsetof(SharedConstants, vertex_fetch_data) == 560);
static_assert(offsetof(SharedConstants, vertex_fetch_slots) == 576);
static_assert(offsetof(SharedConstants, memexport_address) == 960);
static_assert(offsetof(SharedConstants, memexport_streams) == 976);
static_assert(offsetof(SharedConstants, tessellation) == 1040);
static_assert(offsetof(SharedConstants, bound_fetch) == 1056);
static_assert(offsetof(SharedConstants, point_state) == 1312);
static_assert(offsetof(SharedConstants, viewport_size) == 1328);
static_assert(offsetof(SharedConstants, resolution_scale) == 1336);
static_assert(offsetof(SharedConstants, pixel_origin) == 1344);
static_assert(offsetof(SharedConstants, drs_step) == 1356);
static_assert(offsetof(SharedConstants, motion) == 1360);
static_assert(offsetof(SharedConstants, jitter) == 1368);
static_assert(sizeof(SharedConstants) == 1376);
static_assert(sizeof(SharedConstants) == kSharedConstantsUboBytes);

// The guest D3DDevice a draw reads its state from: the live device, or a
// recorded copy of its first kDeviceSnapshotSize bytes.
class GuestDevice {
public:
  explicit GuestDevice(const u8 *bytes) : bytes_(bytes) {}
  bool valid() const { return bytes_ != nullptr; }
  const d3d::Device &dev() const { return *reinterpret_cast<const d3d::Device *>(bytes_); }
  u32 U32(u32 offset) const { return *reinterpret_cast<const be_u32 *>(bytes_ + offset); }
  u8 U8(u32 offset) const { return bytes_[offset]; }
  float F32(u32 offset) const {
    const u32 bits = U32(offset);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }
  // Xenos fetch constant 'index' (0-95) word 0/1 as a vertex fetch constant.
  u32 VertexFetchWord(u32 index, u32 word) const {
    return U32(u32(offsetof(d3d::Device, m_Constants)) + index * 8 + word * 4);
  }

private:
  const u8 *bytes_;
};

// A shader stage's last float constant upload: the guest bytes of the registers
// it holds, reused while the next draw's shader reads only registers it holds
// with the same bytes.
struct ConstantUpload {
  UploadAllocation alloc;
  u32 valid[8] = {};  // registers written to alloc, bit i = register i
  u8 bytes[256 * 16];  // their guest (big-endian) bytes
  // The replay (ReplayConstants::replay) whose draw last made 'valid' match
  // its constants, since when that replay's dirty groups are the only ones
  // that can differ; 0 for none.
  uint64_t replay = 0;
  // fnxf_xiii (fnxf_xiii_gpu_literals_cbuf): the shader whose literal registers alloc holds (UploadFloatConstants).
  const ShaderCacheEntry *literals = nullptr;
  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_push_constants): the registers of the pushed uniform buffer that hold
  // 'literals''s literal values (cleared where a group update overwrites one).
  u32 literal_regs[8] = {};
};

// State bound on the current command list, reset whenever a new one begins.
struct Bound {
  uint64_t generation = UINT64_MAX;
  const plume::RenderFramebuffer *framebuffer = nullptr;
  const plume::RenderPipeline *pipeline = nullptr;
  bool layout = false;
  UploadAllocation zero_stream;
  ConstantUpload vs_constants, ps_constants;
  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_push_constants): the models of the pushed uniform buffers, apart from the ring
  // uploads' (point-sprite draws keep those, and the two must not share one model: emulator verify run 60).
  ConstantUpload pushed_vs, pushed_ps;
  bool blend_constant_set = false;
  float blend_constant[4] = {};

  // State as the draws last set it on the list, which most draws in a row
  // share (the viewport, the zero stream, the memexport buffer): setting it
  // again cost a driver call each. Both backends keep all of it across
  // pipeline and framebuffer changes; D3D12 root descriptors are dropped with
  // the root signature, so they go with 'layout'.
  bool viewport_set = false;
  plume::RenderViewport viewport;
  plume::RenderRect scissor;
  uint32_t viewport_count = 0;  // viewports and scissors set (SetViewport)
  bool root_set[kDrawRootMemExport + 1] = {};
  plume::RenderBufferReference roots[kDrawRootMemExport + 1];
  struct Stream {
    bool set = false;
    plume::RenderVertexBufferView view;
    plume::RenderInputSlot slot;
  };
  Stream streams[kZeroStreamSlot + 1];
  bool index_set = false;
  plume::RenderIndexBufferView index;
  // fnxf_xiii (fnxf_xiii_renut_shared_dedupe): the last SharedConstants upload and its bytes, which a draw with the same
  // bytes reuses (no upload, and set 7 stays bound).
  UploadAllocation shared_alloc;
  bool shared_valid = false;
  SharedConstants shared_bytes;
  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_push_constants): what the pushed shared uniform buffer holds (valid once the
  // list's first pushed draw sent it whole), and whether plume reads bound textures from it.
  bool pushed_shared_valid = false;
  bool shared_words_set = false;
  SharedConstants pushed_shared;
  // fnxf_xiii: the uniform buffer sets 5-7 last bound (Vulkan), and their offsets.
  const plume::RenderDescriptorSet *constant_sets[3] = {};
  uint32_t constant_offsets[3] = {};
};
Bound g_bound;

// fnxf_xiii: binds a constant upload's uniform buffer set at its offset (Vulkan), unless it is bound there already:
// most draws in a row reuse the vertex shader's constants, and many the pixel shader's.
void BindConstantSet(plume::RenderCommandList *cmd, uint32_t set_index, plume::RenderDescriptorSet *set,
                     uint64_t offset) {
  const uint32_t slot = set_index - kVertexConstantsSet;
  const uint32_t offset32 = uint32_t(offset);
  if (g_bound.constant_sets[slot] == set && g_bound.constant_offsets[slot] == offset32)
    return;
  cmd->setGraphicsDescriptorSetDynamic(set, set_index, &offset32, 1);
  g_bound.constant_sets[slot] = set;
  g_bound.constant_offsets[slot] = offset32;
}

// The last declaration a draw looked up by VA (otherwise a registry lookup
// under its lock per draw): draws in a row mostly use the same one. Cleared
// when a declaration goes (ForgetVertexDeclarationLocked).
u32 g_last_declaration_va = 0;
VertexDeclarationRecord *g_last_declaration = nullptr;
// fnxf_xiii (2026-11-08, shadows): where the draw's declaration came from (frame dump): 1 set by VA, 2 the shader's
// bound one, 3 its own vfetches, 4 none; and the VA.
int g_decl_source = 0;
u32 g_decl_va = 0;
// fnxf_xiii (2026-11-08, real-time shadows): the shadow maps' casters are drawn with no vertex declaration set: the
// game bound their vertex shaders with D3DVertexShader_Bind (native, not hooked), which rewrites the shader's vertex
// fetches in its microcode; the renderer's shader is translated from the unbound microcode (format, stride and
// offset 0), so those draws had no vertex input (every vertex at one point, nothing drawn, the shadow maps all far:
// no real-time shadows). The same meshes are drawn in the scene with a declaration set (43 of the 47 vertex buffers
// of the shadow pass in a Steppe frame): the declaration and stream strides each vertex buffer (stream 0's address
// and size) was last drawn with, for a draw of it with none.
struct VertexBufferDeclaration {
  u32 declaration_va = 0;
  u32 strides[d3d::kMaxStreams] = {};
};
std::unordered_map<uint64_t, VertexBufferDeclaration> g_vb_declarations;
uint64_t VertexBufferKey(const GuestDevice &device) {
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(0);
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
  return base && size ? (uint64_t(base) << 32) | size : 0;
}
const ShaderRecord *g_decl_vs = nullptr;

bool SameReference(const plume::RenderBufferReference &a, const plume::RenderBufferReference &b) {
  return a.ref == b.ref && a.offset == b.offset;
}

// plume's Vulkan pipelines have one viewport and scissor per color target
// (all dynamic), each of which a draw must have set; they all get the same.
void SetViewport(plume::RenderCommandList *cmd, const plume::RenderViewport &viewport,
                 const plume::RenderRect &scissor, uint32_t color_count) {
  // Every index set so far is rewritten, so none keeps an older value.
  uint32_t count = std::clamp(color_count, 1u, uint32_t(d3d::kMaxRenderTargets));
  if (g_bound.viewport_set)
    count = std::max(count, g_bound.viewport_count);
  const plume::RenderViewport &v = g_bound.viewport;
  const plume::RenderRect &r = g_bound.scissor;
  const bool more = count > g_bound.viewport_count;
  if (!g_bound.viewport_set || more || v.x != viewport.x || v.y != viewport.y ||
      v.width != viewport.width || v.height != viewport.height ||
      v.minDepth != viewport.minDepth || v.maxDepth != viewport.maxDepth) {
    const plume::RenderViewport viewports[d3d::kMaxRenderTargets] = {viewport, viewport, viewport,
                                                                     viewport};
    cmd->setViewports(viewports, count);
    g_bound.viewport = viewport;
  }
  if (!g_bound.viewport_set || more || r.left != scissor.left || r.top != scissor.top ||
      r.right != scissor.right || r.bottom != scissor.bottom) {
    const plume::RenderRect scissors[d3d::kMaxRenderTargets] = {scissor, scissor, scissor, scissor};
    cmd->setScissors(scissors, count);
    g_bound.scissor = scissor;
  }
  if (!g_bound.viewport_set || more)
    g_bound.viewport_count = count;
  g_bound.viewport_set = true;
}

void SetRootDescriptor(plume::RenderCommandList *cmd, const plume::RenderBufferReference &ref,
                       uint32_t index) {
  if (g_bound.root_set[index] && SameReference(g_bound.roots[index], ref))
    return;
  cmd->setGraphicsRootDescriptor(ref, index);
  g_bound.roots[index] = ref;
  g_bound.root_set[index] = true;
}

void SetVertexBuffer(plume::RenderCommandList *cmd, const plume::RenderVertexBufferView &view,
                     const plume::RenderInputSlot &slot) {
  Bound::Stream &b = g_bound.streams[slot.index];
  if (b.set && SameReference(b.view.buffer, view.buffer) && b.view.size == view.size &&
      b.slot.stride == slot.stride && b.slot.classification == slot.classification)
    return;
  cmd->setVertexBuffers(slot.index, &view, 1, &slot);
  b.set = true;
  b.view = view;
  b.slot = slot;
}

void SetIndexBuffer(plume::RenderCommandList *cmd, const plume::RenderIndexBufferView &view) {
  const plume::RenderIndexBufferView &b = g_bound.index;
  if (g_bound.index_set && SameReference(b.buffer, view.buffer) && b.size == view.size &&
      b.format == view.format)
    return;
  cmd->setIndexBuffer(&view);
  g_bound.index = view;
  g_bound.index_set = true;
}

// The blend constant (RB_BLEND_RED..ALPHA) the draw's blend controls read, or
// false if none reads it. The host has a single constant for BLEND_FACTOR, so a
// colour factor that reads the constant alpha gets the alpha in rgb as well.
bool DrawBlendConstant(const d3d::Device &dev, const PipelineKey &key, float rgba[4]) {
  constexpr u32 kConstantColor = u32(D3DBlend::BlendFactor);
  constexpr u32 kOneMinusConstantAlpha = u32(D3DBlend::InvConstantAlpha);
  constexpr u32 kConstantAlpha = u32(D3DBlend::ConstantAlpha);
  bool used = false, color_reads_color = false, color_reads_alpha = false;
  for (u32 i = 0; i < key.color_count; ++i) {
    // RB_BLENDCONTROL factors: colour src 0, colour dst 8, alpha src 16, alpha dst 24.
    for (u32 shift : {0u, 8u, 16u, 24u}) {
      const u32 factor = (key.blend_control[i] >> shift) & 0x1F;
      if (factor < kConstantColor || factor > kOneMinusConstantAlpha)
        continue;
      used = true;
      if (shift < 16)
        (factor >= kConstantAlpha ? color_reads_alpha : color_reads_color) = true;
    }
  }
  if (!used)
    return false;
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  const float alpha = v.BlendAlpha;
  const bool replicate_alpha = color_reads_alpha && !color_reads_color;
  if (color_reads_alpha && color_reads_color) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("Blend control {:08X} reads both the constant colour and alpha; the host uses the colour",
                key.blend_control[0]);
    }
  }
  rgba[0] = replicate_alpha ? alpha : float(v.BlendRed);
  rgba[1] = replicate_alpha ? alpha : float(v.BlendGreen);
  rgba[2] = replicate_alpha ? alpha : float(v.BlendBlue);
  rgba[3] = alpha;
  return true;
}

std::vector<u32> g_indices;  // scratch for index reads

// SetTilesLocked / SetReplayPredicationLocked.
std::vector<ClearRect> g_tiles;
u32 g_replay_predication = ~0u;

// A vertex stream as a draw reads it: guest vertices through a host layout.
struct StreamKey {
  const u8 *guest;
  uint64_t layout_hash;
  u32 stream, stride, count, endian;
  bool operator==(const StreamKey &) const = default;
};
struct StreamKeyHash {
  size_t operator()(const StreamKey &k) const {
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(k.guest)) * 0x9E3779B97F4A7C15ull;
    h ^= k.layout_hash + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    h ^= (uint64_t(k.stream) << 48 | uint64_t(k.endian) << 40 | k.stride) + (h << 6) + (h >> 2);
    h ^= uint64_t(k.count) * 0xFF51AFD7ED558CCDull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};

// Vertex data a draw reads from guest buffers, read when the ring is kicked
// off (FlushVertexFillsLocked) rather than at the draw call: the GPU starts on
// a draw only after the kick, and the game may still write the buffers the
// draw uses until then.
struct PendingStreamFill {
  HostVertexStream stream;
  const u8 *guest = nullptr;
  u32 stride = 0, count = 0, endian = 0;
  u8 *host = nullptr;
  StreamKey key{};   // cacheable: the g_stream_cache entry
  bool cacheable = false;
};
struct PendingCopy {
  const u8 *src = nullptr;
  u8 *dst = nullptr;
  u32 size = 0;
};
std::vector<PendingStreamFill> g_stream_fills;
std::vector<PendingCopy> g_copy_fills;

// Fetch data copies (UploadVertexFetchData) queued since the last flush, by
// guest bytes: they all read memory at the flush, so a later draw that reads
// the same bytes takes the queued copy rather than another. Where in its
// upload chunk the copy is (the draw's fetch slots count from the chunk's
// start). A generation, bumped at each flush, retires them all at once.
struct FetchCopy {
  const u8 *src = nullptr;
  u32 size = 0;
  uint64_t generation = 0;
  const plume::RenderBuffer *buffer = nullptr;
  u32 offset = 0;
};
constexpr u32 kFetchCopyBits = 8;
FetchCopy g_fetch_copies[1u << kFetchCopyBits];
uint64_t g_fetch_copy_generation = 1;

FetchCopy &FetchCopyFor(const u8 *src, u32 size) {
  const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(src)) ^ (uint64_t(size) << 40);
  return g_fetch_copies[(key * 0x9E3779B97F4A7C15ull) >> (64 - kFetchCopyBits)];
}

// Streams queued in g_stream_fills since the last flush. The fills all read
// guest memory at the flush, so a later draw that reads the same guest
// vertices through the same host layout draws from the upload already queued.
// fnxf_xiii (2026-10-19, render-thread proposal 20): an open-addressed table with a generation (no node allocated per
// stream and freed at each flush); a stream that finds no free slot within kQueuedProbe is converted again (as without
// the table: only slower).
struct QueuedStream {
  StreamKey key{};
  UploadAllocation alloc;
  uint64_t generation = 0;
};
constexpr uint32_t kQueuedStreamSlots = 2048;  // a power of two
constexpr uint32_t kQueuedProbe = 16;
std::vector<QueuedStream> g_queued_streams(kQueuedStreamSlots);
uint64_t g_queued_generation = 1;

const UploadAllocation *FindQueuedStream(const StreamKey &key) {
  const size_t h = StreamKeyHash{}(key);
  for (uint32_t i = 0; i < kQueuedProbe; ++i) {
    const QueuedStream &q = g_queued_streams[(h + i) & (kQueuedStreamSlots - 1)];
    if (q.generation != g_queued_generation)
      return nullptr;
    if (q.key == key)
      return &q.alloc;
  }
  return nullptr;
}

void AddQueuedStream(const StreamKey &key, const UploadAllocation &alloc) {
  const size_t h = StreamKeyHash{}(key);
  for (uint32_t i = 0; i < kQueuedProbe; ++i) {
    QueuedStream &q = g_queued_streams[(h + i) & (kQueuedStreamSlots - 1)];
    if (q.generation != g_queued_generation || q.key == key) {
      q.key = key;
      q.alloc = alloc;
      q.generation = g_queued_generation;
      return;
    }
  }
}

// Converted streams (and index ranges, below) kept across frames. Most are
// geometry the game never rewrites, which every draw would otherwise convert
// again. A stream whose guest bytes hash the same at kStableFlushes flushes in
// a row gets a persistent converted copy, which draws bind while the bytes
// still hash the same, checked off the render thread once every
// kCacheVerifyFrames frames a draw reads it: hashing every copy every frame
// cost more than any other part of a draw. A stream the game rewrites
// kMaxCacheDrops times is never cached again. A copy no draw has read for
// kStreamCacheIdleFrames goes: turning back to a scene within that converts
// nothing again.
constexpr u32 kStableFlushes = 3;
constexpr uint64_t kCacheVerifyFrames = 4;
constexpr u32 kMaxCacheDrops = 3;
constexpr uint64_t kStreamCacheIdleFrames = 3600;
constexpr uint64_t kStreamCacheMaxBytes = 768ull << 20;
uint64_t g_stream_cache_bytes = 0;
uint64_t g_stream_cache_frame = 0;
// fnxf_xiii (2026-10-17): FlushVertexFillsLocked's calls (every kick, and the end of every command list): the points
// after which the game may learn the GPU is done with what the draws before read, and write those buffers again.
// fnxf_xiii_renut_exact_streams proves a cached copy once per interval between two of them.
uint64_t g_flush_serial = 0;
// fnxf_xiii (2026-10-27): a frame's render-thread time in the memexport bursts, by part (the [burst] line,
// AdvanceStreamCacheFrameLocked): the seeds' copies from guest memory, the results' copies to it, the waits for the
// GPU's fences (video.cpp's recycle and the draws' waits).
struct BurstTiming {
  double seed_ms = 0.0, copy_ms = 0.0, gpu_wait_ms = 0.0;
  double idle_copy_ms = 0.0;  // fnxf_xiii (2026-10-28): results copied in the render thread's idle time (deferred)
  uint64_t seed_bytes = 0, seeds = 0;
  uint64_t parallel_seeds = 0;  // fnxf_xiii (2026-10-29): of the seeds, pieces the copy helpers took part in
  uint64_t mask_bytes = 0;      // fnxf_xiii (2026-10-29): results copied to guest memory by the write mask
  uint64_t zero_copy_windows = 0;  // fnxf_xiii (2026-10-30): fresh windows prepared in guest memory itself (ZeroCopyOn)
} g_burst;
// fnxf_xiii (package 28, fnxf_xiii_renut_diag_resolve_trace): draws since the last change of targets.
uint32_t g_trace_draws = 0;

// A cached copy's memory: part of one of the pool's buffers.
struct PoolSpan {
  plume::RenderBuffer *buffer = nullptr;
  u8 *cpu = nullptr;
  u32 chunk = 0;
  uint64_t offset = 0;
  u32 size = 0;
  explicit operator bool() const { return buffer != nullptr; }
  plume::RenderBufferReference Ref() const { return plume::RenderBufferReference(buffer, offset); }
};

// Where the cached copies live: a few large upload buffers, mapped once, that
// copies are carved from. A buffer of its own per copy cost a driver
// allocation each, and a scene coming into view made hundreds of copies in a
// frame. A freed span is reused once the frame slot it was freed in has
// finished on the GPU (DrainSlot), as retired resources are. A chunk left
// with nothing in it is released (the first one stays): the copies of a
// scene the game has left go after kStreamCacheIdleFrames, and their memory
// with them.
class CopyPool {
public:
  PoolSpan Allocate(u32 size) {
    const uint64_t need = (uint64_t(size) + kAlign - 1) & ~(kAlign - 1);
    for (u32 i = 0; i < chunks_.size(); ++i) {
      if (PoolSpan span = Take(i, need); span)
        return span;
    }
    const u32 index = AddChunk(need);
    if (index == UINT32_MAX)
      return {};
    return Take(index, need);
  }

  void Free(const PoolSpan &span) {
    if (span)
      deferred_[State().frame].push_back(span);
  }

  // The slot's lists have finished: what was freed while it recorded is free.
  void ReleaseSlot(uint32_t slot) {
    for (const PoolSpan &span : deferred_[slot]) {
      Chunk &chunk = chunks_[span.chunk];
      Insert(chunk, span.offset, span.size);
      // Spans hold their chunk's index, so the entry stays for AddChunk to
      // reuse. The other slot may still draw from it: retired, not freed.
      if (span.chunk != 0 && chunk.free.size() == 1 && chunk.free.begin()->second == chunk.size) {
        Video::RetireLocked(std::move(chunk.buffer));
        chunk.cpu = nullptr;
        chunk.free.clear();
        chunk.size = 0;
      }
    }
    deferred_[slot].clear();
  }

  // Host memory the chunks hold.
  uint64_t Bytes() const {
    uint64_t bytes = 0;
    for (const Chunk &chunk : chunks_)
      bytes += chunk.size;
    return bytes;
  }

private:
  static constexpr uint64_t kChunkSize = 64ull << 20;
  static constexpr uint64_t kAlign = 256;

  struct Chunk {
    std::unique_ptr<plume::RenderBuffer> buffer;  // null once released
    u8 *cpu = nullptr;
    uint64_t size = 0;
    std::map<uint64_t, uint64_t> free;  // offset -> size, adjacent ranges merged
  };

  // The index of a new chunk of at least 'min_size' bytes, or UINT32_MAX.
  u32 AddChunk(uint64_t min_size) {
    // fnxf_xiii (2026-10-24): a 64 MB chunk the heap cannot give is tried at 32 and 16 MB (sizes freed chunks can be
    // reused for); a size that failed is not tried again for a while (each try walked the fragmented heap on the render
    // thread, and every copy tried again; a copy that is not cached is converted at each draw instead: nothing lost).
    constexpr uint64_t kBackoffFrames = 300;
    static uint64_t failed_size = 0, failed_frame = 0;
    const uint64_t frame = TextureFrameForLog();
    const uint64_t sizes[] = {std::max(kChunkSize, min_size), std::max<uint64_t>(32u << 20, min_size),
                              std::max<uint64_t>(16u << 20, min_size)};
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint64_t size = 0;
    for (const uint64_t candidate : sizes) {
      if (candidate == size)
        continue;  // the same size as the one just tried
      size = candidate;
      if (failed_size && size >= failed_size && frame - failed_frame < kBackoffFrames)
        continue;
      buffer = State().device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
          size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX));
      if (buffer)
        break;
      failed_size = size;
      failed_frame = frame;
    }
    void *mapped = buffer ? buffer->map() : nullptr;
    if (!mapped)
      return UINT32_MAX;
    u32 index = 0;
    while (index < chunks_.size() && chunks_[index].buffer)
      ++index;
    if (index == chunks_.size())
      chunks_.emplace_back();
    buffer->setName("renut stream cache #" + std::to_string(index));
    Chunk &chunk = chunks_[index];
    chunk.buffer = std::move(buffer);
    chunk.cpu = static_cast<u8 *>(mapped);
    chunk.size = size;
    chunk.free.emplace(0, size);
    return index;
  }

  // First fit in chunk 'index'.
  PoolSpan Take(u32 index, uint64_t size) {
    Chunk &chunk = chunks_[index];
    for (auto it = chunk.free.begin(); it != chunk.free.end(); ++it) {
      if (it->second < size)
        continue;
      const uint64_t offset = it->first, left = it->second - size;
      chunk.free.erase(it);
      if (left)
        chunk.free.emplace(offset + size, left);
      PoolSpan span;
      span.buffer = chunk.buffer.get();
      span.cpu = chunk.cpu + offset;
      span.chunk = index;
      span.offset = offset;
      span.size = u32(size);
      return span;
    }
    return {};
  }

  static void Insert(Chunk &chunk, uint64_t offset, uint64_t size) {
    auto next = chunk.free.lower_bound(offset);
    if (next != chunk.free.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {
        offset = prev->first;
        size += prev->second;
        chunk.free.erase(prev);
      }
    }
    if (next != chunk.free.end() && offset + size == next->first) {
      size += next->second;
      chunk.free.erase(next);
    }
    chunk.free.emplace(offset, size);
  }

  std::vector<Chunk> chunks_;
  std::vector<PoolSpan> deferred_[kFramesInFlight];
};
CopyPool g_copy_pool;

// A direct-mapped memo of a node map's entries by key hash, in front of the
// stream and index caches: draws look up the same keys every frame, and the
// map's bucket and node chain were cache misses each time (a tenth of the
// render thread). Here a repeat is one line and the entry itself, which the
// draw reads anyway. The entries stay where they are until erased (the map is
// node-based, a rehash moves none), so the memo is cleared whenever the map
// erases any.
template <typename Map, typename Hash, u32 kBits>
class EntryMemo {
public:
  using Entry = typename Map::value_type;

  // fnxf_xiii (2026-10-19, render-thread proposal 9): the slot holds the key itself, so a hit compares it in the slot's
  // line (no read of the node's key, a second cache miss: the node is read after for its fields only).
  Entry *Find(const typename Map::key_type &key, size_t hash) const {
    const Slot &slot = slots_[Index(hash)];
    return slot.entry && slot.hash == hash && slot.key == key ? slot.entry : nullptr;
  }

  // The map's entry for 'key', made if 'make' (else null when it has none).
  Entry *Get(Map &map, const typename Map::key_type &key, bool make) {
    const size_t hash = Hash{}(key);
    if (Entry *entry = Find(key, hash))
      return entry;
    auto it = map.find(key);
    if (it == map.end()) {
      if (!make)
        return nullptr;
      it = map.try_emplace(key).first;
    }
    slots_[Index(hash)] = {hash, &*it, key};
    return &*it;
  }

  void Clear() { std::fill(std::begin(slots_), std::end(slots_), Slot{}); }

private:
  struct Slot {
    size_t hash = 0;
    Entry *entry = nullptr;
    typename Map::key_type key{};
  };
  static u32 Index(size_t hash) {
    return u32((uint64_t(hash) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
  }
  Slot slots_[1u << kBits];
};

struct CachedStream {
  uint64_t hash = 0;  // guest bytes at the last conversion, or of the copy
  u32 stable = 0;     // flushes in a row that converted the same bytes
  u32 drops = 0;      // copies dropped because the guest bytes changed
  bool dynamic = false;
  PoolSpan copy;
  u32 bytes = 0;
  uint64_t last_used = 0;               // g_stream_cache_frame
  uint64_t checked_frame = UINT64_MAX;  // g_stream_cache_frame the copy was last checked in
  uint64_t proven_flush = UINT64_MAX;   // fnxf_xiii: g_flush_serial the copy was last hashed equal in
  uint64_t exact_frame = UINT64_MAX;    // fnxf_xiii: g_stream_cache_frame of its last exact check (repeat count)
  // fnxf_xiii (2026-11-06, seen19): a dynamic stream's quiet run (RedeemQuietStream): the flush it was last converted
  // at, how many conversions in a row found its memory unwritten since the one before, and how often it came back.
  uint64_t quiet_flush = UINT64_MAX;
  u32 quiet = 0;
  u32 redeemed = 0;
  // fnxf_xiii (2026-11-06, seen21): converted at a flush before (TrackConvertedStream hashes it from its second one).
  bool seen = false;
};
using StreamCache = std::unordered_map<StreamKey, CachedStream, StreamKeyHash>;
StreamCache g_stream_cache;
EntryMemo<StreamCache, StreamKeyHash, 12> g_stream_memo;  // fnxf_xiii: 4,096 slots of a key each (proposal 9)

// Whether a cached copy last checked in 'checked_frame' is due a check now.
bool CacheCheckDue(uint64_t checked_frame) {
  return checked_frame == UINT64_MAX || g_stream_cache_frame - checked_frame >= kCacheVerifyFrames;
}

// fnxf_xiii (package 27c): fnxf_xiii_renut_exact_streams' checks, through a memo of the ranges hashed in this interval
// (between two kicks): a buffer cached under several stream keys (layouts, stream slots) or index keys is read once.
// Exact as the checks themselves: they hash at the first use in the interval, and so does the memo's first.
struct IntervalHash {
  const void *data = nullptr;
  size_t size = 0;
  uint64_t serial = UINT64_MAX;
  uint64_t hash = 0;
};
IntervalHash g_interval_hashes[4096];

// fnxf_xiii (2026-10-19, CPU proposal 1, fnxf_xiii_renut_store_cards): the store cards (rex/system/xmemory.h,
// tools/store_cards.py). Every guest store outside the stack zeroes the card of its page (the byte at its guest page
// number in the first MB of the guest window), and so does every host writer of guest memory. A copy's check reads the
// cards of its pages, sets those it finds written back to 1 and records the interval (g_flush_serial) it found them in;
// the copy is current when no page of it was found written in or after the interval it was last proven in. A write
// whose card the check reads before its data is visible is found in that interval, so the copy is hashed again in the
// next one, when the data has long landed: the cards never answer for an interval in which they were cleared.
//
// A physical page's cards are those of the views the guest writes it through: 0xA0000000, 0xC0000000, 0xE0000000
// (4 KB lower: guest 0xE0000000 is physical 0x1000) and, in the first 16 MB, 0x7F000000. A store marks its first
// byte's page only, so an unaligned store that runs into the next page leaves that page's cards clean: a page also
// counts as written when the page before it was.
uint32_t g_page_serial[0x20000];
// fnxf_xiii (2026-10-27): the interval of each page's last write by the game or a host writer (the cards' zeroes, as
// g_page_serial), without the renderer's own memexport copies (WrotePagesLocked): a memexport window's page the game
// did not write since the window's seed takes the GPU's result whole, no comparing (CompleteMemExportsLocked).
uint32_t g_game_serial[0x20000];
// fnxf_xiii (2026-10-27, fnxf_xiii_diag_card_writers): how many times a page's cards were found written.
uint32_t g_game_writes[0x20000];
// fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy): the same last write, in the zero-copy intervals: one begins at
// each export draw's prepare (PrepareZeroCopyExportLocked), so a reader can tell a game write before a window's record
// from one after it (ZeroCopyReadOnGpuLocked) without g_flush_serial moving at every kernel draw (every cached copy's
// proof is redone once an interval of that one: 675 kernel draws a frame made it 675 proofs of each stream).
uint32_t g_game_zc_serial[0x20000];
uint32_t g_zc_serial = 1;  // never 0: a PendingExport::seed_interval of 0 means "unknown, copy as recorded"

// fnxf_xiii_renut_store_cards_verify (PC): what the checks found. [0] pages read, [1] found written, [2..5] by the view
// 0xA0000000 / 0xC0000000 / 0xE0000000 / 0x7F000000, [6] copies written only through the page before theirs,
// [7] copies hashed after a written page whose bytes had not changed, [8] copies not in physical memory.
uint64_t g_card_diag[9];
uint32_t g_card_samples[8];
uint32_t g_card_sample_count;

bool LogLimited(uint32_t &counter, uint32_t limit);

// The cards: the guest window's first MB (rex::memory::g_store_cards, a runtime variable this file cannot link to on
// Windows, where the runtime is a DLL).
u8 *StoreCards() {
  static u8 *const cards = REX_KERNEL_MEMORY()->virtual_membase();
  return cards;
}

// The physical address of the guest memory at 'data' (a pointer into the raw physical view, or into the guest window at
// a guest address of one of the views of physical memory: a vertex or index buffer's CPU address, where the game wrote
// it), or UINT32_MAX for anything else.
u32 PhysicalOf(const u8 *data) {
  static const u8 *const physical = GuestPhysicalPtr<const u8>(0);
  const uintptr_t raw = uintptr_t(data) - uintptr_t(physical);
  if (raw < 0x20000000u)
    return u32(raw);
  const uint64_t window = uint64_t(uintptr_t(data) - uintptr_t(StoreCards()));
  if (window >= 0x100000000ull)
    return UINT32_MAX;
  u32 va = u32(window);
  if (rex::memory::detail::PhysicalHostOffset(va) && va >= 0xE0001000u)
    va -= 0x1000u;  // the PC's host offset of the 0xE0000000 view
  if (va >= 0xA0000000u)
    return (va & 0x1FFFFFFFu) + (va >= 0xE0000000u ? 0x1000u : 0u);
  if (va >= 0x7F000000u && va < 0x80000000u)
    return va - 0x7F000000u;
  return UINT32_MAX;
}

bool PagesClean(const u8 *data, size_t bytes, uint64_t proven) {
  u8 *cards = StoreCards();
  const u32 offset = cards && bytes ? PhysicalOf(data) : UINT32_MAX;
  const bool diag = fnxf_xiii::renut_gpu::StoreCardsVerifyOn();
  if (offset == UINT32_MAX || bytes > 0x20000000u - offset) {
    if (diag)
      ++g_card_diag[7 + 1];  // not physical memory
    return false;
  }
  const u32 serial = u32(g_flush_serial);
  const u32 last = u32((uint64_t(offset) + bytes - 1) >> 12);
  u32 page = offset >> 12;
  page -= page != 0;
  bool clean = true;
  const u32 own_first = offset >> 12;
  bool own_written = false;
  for (; page <= last; ++page) {
    u8 *a = cards + 0xA0000u + page, *c = cards + 0xC0000u + page;
    u8 *e = page ? cards + 0xE0000u + page - 1 : a, *f = page < 0x1000u ? cards + 0x7F000u + page : a;
    if (diag) {
      ++g_card_diag[0];
      const bool va = !*a, vc = !*c, ve = !*e, vf = !*f;
      if (va || vc || ve || vf) {
        ++g_card_diag[1];
        g_card_diag[2] += va;
        g_card_diag[3] += vc;
        g_card_diag[4] += ve;
        g_card_diag[5] += vf;
        own_written |= page >= own_first;
        if (g_card_sample_count < 8)
          g_card_samples[g_card_sample_count++] = page | (va ? 1u << 28 : 0) | (vc ? 1u << 29 : 0) |
                                                  (ve ? 1u << 30 : 0) | (vf ? 1u << 31 : 0);
      }
    }
    if (!__atomic_load_n(a, __ATOMIC_RELAXED) || !__atomic_load_n(c, __ATOMIC_RELAXED) ||
        !__atomic_load_n(e, __ATOMIC_RELAXED) || !__atomic_load_n(f, __ATOMIC_RELAXED)) {
      __atomic_store_n(a, u8(1), __ATOMIC_RELAXED);
      __atomic_store_n(c, u8(1), __ATOMIC_RELAXED);
      __atomic_store_n(e, u8(1), __ATOMIC_RELAXED);
      __atomic_store_n(f, u8(1), __ATOMIC_RELAXED);
      g_page_serial[page] = serial;
      g_game_serial[page] = serial;  // fnxf_xiii (2026-10-27)
      g_game_zc_serial[page] = g_zc_serial;  // fnxf_xiii (2026-10-30)
      ++g_game_writes[page];
    }
    if (g_page_serial[page] >= u32(proven))
      clean = false;
  }
  if (diag && !clean && !own_written && own_first != 0 && g_page_serial[own_first - 1] >= u32(proven))
    ++g_card_diag[6];
  return clean;
}

// fnxf_xiii (2026-10-27): a physical page's cards taken into g_page_serial / g_game_serial now (as PagesClean does:
// a zero card is a write since they were last taken); its last write's interval by the game or a host writer.
u32 HarvestPageLocked(u32 page) {
  u8 *cards = StoreCards();
  if (!cards || page >= 0x20000u)
    return UINT32_MAX;  // unknown: written
  u8 *a = cards + 0xA0000u + page, *c = cards + 0xC0000u + page;
  u8 *e = page ? cards + 0xE0000u + page - 1 : a, *f = page < 0x1000u ? cards + 0x7F000u + page : a;
  if (!__atomic_load_n(a, __ATOMIC_RELAXED) || !__atomic_load_n(c, __ATOMIC_RELAXED) ||
      !__atomic_load_n(e, __ATOMIC_RELAXED) || !__atomic_load_n(f, __ATOMIC_RELAXED)) {
    __atomic_store_n(a, u8(1), __ATOMIC_RELAXED);
    __atomic_store_n(c, u8(1), __ATOMIC_RELAXED);
    __atomic_store_n(e, u8(1), __ATOMIC_RELAXED);
    __atomic_store_n(f, u8(1), __ATOMIC_RELAXED);
    g_page_serial[page] = u32(g_flush_serial);
    g_game_serial[page] = u32(g_flush_serial);
    g_game_zc_serial[page] = g_zc_serial;  // fnxf_xiii (2026-10-30)
    ++g_game_writes[page];
  }
  return g_game_serial[page];
}

// fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy): the same, in the zero-copy intervals (g_zc_serial).
u32 HarvestPageZcLocked(u32 page) {
  if (HarvestPageLocked(page) == UINT32_MAX)
    return UINT32_MAX;  // unknown: written
  return g_game_zc_serial[page];
}

// fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy): the cards of pages [first, last] taken (HarvestPageLocked),
// eight pages a load where all of them are clean: a 32 MB window is 8192 pages, and the export draws' prepare and the
// submits' clean read every page of every window (one page at a time, with four atomic loads each, that was 0.5 ms a
// window: 319 ms in a frame of 675 kernel draws, fnxf_xiii_002.log).
void HarvestPagesLocked(u32 first, u32 last) {
  u8 *cards = StoreCards();
  if (!cards || first > last)
    return;
  last = std::min(last, 0x1FFFFu);
  constexpr uint64_t kClean = 0x0101010101010101ull;  // eight cards of 1
  u32 page = first;
  for (; page <= last && ((page & 7) != 0 || page == 0); ++page)
    HarvestPageLocked(page);
  for (; page + 7 <= last; page += 8) {
    uint64_t a, c, e, f = kClean;
    std::memcpy(&a, cards + 0xA0000u + page, 8);
    std::memcpy(&c, cards + 0xC0000u + page, 8);
    std::memcpy(&e, cards + 0xE0000u + page - 1, 8);  // the 0xE0000000 view's cards are a page lower (page >= 8 here)
    if (page < 0x1000u)
      std::memcpy(&f, cards + 0x7F000u + page, 8);
    if (a == kClean && c == kClean && e == kClean && f == kClean)
      continue;  // nothing new on these eight pages (a store racing this load is found by the next harvest)
    for (u32 k = 0; k < 8; ++k)
      HarvestPageLocked(page + k);
  }
  for (; page <= last; ++page)
    HarvestPageLocked(page);
}

// fnxf_xiii (2026-10-27): pages the renderer wrote into guest memory (the memexport results): written in this interval
// for the cached copies (g_page_serial), not for g_game_serial. The game's writes not yet taken are taken first.
void WrotePagesLocked(u32 base, u32 size) {
  if (!size)
    return;
  const u32 first = (base & 0x1FFFFFFFu) >> 12, last = std::min<u32>(((base & 0x1FFFFFFFu) + size - 1) >> 12, 0x1FFFFu);
  for (u32 page = first; page <= last; ++page) {
    HarvestPageLocked(page);
    g_page_serial[page] = u32(g_flush_serial);
  }
}

// The diagnostics' 10 s line (fnxf_xiii_renut_tuning.cpp calls it in verify mode).
extern "C" uint64_t RexStoreCardsHostBytes(int which);

// Whether the cards prove a cached copy last proven in interval 'proven' (UINT64_MAX: never, made at a flush) whose
// guest bytes hashed 'hash' then. The cards are read, and cleared, before any hash of the range in this interval.
bool CardsProve(const u8 *data, size_t bytes, uint64_t proven, uint64_t hash) {
  if (!fnxf_xiii::renut_gpu::StoreCardsOn())
    return false;
  const bool clean = PagesClean(data, bytes, proven) && proven != UINT64_MAX;
  if (clean && fnxf_xiii::renut_gpu::StoreCardsVerifyOn() && fnxf_xiii::renut_gpu::ContentHash64(data, bytes) != hash) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 32)) {
      static const u8 *const physical = GuestPhysicalPtr<const u8>(0);
      RGPU_WARN("[cards] physical {:08X}-{:08X} ({} bytes) changed under clean cards: a writer of guest memory that "
                "marks none (fnxf_xiii_renut_store_cards_verify)",
                u32(data - physical), u32(data - physical + bytes - 1), bytes);
    }
    fnxf_xiii::renut_gpu::CountCardsMissed(bytes);
    return false;
  }
  fnxf_xiii::renut_gpu::CountCards(bytes, clean);
  return clean;
}

uint64_t ExactHash(const void *data, size_t size) {
  const uintptr_t k = (reinterpret_cast<uintptr_t>(data) >> 4) ^ (uintptr_t(size) * 0x9E3779B1u);
  IntervalHash &e = g_interval_hashes[(k ^ (k >> 12)) & 4095];
  if (e.serial == g_flush_serial && e.data == data && e.size == size) {
    fnxf_xiii::renut_gpu::CountExactMemoHit(size);
    return e.hash;
  }
  e = {data, size, g_flush_serial, fnxf_xiii::renut_gpu::ContentHash64(data, size)};
  return e.hash;
}

uint64_t HashGuestStream(const StreamKey &key) {
  // fnxf_xiii: fnxf_xiii_renut_pmull_hash (the PMULL CRC-64 on ARM).
  return fnxf_xiii::renut_gpu::ContentHash64(key.guest, size_t(key.count) * key.stride);
}

// The cached copies' checks, hashed off the render thread: every copy's guest
// bytes each kCacheVerifyFrames frames was a twentieth of its time. A copy
// found stale is dropped when the render thread takes the results, at the next
// frame (AdvanceStreamCacheFrameLocked), so a rewritten range can draw from
// its old copy for about a frame longer than the check interval alone allows.
template <typename Key, uint64_t (*Hash)(const Key &)>
class ContentVerifier {
public:
  struct Check {
    Key key;
    uint64_t hash;  // the copy's hash when submitted
  };

  ~ContentVerifier() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable())
      thread_.join();
  }

  // Render thread. Collected until the frame's Flush: waking the worker for
  // each check was a system call per draw.
  void Submit(const Key &key, uint64_t hash) { submitted_.push_back({key, hash}); }

  // Render thread, once a frame: hands the frame's checks to the worker.
  void Flush() {
    if (submitted_.empty())
      return;
    {
      std::lock_guard lock(mutex_);
      if (!thread_.joinable())
        thread_ = std::thread([this] { Run(); });
      pending_.insert(pending_.end(), submitted_.begin(), submitted_.end());
    }
    submitted_.clear();
    wake_.notify_one();
  }

  // Render thread: the checks whose bytes no longer hash as submitted.
  void TakeStale(std::vector<Check> &out) {
    std::lock_guard lock(mutex_);
    out.swap(stale_);
    stale_.clear();
  }

private:
  void Run() {
    // fnxf_xiii: the background core and priority on the Switch (fnxf_xiii_renut_tuning.h).
    fnxf_xiii::renut_gpu::OnBackgroundThreadStart("content verifier");
    std::vector<Check> batch;
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return stop_ || !pending_.empty(); });
      if (stop_)
        return;
      batch.swap(pending_);
      lock.unlock();
      // Guest memory stays mapped; a read torn by a game write only fails
      // the check, which converts the range again.
      std::erase_if(batch, [](const Check &c) { return Hash(c.key) == c.hash; });
      lock.lock();
      stale_.insert(stale_.end(), batch.begin(), batch.end());
      batch.clear();
    }
  }

  std::vector<Check> submitted_;  // render thread only
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Check> pending_, stale_;
  bool stop_ = false;
  std::thread thread_;
};
ContentVerifier<StreamKey, HashGuestStream> g_stream_verifier;

void DropCachedCopy(CachedStream &c) {
  if (!c.copy)
    return;
  g_stream_cache_bytes -= c.bytes;
  g_copy_pool.Free(c.copy);
  c.copy = {};
  c.bytes = 0;
}

// fnxf_xiii (2026-11-06, seen19): a stream turned dynamic (its guest bytes changed kMaxCacheDrops times) stayed so for
// good and was converted at every kick: run 751caa1c, 13.7-15.4 MB of streams a frame at the kicks, 7-7.7 ms of the
// render thread with three threads, in every [rtwork] frame of play. A stream the game rewrote a few times (an area's
// first seconds) and then left alone paid that for the rest of the area. At each conversion of a dynamic stream the
// store cards say whether its memory was written since the conversion before (PagesClean, as the cached copies are
// proven); after kRedeemFlushes such conversions in a row it is a plain stream again (no drops, no stable count),
// and takes the usual way to a cached copy (kStableFlushes equal hashes, then the copy and its checks). A stream that
// turns dynamic again after kMaxRedeems returns stays so. g_redeem counts for [rtwork]: the bytes of dynamic streams
// converted, and of those found unwritten.
constexpr u32 kRedeemFlushes = 60;
constexpr u32 kMaxRedeems = 4;
uint64_t g_dynamic_fill_bytes = 0, g_quiet_fill_bytes = 0, g_redeemed_streams = 0;
void RedeemQuietStream(const PendingStreamFill &f) {
  auto it = g_stream_cache.find(f.key);
  if (it == g_stream_cache.end() || !it->second.dynamic)
    return;
  CachedStream &c = it->second;
  const size_t bytes = size_t(f.count) * f.stride;
  g_dynamic_fill_bytes += bytes;
  if (!fnxf_xiii::renut_gpu::StoreCardsOn() || c.redeemed >= kMaxRedeems) {
    c.quiet = 0;
    return;
  }
  const bool clean = c.quiet_flush != UINT64_MAX && PagesClean(f.guest, bytes, c.quiet_flush);
  c.quiet_flush = g_flush_serial;
  if (!clean) {
    c.quiet = 0;
    return;
  }
  g_quiet_fill_bytes += bytes;
  if (++c.quiet < kRedeemFlushes)
    return;
  c.dynamic = false;
  c.drops = 0;
  c.stable = 0;
  c.hash = 0;
  c.quiet = 0;
  c.quiet_flush = UINT64_MAX;
  ++c.redeemed;
  ++g_redeemed_streams;
}

// After a flush converted 'f': count how long its bytes have held, and give a
// stream that has held long enough its persistent copy.
void TrackConvertedStream(const PendingStreamFill &f) {
  auto it = g_stream_cache.find(f.key);
  if (it == g_stream_cache.end())
    return;
  CachedStream &c = it->second;
  if (c.dynamic || c.copy)
    return;
  const size_t guest_bytes = size_t(f.count) * f.stride;  // fnxf_xiii (2026-11-06, seen20, [rtwork])
  // fnxf_xiii (2026-11-06, seen21): a stream's first conversion is not hashed: run 9a82c4a6, 8.1-9.6 MB a frame of the
  // kicks' streams were seen for the first time (new keys every frame: the game's per-frame vertex buffers) and every
  // one was hashed whole for a copy it never got. From the second conversion on as before (the copy one flush later).
  if (!c.seen) {
    c.seen = true;
    rtw::g_fill_first_bytes += guest_bytes;
    return;
  }
  const uint64_t hash = HashGuestStream(f.key);
  // The second conversion has no hash to compare with (the first skipped it): counted as waiting.
  (c.hash != 0 && hash != c.hash ? rtw::g_fill_changed_bytes : rtw::g_fill_waiting_bytes) +=
      guest_bytes;
  c.stable = hash == c.hash ? c.stable + 1 : 0;
  c.hash = hash;
  const u32 bytes = f.count * f.stream.host_stride;
  if (c.stable + 1 < kStableFlushes)
    return;
  if (g_stream_cache_bytes + bytes > fnxf_xiii::renut_gpu::StreamCacheMaxBytes(kStreamCacheMaxBytes)) {  // fnxf_xiii
    rtw::g_fill_full_bytes += guest_bytes;
    return;
  }
  const PoolSpan copy = g_copy_pool.Allocate(bytes);
  if (!copy) {
    rtw::g_fill_full_bytes += guest_bytes;
    return;
  }
  // Converted again rather than copied from f.host: upload memory is
  // write-combined, slow to read back.
  ConvertVertices(f.stream, f.guest, f.stride, f.count, f.endian, copy.cpu);
  c.copy = copy;
  c.bytes = bytes;
  c.checked_frame = g_stream_cache_frame;
  c.proven_flush = UINT64_MAX;  // fnxf_xiii: made at a flush; proven again at its first use after it
  g_stream_cache_bytes += bytes;
}

void SyncBound(VideoState &s) {
  if (g_bound.generation != s.list_generation) {
    g_bound = Bound{};
    g_bound.generation = s.list_generation;
  }
}

bool LogLimited(uint32_t &counter, uint32_t limit = 8) {
  return counter++ < limit;
}

// Guest VA or GPU physical address to a host pointer.
const u8 *AddressPtr(u32 address) {
  if (address < 0x20000000u)
    return GuestPhysicalPtr<const u8>(address);
  return GuestPtr<const u8>(address);
}

plume::RenderPrimitiveTopology Topology(u32 primitive) {
  switch (primitive) {
  case kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case kLineList:
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case kLineStrip:
  case kLineLoop:
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  default:  // lists, and fans / quads expanded to lists
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------

struct Targets {
  HostSurface *colors[d3d::kMaxRenderTargets] = {};
  uint32_t color_count = 0;
  HostSurface *depth = nullptr;
  // In guest pixels, as everything the draws and clears work out; host
  // viewports, scissors and rects are 'scale' times that (HostSurface::scale).
  uint32_t width = 0, height = 0;
  RenderScale scale;
  // The size the guest surface header gives; smaller than width x height for
  // a target grown for tiling (SetTilingExtentLocked).
  uint32_t guest_width = 0, guest_height = 0;
  // fnxf_xiii (2026-10-27): the stand-in target of an exporting draw without one (ExportTargetsLocked): its pixels go
  // nowhere the game reads.
  bool stand_in = false;
};

// fnxf_xiii (2026-10-31): the EDRAM colour targets share (defined with the clears below).
void EdramCarryLocked(const Targets &t);
void EdramNoteDrawnLocked(HostSurface *c);
void EdramNoteClearLocked(HostSurface *c);

// fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover; "Depth hand-over" below): the targets of the draw whose textures
// FillTextures binds (a destination is not sampled from the image of a target of its own draw), and what is done
// before a depth target is bound again.
const Targets *g_handover_targets = nullptr;
void DepthHandOverBeforeBindLocked(HostSurface *surface, const plume::RenderRect *clear_rect, u32 clear_flags);
// fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover; "Colour hand-over" below).
std::unordered_set<const HostSurface *> g_colour_tracked;  // colour targets whose content a destination holds
bool g_colour_ho_draw_checked = false;  // BindTargets for a draw or clear whose colour targets were seen to before
struct ColourTakePending {
  HostSurface *surface = nullptr;
  int kind = 0;
};
ColourTakePending g_colour_take_pending[d3d::kMaxRenderTargets];
uint32_t g_colour_take_pending_count = 0;
int g_colour_mat_why = 0;  // (diagnostic) see g_colour_mat_tally
void ColourHandOverBeforeWriteLocked(HostSurface *surface, int take);
struct FloatConstants;
void ColourHandOverDrawStartLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord *vs,
                                   const ShaderRecord *ps, const Targets &t,
                                   const VertexDeclarationRecord *declaration, const u32 *shader_strides,
                                   const FloatConstants &vs_constants);
void ColourHandOverDrawBindLocked();
void ColourHandOverReportLocked();
bool ColourTakePendingFor(const HostSurface *surface);

// A rect in guest pixels as host pixels of targets 'scale' times the size.
plume::RenderRect ScaleRect(const plume::RenderRect &rect, RenderScale scale) {
  return plume::RenderRect(scale.Coord(rect.left), scale.Coord(rect.top), scale.Coord(rect.right),
                           scale.Coord(rect.bottom));
}

bool GatherTargets(const GuestDevice &device, Targets &t) {
  for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i) {
    const u32 va = device.U32(d3d::kDeviceOffsetRenderTargets + i * 4);
    if (!va)
      break;
    HostSurface *surface = GetHostSurfaceLocked(va);
    if (!surface || surface->depth)
      break;
    t.colors[t.color_count++] = surface;
  }
  HostSurface *depth = GetHostSurfaceLocked(device.U32(d3d::kDeviceOffsetDepthStencil));
  if (depth && depth->depth)
    t.depth = depth;
  if (!t.color_count && !t.depth)
    return false;
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  t.width = first->width;
  t.height = first->height;
  t.scale = first->scale;
  // A pass over targets rendered at different scales (graphics_options) would
  // address them alike; the options pick only targets drawn on their own. The
  // post-processing buffers render at 1x with the scene's depth still bound,
  // untested (on the console too its pitch is not theirs), so only colors count.
  bool mixed = false;
  for (uint32_t i = 1; i < t.color_count; ++i)
    mixed = mixed || t.colors[i]->scale != t.scale;
  if (mixed) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 8)) {
      RGPU_WARN("Draw binds color targets of different render scales ({}x{} at {:.3g}x)",
                first->width, first->height, t.scale.Factor());
    }
  }
  t.guest_width = d3d::SurfaceWidth(first->size_word);
  t.guest_height = d3d::SurfaceHeight(first->size_word);
  return true;
}

// fnxf_xiii (2026-10-25): a host format's short name and bytes a pixel, for the GPU profile's pass names (what each pass
// writes is what decides its memory traffic).
const char *ProfileFormatName(plume::RenderFormat f) {
  using F = plume::RenderFormat;
  switch (f) {
  case F::R8G8B8A8_UNORM: return "RGBA8";
  case F::B8G8R8A8_UNORM: return "BGRA8";
  case F::R16G16B16A16_FLOAT: return "RGBA16F";
  case F::R16G16B16A16_UNORM: return "RGBA16";
  case F::R10G10B10A2_UNORM: return "RGB10A2";
  case F::R16G16B16A16_SNORM: return "RGBA16S";
  case F::R16G16_FLOAT: return "RG16F";
  case F::R16G16_SNORM: return "RG16S";
  case F::R32_FLOAT: return "R32F";
  case F::R32G32_FLOAT: return "RG32F";
  case F::D24_UNORM_S8_UINT: return "D24S8";
  case F::D32_FLOAT_S8_UINT: return "D32FS8";
  default: return "fmt?";
  }
}

// gpu_profile: draws into 't', by their host size and attachments. fnxf_xiii (2026-10-25): with the formats and the
// sample count ("draws 1024x576 RGBA16F x2 + D24S8").
uint32_t TargetsProfileLabel(const Targets &t) {
  HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  const uint32_t variant = t.color_count ? (t.depth ? 1 : 0) : 2;
  uint32_t &label = first->profile_labels[variant];
  if (label == UINT32_MAX) {
    std::string formats;
    for (uint32_t i = 0; i < t.color_count; ++i)
      if (t.colors[i])
        formats += std::string(formats.empty() ? "" : "+") + ProfileFormatName(t.colors[i]->format);
    if (t.depth)
      formats += std::string(formats.empty() ? "" : " + ") + ProfileFormatName(t.depth->format);
    label = gpu_profile::Label(fmt::format("draws {}x{} {} x{}", first->scale.Size(first->width),
                                           first->scale.Size(first->height), formats, uint32_t(first->samples)));
  }
  return label;
}

// fnxf_xiii (2026-10-25): gpu_profile: a resolve of 'surface' by its host size, format and samples ("resolve 1024x576
// RGBA16F x2 colour to texture +clear"), to split the resolves' GPU time by surface. 'variant' as in resolve_labels.
uint32_t ResolveProfileLabel(HostSurface *surface, uint32_t variant) {
  uint32_t &label = surface->resolve_labels[variant];
  if (label == UINT32_MAX) {
    const char *what = variant >= 4 ? (variant == 4 ? "depth resolve (words)" : "depth resolve (depth only)")
                                    : surface->depth ? "resolve depth" : "resolve colour";
    const char *suffix = variant >= 4 ? "" : variant == 0 ? " (no copy)" : variant == 1 ? " to texture"
                                       : variant == 2 ? " clear only" : " to texture +clear";
    label = gpu_profile::Label(fmt::format("{} {}x{} {} x{}{}", what, surface->scale.Size(surface->width),
                                           surface->scale.Size(surface->height), ProfileFormatName(surface->format),
                                           uint32_t(surface->samples), suffix));
  }
  return label;
}

// fnxf_xiii (fnxf_xiii_renut_diag_gpu_shaders, package 28b): the draw's pass split by its pixel shader and blending.
// 2026-10-23: and by its vertex shader; the label carries both hashes (gpu_profile::SetLabelShaders), so the report
// can also rank the shaders across passes (by pixel shader, by vertex shader).
uint32_t ShaderProfileLabel(const Targets &t, const ShaderRecord *vs, const ShaderRecord *ps, const PipelineKey &key) {
  const uint32_t target = TargetsProfileLabel(t);
  // RB_BLENDCONTROL ONE / ZERO / ADD for colour and alpha (BlendDesc's no-blend case).
  const bool blended = key.color_count && (key.blend_control[0] & 0x1FFF1FFF) != 0x00010001;
  const uint64_t vs_hash = vs ? vs->hash : 0, ps_hash = ps ? ps->hash : 0;
  struct Key {
    uint64_t vs, ps;
    uint32_t target;
    bool blended;
    bool operator==(const Key &o) const {
      return vs == o.vs && ps == o.ps && target == o.target && blended == o.blended;
    }
  };
  struct KeyHash {
    size_t operator()(const Key &k) const {
      return size_t(k.vs * 0x9E3779B97F4A7C15ull ^ k.ps ^ (uint64_t(k.target) << 40) ^ (k.blended ? 1ull : 0ull));
    }
  };
  static std::unordered_map<Key, uint32_t, KeyHash> labels;
  const Key k{vs_hash, ps_hash, target, blended};
  if (auto it = labels.find(k); it != labels.end())
    return it->second;
  const std::string name = fmt::format("{} | VS {:016X} | {}{}", gpu_profile::Name(target), vs_hash,
                                       ps ? fmt::format("PS {:016X}", ps_hash) : std::string("no pixel shader"),
                                       blended ? " blended" : "");
  const uint32_t label = gpu_profile::Label(name);
  gpu_profile::SetLabelShaders(label, target, vs_hash, ps_hash);
  labels.emplace(k, label);
  return label;
}

// fnxf_xiii (2026-11-06, fnxf_xiii_motion_vectors): the scene's motion vector target (RG16F, the scene colour target's host
// size and sample count), bound as colour target 1 next to it for the draws that write motion (DrawStateLocked: a scene
// draw with its constants matched, not blended, writing depth; the shaders' output 1, nx_shaders/xenos_glsl.cpp). Cleared
// at its first bind in a frame to zero with alpha 0 (no surface wrote motion there); the shaders write alpha 1.
struct MotionTargetState {
  std::unique_ptr<plume::RenderTexture> texture;
  uint32_t width = 0, height = 0;
  plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  struct Framebuffer {
    const HostSurface *color = nullptr, *depth = nullptr;
    uint64_t color_generation = 0, depth_generation = 0;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::vector<Framebuffer> framebuffers;
  uint64_t frame = 0, cleared_frame = ~uint64_t(0);
  HostSurface *scene = nullptr;        // the colour target it went with in its last frame
  HostSurface *scene_depth = nullptr;  // and the depth target (FSR 3's depth input)
  // fnxf_xiii (FSR 3 at the game's own resolution, DrawStateLocked): a draw to another target since the frame's last
  // scene draw (the post-processing has begun), and the frame FSR last ran in place.
  bool post_started = false;
  uint64_t native_frame = ~uint64_t(0);
  bool wanted = false;                  // the draw being bound writes motion (DrawStateLocked)
  bool failed = false;
};
MotionTargetState g_motion_target;

// fnxf_xiii (2026-11-08, FSR 3.1 before the post-processing): FSR 3.1 before the post-processing of a 1280x720 scene. The game
// resolves its scene's colour target (twice a frame: for the per-tile restores after the opaque pass, and when the scene
// is done) into textures its post-processing samples. The resolves of the scene's target since its last scene draw are
// kept; at the frame's first draw after them into another target (the post-processing's first), FSR 3.1 upscales the
// scene into its target's output-phase variant (1.5x for a 1920x1080 output, else the full 1280x720 without dynamic
// resolution), the output phase begins (the post-processing's 1280x720 targets promoted, as the HUD's were) and those
// resolves are made again from the upscaled picture.
struct SceneResolve {
  u32 dest_va = 0;
  d3d::TextureFetchConstant format{};
  ClearRect src{};
  i32 dest_x = 0, dest_y = 0;
};
struct EarlyFsrState {
  std::vector<SceneResolve> resolves;
  uint64_t frame = ~uint64_t(0);      // the motion frame the resolves are of
  uint64_t ran_frame = ~uint64_t(0);  // the motion frame FSR ran before the post-processing in
};
EarlyFsrState g_early_fsr;
// fnxf_xiii (2026-11-08, fnxf_xiii_diag_load_draw_log): EarlyFsrBeforePostLocked's last outcome in a frame and its
// resolves noted (the [frames] line).
int g_early_reason = 0;
std::vector<std::string> g_load_lines;  // the frame's [drawlog] lines (draws, resolves), written with its [frames] line
uint32_t g_early_noted = 0;
// fnxf_xiii (2026-11-08): of every scene no larger than the output (1280x720, 1600x900, 1920x1080 at a 1920x1080
// output; 1024x576 at both): its post-processing's targets of the scene's size then promoted to the output's size
// (graphics_options.cpp: 6/5, 15/8, 5/4), the game's upscale draw a 1:1 copy. The only FSR 3.1 path (FSR on the
// finished picture, at the game's upscale draw and at a 1280x720 scene's copy, is gone). A scene larger than the output
// (1600x900, 1920x1080 in hand) has no FSR: the game scales it down.
bool EarlyFsrOn() {
  return Fsr3On() && fnxf_xiii::RenderHeight() <= (Output1080p() ? 1080u : 720u);
}
// fnxf_xiii (2026-11-08): the targets of the scene's size (fnxf_xiii_render_height, 16:9) drawn in the output phase are
// promoted: a 1280x720 scene's with a 1920x1080 output or dynamic resolution; another scene's with FSR 3.1 before its
// post-processing.
bool OutputPromotionOn() {
  if (fnxf_xiii::RenderHeight() == 720)
    return Output1080p() || DrsOn();
  return EarlyFsrOn();
}
bool SceneSized(u32 width, u32 height) { return OutputSceneSized(width, height); }  // graphics_options.cpp
uint64_t g_output_copied_frame = ~uint64_t(0);  // the frame the output phase began in (was DrawStateLocked's static)

// The framebuffer of t's colour target 0, the motion target and t's depth; nullptr when it cannot be made.
plume::RenderFramebuffer *MotionFramebufferLocked(const Targets &t) {
  MotionTargetState &m = g_motion_target;
  HostSurface *c = t.colors[0];
  if (m.failed || !c || !c->texture || !t.depth || !t.depth->texture)
    return nullptr;
  const uint32_t w = std::max(c->scale.Size(c->width), 1u), h = std::max(c->scale.Size(c->height), 1u);
  if (!m.texture || m.width != w || m.height != h || m.samples != c->samples) {
    for (auto &f : m.framebuffers)
      Video::RetireLocked(std::move(f.framebuffer));
    m.framebuffers.clear();
    if (m.texture)
      Video::RetireLocked(std::move(m.texture));
    m.texture = State().device->createTexture(plume::RenderTextureDesc::ColorTarget(
        w, h, plume::RenderFormat::R16G16_FLOAT, plume::RenderMultisampling(c->samples)));
    if (!m.texture) {
      m.failed = true;
      RGPU_WARN("[motion] the motion vector target ({}x{}) could not be made: no motion vectors", w, h);
      return nullptr;
    }
    m.texture->setName("fnxf_xiii motion vectors");
    m.width = w;
    m.height = h;
    m.samples = c->samples;
    m.layout = plume::RenderTextureLayout::UNKNOWN;
    m.cleared_frame = ~uint64_t(0);
    RGPU_INFO("[motion] motion vector target {}x{}, {} samples", w, h, u32(c->samples));
  }
  for (auto it = m.framebuffers.begin(); it != m.framebuffers.end();) {
    if (it->color == c && it->depth == t.depth && it->color_generation == c->generation &&
        it->depth_generation == t.depth->generation)
      return it->framebuffer.get();
    // One of its surfaces was made again since: not used again.
    const bool stale = (it->color == c && it->color_generation != c->generation) ||
                       (it->depth == t.depth && it->depth_generation != t.depth->generation);
    if (stale) {
      Video::RetireLocked(std::move(it->framebuffer));
      it = m.framebuffers.erase(it);
    } else {
      ++it;
    }
  }
  const plume::RenderTexture *attachments[2] = {c->texture.get(), m.texture.get()};
  auto framebuffer = State().device->createFramebuffer(plume::RenderFramebufferDesc(attachments, 2, t.depth->texture.get()));
  if (!framebuffer)
    return nullptr;
  if (m.framebuffers.size() >= 8) {
    Video::RetireLocked(std::move(m.framebuffers.front().framebuffer));
    m.framebuffers.erase(m.framebuffers.begin());
  }
  m.framebuffers.push_back({c, t.depth, c->generation, t.depth->generation, std::move(framebuffer)});
  return m.framebuffers.back().framebuffer.get();
}

// Binds t with the motion target as colour target 1 (true), or reports it cannot (false: bound as usual then).
bool BindMotionTargetsLocked(const Targets &t) {
  MotionTargetState &m = g_motion_target;
  plume::RenderFramebuffer *framebuffer = t.color_count == 1 ? MotionFramebufferLocked(t) : nullptr;
  if (!framebuffer)
    return false;
  TransitionSurfaceLocked(t.colors[0], plume::RenderTextureLayout::COLOR_WRITE);
  TransitionSurfaceLocked(t.depth, plume::RenderTextureLayout::DEPTH_WRITE);
  plume::RenderCommandList *cmd = State().command_list;
  if (m.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(m.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
    m.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  if (g_bound.framebuffer != framebuffer) {
    cmd->setFramebuffer(framebuffer);
    g_bound.framebuffer = framebuffer;
  }
  if (t.colors[0]->needs_clear) {
    t.colors[0]->needs_clear = false;
    ++t.colors[0]->writes;
    cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  if (m.cleared_frame != m.frame) {
    m.cleared_frame = m.frame;
    m.scene = t.colors[0];
    m.scene_depth = t.depth;
    m.post_started = false;
    cmd->clearColor(1, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  return true;
}

bool BindTargets(const Targets &t) {
  if (t.depth && t.depth->handed)
    DepthHandOverBeforeBindLocked(t.depth, nullptr, 0);  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover)
  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): a bind that is not a draw's or a clear's own (those see to it before): a
  // colour target whose content a resolve destination holds gets its resolve drawn first.
  if (!g_colour_ho_draw_checked && !g_colour_tracked.empty()) {
    for (uint32_t i = 0; i < t.color_count; ++i)
      if (t.colors[i] && t.colors[i]->handed && !t.colors[i]->depth)
        ColourHandOverBeforeWriteLocked(t.colors[i], 0);
  }
#if defined(PLUME_DEKO3D)
  // fnxf_xiii (2026-11-06, fnxf_xiii_motion_vectors): a draw that writes motion binds the motion target too and writes
  // its red and green; every other draw writes none of a target past its own.
  if (g_motion_target.wanted && BindMotionTargetsLocked(t)) {
    plume::Deko3DSetExtraWriteMask(State().command_list, 0x3u << 4);
    return true;
  }
  if (IsDeko3D())
    plume::Deko3DSetExtraWriteMask(State().command_list, 0);
#endif
  // fnxf_xiii: with fnxf_xiii_renut_diag_gpu_shaders the draw marks its own label (ShaderProfileLabel), the pass's name
  // in it, which saves a timestamp a draw.
  if (!fnxf_xiii::renut_gpu::DiagGpuShadersOn() && gpu_profile::Timed())
    gpu_profile::Mark(State(), TargetsProfileLabel(t));
  for (uint32_t i = 0; i < t.color_count; ++i)
    TransitionSurfaceLocked(t.colors[i], plume::RenderTextureLayout::COLOR_WRITE);
  if (t.depth)
    TransitionSurfaceLocked(t.depth, plume::RenderTextureLayout::DEPTH_WRITE);
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): the last bind's framebuffer again for the same surfaces while no target
  // was built, retired or rebound (their generations with it) and no framebuffer retired: GetFramebufferLocked would
  // find that one.
  struct FramebufferReuse {
    HostSurface *colors[d3d::kMaxRenderTargets] = {};
    uint32_t count = 0;
    HostSurface *depth = nullptr;
    uint64_t surface_epoch = 0, framebuffer_epoch = 0;
    plume::RenderFramebuffer *framebuffer = nullptr;
  };
  static FramebufferReuse last;
  static const bool fb_reuse = REXCVAR_GET(fnxf_xiii_renut_chain_reuse);
  static const bool fb_verify = fb_reuse && REXCVAR_GET(fnxf_xiii_renut_chain_reuse_verify);
  const uint64_t surface_epoch = SurfaceEpochLocked(), framebuffer_epoch = FramebufferEpochLocked();
  bool same = fb_reuse && last.framebuffer && last.count == t.color_count && last.depth == t.depth &&
              last.surface_epoch == surface_epoch && last.framebuffer_epoch == framebuffer_epoch;
  for (uint32_t i = 0; same && i < t.color_count; ++i)
    same = last.colors[i] == t.colors[i];
  plume::RenderFramebuffer *framebuffer = nullptr;
  if (same) {
    framebuffer = last.framebuffer;
    if (fb_verify) {
      plume::RenderFramebuffer *made = GetFramebufferLocked(t.colors, t.color_count, t.depth);
      if (made != framebuffer) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("fnxf_xiii_renut_chain_reuse_verify: the reused framebuffer differs from the one looked up");
        framebuffer = made;
      }
    }
  } else {
    framebuffer = GetFramebufferLocked(t.colors, t.color_count, t.depth);
    if (fb_reuse) {
      for (uint32_t i = 0; i < d3d::kMaxRenderTargets; ++i)
        last.colors[i] = i < t.color_count ? t.colors[i] : nullptr;
      last.count = t.color_count;
      last.depth = t.depth;
      last.surface_epoch = SurfaceEpochLocked();
      last.framebuffer_epoch = FramebufferEpochLocked();
      last.framebuffer = framebuffer;
    }
  }
  if (!framebuffer)
    return false;
  if (fnxf_xiii::renut_gpu::ResolveTraceOn() && g_bound.framebuffer != framebuffer) {
    RGPU_INFO("[trace] targets: {} draws before; now {} colour {:08X} {:08X} ({}x{} fmt {} samples {}), depth {:08X} "
              "({}x{} samples {})",
              g_trace_draws, t.color_count, t.color_count ? t.colors[0]->guest_va : 0,
              t.color_count > 1 ? t.colors[1]->guest_va : 0, t.color_count ? t.colors[0]->width : 0,
              t.color_count ? t.colors[0]->height : 0, t.color_count ? u32(t.colors[0]->format) : 0,
              t.color_count ? u32(t.colors[0]->samples) : 0, t.depth ? t.depth->guest_va : 0,
              t.depth ? t.depth->width : 0, t.depth ? t.depth->height : 0, t.depth ? u32(t.depth->samples) : 0);
    g_trace_draws = 0;
  }
  if (g_bound.framebuffer != framebuffer) {
    State().command_list->setFramebuffer(framebuffer);
    g_bound.framebuffer = framebuffer;
  }
  // fnxf_xiii (2026-10-31): a colour target new since its last bind starts transparent black (HostSurface::needs_clear).
  for (uint32_t i = 0; i < t.color_count; ++i) {
    if (t.colors[i]->needs_clear) {
      t.colors[i]->needs_clear = false;
      ++t.colors[i]->writes;  // fnxf_xiii_renut_resolve_skip
      // fnxf_xiii (2026-11-17, fnxf_xiii_diag_colour_handover_poison): magenta for an image the colour hand-over gave it.
      const bool poison = t.colors[i]->poison;
      t.colors[i]->poison = false;
      State().command_list->clearColor(i, poison ? plume::RenderColor(1.0f, 0.0f, 1.0f, 1.0f)
                                                 : plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
  }
  return true;
}

// A draw's host viewport and scissor. The game's D3D sets the viewport one of
// two ways (N&B database):
// - D3DDevice_SetViewport (0x82222F20) writes scale w/2, -h/2, maxZ - minZ
//   and offset x + w/2, y + h/2, minZ, all enabled in PA_CL_VTE_CNTL: the
//   host viewport is that rectangle, and the shaders pass positions through.
// - D3DRS_VIEWPORTENABLE false (SetRenderState_ViewportEnable, 0x82205A80)
//   disables the transform (and clipping): the vertices are in pixels. The
//   host viewport then covers the target, and the vertex shader maps the
//   pixels to clip space itself (screen_space; any enabled part of the
//   transform comes along as screen_scale / screen_offset).
struct ViewportState {
  // Host pixels (Targets::scale).
  plume::RenderViewport viewport;
  plume::RenderRect scissor;
  // The viewport's size in guest pixels, which screen-space positions and
  // point sizes are in.
  float guest_size[2] = {};
  bool screen_space = false;
  float screen_scale[2] = {1.0f, 1.0f};
  float screen_offset[2] = {0.0f, 0.0f};
  // Guest pixels (Renut upstream 574b488): the pixel position (VPOS) the GPU gives the pixel shader is in the
  // coordinates of the EDRAM it renders to. While tiling that is one tile, and N&B adds the tile's corner back from a
  // per-tile constant (the deferred shadow pass, vs_f6965212d74a7552 / ps_d86a35e4f2b956b3: c0.x = 384 in the lower
  // tile). The host renders every tile into one target, so a draw predicated to one tile has its position made
  // relative to that tile.
  float pixel_origin[2] = {0.0f, 0.0f};
};

// The screen rect of the one tile a draw is predicated to while tiling
// (SetTilesLocked); false for a draw that runs in more tiles, or none.
bool PredicatedTile(const GuestDevice &device, ClearRect &rect) {
  if (g_tiles.size() < 2)
    return false;
  u32 mask = device.U32(d3d::kDeviceOffsetPredication);
  mask = (mask ? mask : ~0u) & g_replay_predication;
  int32_t tile = -1;
  for (size_t i = 0; i < g_tiles.size() && i < 16; ++i) {
    if (!(mask & (3u << (2 * i))))
      continue;
    if (tile >= 0)
      return false;
    tile = int32_t(i);
  }
  if (tile < 0)
    return false;
  rect = g_tiles[tile];
  return true;
}

ViewportState ComputeViewportSlow(const GuestDevice &device, const Targets &t);

// fnxf_xiii (2026-10-19, render-thread proposal 8): the last draw's viewport again when everything ComputeViewportSlow
// reads is the same: the device words (VteControl, the six viewport values, ModeControl, WindowOffset, the window
// scissor, 0x29C0), the predicated tile (worked out first: it reads the replay's predication and the tiles), the
// targets' sizes and scale, and the half-pixel switch.
ViewportState ComputeViewport(const GuestDevice &device, const Targets &t) {
  const d3d::Device &dev = device.dev();
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  ClearRect tile{};
  const bool one_tile = PredicatedTile(device, tile);
  u32 key[24];
  key[0] = dev.m_ControlPacket.VteControl;
  std::memcpy(&key[1], &v.VportXScale, 4);
  std::memcpy(&key[2], &v.VportXOffset, 4);
  std::memcpy(&key[3], &v.VportYScale, 4);
  std::memcpy(&key[4], &v.VportYOffset, 4);
  std::memcpy(&key[5], &v.VportZScale, 4);
  std::memcpy(&key[6], &v.VportZOffset, 4);
  key[7] = u32(dev.m_ControlPacket.ModeControl);
  key[8] = dev.m_WindowPacket.WindowOffset;
  key[9] = dev.m_WindowPacket.WindowScissorTL;
  key[10] = dev.m_WindowPacket.WindowScissorBR;
  key[11] = device.U32(0x29C0);
  key[12] = u32(one_tile) | (fnxf_xiii::renut_gpu::HalfPixelOffsetOn() ? 2u : 0u);
  key[13] = one_tile ? u32(tile.x1) : 0;
  key[14] = one_tile ? u32(tile.y1) : 0;
  key[15] = one_tile ? u32(tile.x2) : 0;
  key[16] = one_tile ? u32(tile.y2) : 0;
  key[17] = t.width;
  key[18] = t.height;
  key[19] = t.guest_width;
  key[20] = t.guest_height;
  key[21] = t.scale.Key();
  key[22] = t.scale.drs ? std::bit_cast<u32>(g_drs_factor) : 0u;  // fnxf_xiii (2026-11-07, dynamic resolution)
  key[23] = 0;
  static u32 last_key[24];
  static ViewportState last;
  static bool valid = false;
  if (valid && std::memcmp(key, last_key, sizeof(key)) == 0)
    return last;
  last = ComputeViewportSlow(device, t);
  std::memcpy(last_key, key, sizeof(key));
  valid = true;
  return last;
}

ViewportState ComputeViewportSlow(const GuestDevice &device, const Targets &t) {
  const d3d::Device &dev = device.dev();
  const u32 vte = dev.m_ControlPacket.VteControl;
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  const float xs = (vte & 0x01) ? float(v.VportXScale) : 1.0f;
  float xo = (vte & 0x02) ? float(v.VportXOffset) : 0.0f;
  const float ys = (vte & 0x04) ? float(v.VportYScale) : 1.0f;
  float yo = (vte & 0x08) ? float(v.VportYOffset) : 0.0f;
  const float zs = (vte & 0x10) ? float(v.VportZScale) : 1.0f;
  const float zo = (vte & 0x20) ? float(v.VportZOffset) : 0.0f;

  // PA_SU_SC_MODE_CNTL.vtx_window_offset_enable adds PA_SC_WINDOW_OFFSET.
  int32_t wx = 0, wy = 0;
  if (u32(dev.m_ControlPacket.ModeControl) & 0x10000) {
    const u32 offset = dev.m_WindowPacket.WindowOffset;
    wx = int32_t(offset << 17) >> 17;
    wy = int32_t(offset << 1) >> 17;
    xo += float(wx);
    yo += float(wy);
  }
  // A draw predicated to one tile renders into that tile's EDRAM only, which
  // the GPU resolves to the tile's rect of the screen: it is cut to the rect.
  // Most such draws are in the tile's own coordinates, a viewport the size of
  // the tile-sized surface (N&B's per-tile scene command buffers: the vehicle
  // parts), which the host offsets to the rect. A viewport larger than that
  // surface addresses the whole screen (N&B's water, sub_823BA3A8, drawn once
  // per tile with a full-screen viewport and that tile's constants): it stays.
  ClearRect tile_rect{};
  const bool one_tile = PredicatedTile(device, tile_rect);
  // fnxf_xiii (2026-10-28): the viewport the device holds tells it whether or not the transform is on (VteControl
  // 0x0F). The game at 1280x720 (two tiles) restores its scene and depth after the opaque pass with one quad per tile,
  // the transform off (vte 00) and the full-screen viewport set: positions in screen pixels. Taken as tile-local,
  // the second tile's quads were moved a tile further right, past its scissor, and not drawn: the right half kept
  // what the passes in between wrote over that EDRAM, and the effects drawn next were depth-tested against it (the
  // blinking checkered blocks and glows in x >= 640).
  const float raw_xs = float(v.VportXScale), raw_ys = float(v.VportYScale);
  // Larger than the tile (whose EDRAM the tile-sized surface is): the whole screen.
  const bool screen_wide = one_tile && (2.0f * std::fabs(raw_xs) > float(tile_rect.x2 - tile_rect.x1) + 0.5f ||
                                        2.0f * std::fabs(raw_ys) > float(tile_rect.y2 - tile_rect.y1) + 0.5f);
  const bool tile_local = one_tile && !screen_wide;
  if (one_tile && (vte & 0x0F) != 0x0F) {
    static uint32_t logged = 0;
    if (logged++ < 4 && REXCVAR_GET(log_diagnostics))
      RGPU_INFO("[tiles] a one-tile draw without the viewport transform (vte {:02X}): tile {},{}-{},{}, viewport scale "
                "{} x {}, surface {}x{}: {}",
                vte, tile_rect.x1, tile_rect.y1, tile_rect.x2, tile_rect.y2, raw_xs, raw_ys, t.guest_width,
                t.guest_height, screen_wide ? "screen coordinates" : "tile coordinates");
  }
  int32_t tx = 0, ty = 0;
  if (tile_local) {
    tx = tile_rect.x1;
    ty = tile_rect.y1;
  }
  xo += float(tx);
  yo += float(ty);

  ViewportState vs;
  if (one_tile) {  // Renut upstream 574b488 (the Jinjo Bingo silhouette)
    vs.pixel_origin[0] = float(tile_rect.x1);
    vs.pixel_origin[1] = float(tile_rect.y1);
  }
  const float min_z = std::clamp(zo, 0.0f, 1.0f), max_z = std::clamp(zo + zs, 0.0f, 1.0f);
  // fnxf_xiii (2026-10-18, fnxf_xiii_renut_half_pixel_offset): PA_SU_VTX_CNTL.pix_center (device 0x29C0 bit 0, which
  // D3DRS_HALFPIXELOFFSET sets) 0 puts the pixel centres at integer window coordinates (D3D9's), so the GPU's window
  // positions are the host's plus half a pixel, as Xenia offsets the viewport (draw_util). Without it everything drew
  // half a pixel up-left; the depth of field's 320x180 passes, whose blur taps are one-sided, shifted their result a
  // whole texel: the orange halo above Kazooie's crest. Not in the scissor (whole pixels).
  const float pixel_center = (fnxf_xiii::renut_gpu::HalfPixelOffsetOn() && !(device.U32(0x29C0) & 1)) ? 0.5f : 0.0f;
  // SetViewport's rectangle back from its scale and offset: x = offset - scale
  // and width = 2 * scale; y = offset + scale and height = -2 * scale (the
  // scale is negative: y goes down).
  vs.screen_space = (vte & 0x0F) != 0x0F || !(xs > 0.0f) || !(ys < 0.0f);
  if (!vs.screen_space) {
    vs.viewport = plume::RenderViewport(xo + pixel_center - xs, yo + pixel_center + ys, 2.0f * xs, -2.0f * ys,
                                        min_z, max_z);
  } else {
    vs.viewport = plume::RenderViewport(0.0f, 0.0f, float(t.width), float(t.height), min_z, max_z);
    vs.screen_scale[0] = xs;
    vs.screen_scale[1] = ys;
    vs.screen_offset[0] = xo + pixel_center;
    vs.screen_offset[1] = yo + pixel_center;
  }

  // PA_SC_WINDOW_SCISSOR_TL/BR: x 0-14, y 16-30.
  const u32 tl = dev.m_WindowPacket.WindowScissorTL;
  const u32 br = dev.m_WindowPacket.WindowScissorBR;
  int32_t sx1 = int32_t(tl & 0x7FFF) + wx + tx, sy1 = int32_t((tl >> 16) & 0x7FFF) + wy + ty;
  int32_t sx2 = int32_t(br & 0x7FFF) + wx + tx, sy2 = int32_t((br >> 16) & 0x7FFF) + wy + ty;
  // A target grown for tiling: D3D bounds the window scissor by the tile-sized
  // surface, and the GPU applies it per tile in window coordinates, so across
  // the tiles that bound covers the whole target (a one-tile draw is cut to
  // its tile below; one in tile coordinates has the bound offset there).
  if (!tile_local && t.guest_width && t.width > t.guest_width && sx2 >= int32_t(t.guest_width))
    sx2 = int32_t(t.width);
  if (!tile_local && t.guest_height && t.height > t.guest_height &&
      sy2 >= int32_t(t.guest_height))
    sy2 = int32_t(t.height);
  // A screen-space draw's clip space spans the whole target: what a partly
  // enabled transform would clip is cut by the scissor instead.
  if (vs.screen_space && (vte & 0x0F) == 0x0F) {
    sx1 = std::max(sx1, int32_t(std::floor(xo - std::fabs(xs))));
    sx2 = std::min(sx2, int32_t(std::ceil(xo + std::fabs(xs))));
    sy1 = std::max(sy1, int32_t(std::floor(yo - std::fabs(ys))));
    sy2 = std::min(sy2, int32_t(std::ceil(yo + std::fabs(ys))));
  }
  if (one_tile) {
    sx1 = std::max(sx1, tile_rect.x1);
    sy1 = std::max(sy1, tile_rect.y1);
    sx2 = std::min(sx2, tile_rect.x2);
    sy2 = std::min(sy2, tile_rect.y2);
  }
  sx1 = std::clamp(sx1, 0, int32_t(t.width));
  sy1 = std::clamp(sy1, 0, int32_t(t.height));
  sx2 = std::clamp(sx2, sx1, int32_t(t.width));
  sy2 = std::clamp(sy2, sy1, int32_t(t.height));

  // All of the above is in guest pixels; the host targets are t.scale times
  // that. Depth is not scaled. fnxf_xiii (2026-11-07): as drawn this frame (dynamic resolution: RenderScale::drs).
  vs.guest_size[0] = vs.viewport.width;
  vs.guest_size[1] = vs.viewport.height;
  const float scale = t.scale.DrawFactor();
  vs.viewport.x *= scale;
  vs.viewport.y *= scale;
  vs.viewport.width *= scale;
  vs.viewport.height *= scale;
  vs.scissor = ScaleRect(plume::RenderRect(sx1, sy1, sx2, sy2), t.scale);
  return vs;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Where a draw's float constant registers are (ReplayConstants).
struct FloatConstants {
  const u8 *state = nullptr;
  const u8 *running = nullptr;  // null: all in 'state'
  uint64_t set = 0;
  u32 offset = 0;  // of VertexShaderF or PixelShaderF in the device layout
  uint64_t *dirty = nullptr;    // ReplayConstants::vs_dirty / ps_dirty
  uint64_t replay = 0;

  const u8 *Register(u32 r) const {
    const bool own = !running || ((set >> (63 - r / 4)) & 1);
    return (own ? state : running) + offset + r * 16;
  }
};

FloatConstants DrawFloatConstants(const u8 *state, const ReplayConstants *replay, bool pixel) {
  FloatConstants c;
  c.state = state;
  c.offset = u32(offsetof(d3d::Device, m_Constants)) +
             u32(pixel ? offsetof(d3d::Constants, PixelShaderF) : offsetof(d3d::Constants, VertexShaderF));
  if (replay) {
    c.running = replay->running;
    c.set = pixel ? replay->ps_set : replay->vs_set;
    c.dirty = pixel ? replay->ps_dirty : replay->vs_dirty;
    c.replay = c.dirty ? replay->replay : 0;
  }
  return c;
}

// Float constants go by groups of four registers (64 bytes): the unit m_Pending
// marks dirty, and the one a replayed draw takes from its snapshot or from the
// running device (FloatConstants::Register), so a group's bytes are contiguous.
// Bit 63 - group, as in m_Pending.
constexpr u32 kConstantGroupBytes = 4 * 16;

// The groups holding a register the shader reads.
uint64_t UsedGroups(const u32 used[8]) {
  uint64_t groups = 0;
  for (u32 word = 0; word < 8; ++word) {
    for (u32 bits = used[word]; bits;) {
      const u32 k = u32(std::countr_zero(bits)) / 4;
      groups |= uint64_t(1) << (63 - (word * 8 + k));
      bits &= ~(0xFu << (k * 4));
    }
  }
  return groups;
}

// fnxf_xiii (2026-10-19, render-thread proposal 12): UsedGroups and the registers of those groups (an upload's 'valid')
// once per shader: 'used' is a shader's ShaderCacheEntry::floatConstants (static) or one of the two static masks, so
// its address names it.
struct GroupSummary {
  const u32 *used = nullptr;
  uint64_t groups = 0;
  u32 valid[8] = {};
  // fnxf_xiii (2026-10-19, render-thread proposal 10): the bytes an upload must hold for the shader: all 257 registers
  // (with the zero register 256 that indexed reads past the end reach) when it indexes (every register marked used),
  // else up to its last group.
  uint32_t bytes = 0;
};
GroupSummary g_group_summaries[512];

const GroupSummary &SummaryOf(const u32 used[8]) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(used);
  GroupSummary &g = g_group_summaries[((a >> 3) ^ (a >> 12)) & 511];
  if (g.used != used) {
    g.groups = UsedGroups(used);
    for (u32 word = 0; word < 8; ++word) {
      const u32 word_groups = u32(g.groups >> (56 - word * 8)) & 0xFFu;  // group word * 8 at bit 7
      u32 valid = 0;
      for (u32 k = 0; k < 8; ++k) {
        if ((word_groups >> (7 - k)) & 1)
          valid |= 0xFu << (k * 4);
      }
      g.valid[word] = valid;
    }
    bool all = true;
    for (u32 word = 0; word < 8; ++word)
      all &= used[word] == ~0u;
    g.bytes = all ? kFloatConstantsUboBytes
          : g.groups ? (64u - u32(std::countr_zero(g.groups))) * u32(kConstantGroupBytes)  // last group + 1
                     : 16u;
    g.used = used;
  }
  return g;
}

// The registers of a group the shader reads, bit i = the group's register i.
u32 GroupRegisters(const u32 used[8], u32 group) {
  return (used[group / 8] >> (group % 8 * 4)) & 0xFu;
}

// The registers a shader reads ('used', ShaderCacheEntry::floatConstants) as a
// little-endian upload of all 256, reusing the last upload on this command list
// when it holds them with the same bytes. Only the groups holding a register
// the shader reads are written, whole: fixed-size copies the compiler keeps
// inline, where a run of registers at a time cost a branch per register.
UploadAllocation UploadFloatConstants(const FloatConstants &constants, const u32 used[8],
                                      ConstantUpload &last, const ShaderCacheEntry *entry) {
  // fnxf_xiii (fnxf_xiii_gpu_literals_cbuf): the shader's literal registers it reads from the constant buffer hold its
  // literal values, as the console's Direct3D loads them at SetShader. An upload holding another shader's (or none) is
  // not reused for it.
  const ShaderCacheEntry *literals = entry && entry->literalCount ? entry : nullptr;
  bool reuse = bool(last.alloc) && (!literals || last.literals == literals);
  for (u32 word = 0; reuse && word < 8; ++word)
    reuse = !(used[word] & ~last.valid[word]);
  // Within the replay that last matched 'last', only groups written since can
  // differ (the running device is the same throughout a replay).
  const bool tracked = constants.replay && last.replay == constants.replay;
  const uint64_t dirty = tracked ? *constants.dirty : ~uint64_t(0);
  const GroupSummary &summary = SummaryOf(used);  // fnxf_xiii (2026-10-19, render-thread proposal 12)
  const uint64_t groups = summary.groups;
  // Only the registers the shader reads count: the rest of a group may be
  // another shader's, and differ.
  for (uint64_t check = reuse ? groups & dirty : 0; reuse && check; check &= check - 1) {
    const u32 group = 63 - u32(std::countr_zero(check));
    const u32 registers = GroupRegisters(used, group);
    const u8 *cached = last.bytes + group * kConstantGroupBytes;
    const u8 *guest = constants.Register(group * 4);
    uint64_t diff = 0;
    for (u32 i = 0; i < 4; ++i) {
      if (!((registers >> i) & 1))
        continue;
      uint64_t a[2], b[2];
      std::memcpy(a, cached + i * 16, 16);
      std::memcpy(b, guest + i * 16, 16);
      diff |= (a[0] ^ b[0]) | (a[1] ^ b[1]);
    }
    reuse = diff == 0;
  }
  if (reuse) {
    if (constants.replay && !tracked) {
      // Every register the draw reads now matches: keep just those, and
      // track the replay's writes from here.
      std::memcpy(last.valid, used, sizeof(last.valid));
      last.replay = constants.replay;
      *constants.dirty = 0;
    }
    return last.alloc;
  }

  // fnxf_xiii (2026-10-19, render-thread proposal 10): only the bytes the shader reads (its groups, its literal
  // registers; all of them when it indexes) are allocated, so only those are cleaned at the submit; the uniform buffer
  // stays bound at its full size, which the allocation keeps room for in the chunk. The registers past what is
  // allocated are never read by this shader (it reads only those in floatConstants, all of them when it indexes).
  uint32_t need = summary.bytes;
  if (literals) {
    for (u32 i = 0; i < literals->literalCount; ++i) {
      const ShaderLiteral &l = g_shaderLiterals[literals->literalOffset + i];
      if (l.reg < 256)
        need = std::max(need, (l.reg + 1) * 16u);
    }
  }
  UploadAllocation alloc = Video::UploadsLocked().AllocateWithRoom(need, kFloatConstantsUboBytes, 256);
  if (!alloc)
    return alloc;
  auto *out = static_cast<u8 *>(alloc.cpu);
  // fnxf_xiii (2026-10-18): register 256, zero (pass bonerow of tools/deko3d_glsl_opt.py), for a shader that indexes.
  if (need > 256 * 16)
    std::memset(out + 256 * 16, 0, kFloatConstantsUboBytes - 256 * 16);
  // The upload memory is write-combined: written in order, never read.
  for (uint64_t remaining = groups; remaining;) {
    const u32 group = u32(std::countl_zero(remaining));
    remaining &= ~(uint64_t(1) << (63 - group));
    u32 lanes[kConstantGroupBytes / 4];
    std::memcpy(lanes, constants.Register(group * 4), kConstantGroupBytes);
    std::memcpy(last.bytes + group * kConstantGroupBytes, lanes, kConstantGroupBytes);
    for (u32 &lane : lanes)
      lane = std::byteswap(lane);
    std::memcpy(out + group * kConstantGroupBytes, lanes, kConstantGroupBytes);
  }
  // Every register of those groups is in the upload, with the bytes it has
  // now.
  std::memcpy(last.valid, summary.valid, sizeof(last.valid));
  // fnxf_xiii (fnxf_xiii_gpu_literals_cbuf): the literal registers, after the groups (a group may hold one). They are not
  // the device's values: out of 'valid', so a shader reading them as registers takes a new upload.
  last.literals = literals;
  if (literals) {
    for (u32 i = 0; i < literals->literalCount; ++i) {
      const ShaderLiteral &l = g_shaderLiterals[literals->literalOffset + i];
      if (l.reg >= 256)
        continue;
      std::memcpy(out + l.reg * 16, l.value, 16);
      last.valid[l.reg / 32] &= ~(1u << (l.reg % 32));
    }
  }
  last.alloc = alloc;
  last.replay = constants.replay;
  if (constants.replay)
    *constants.dirty = 0;
  return alloc;
}

// ---------------------------------------------------------------------------
// Pushed constants (fnxf_xiii, 2026-10-19, fnxf_xiii_deko3d_push_constants; render-thread proposals 2 and 5)
// ---------------------------------------------------------------------------
// On deko3d the vertex shader's float constants, the pixel shader's and SharedConstants live in three fixed uniform
// buffers (zeroed once, so register 256 stays zero), bound at the bindings the game shaders read (0, 1, 2) and updated
// with pipelined constant-buffer updates (plume::Deko3DPushUniform: dkCmdBufPushConstants; a draw reads the values
// pushed before it in command order). ConstantUpload is then a model of what the GPU's buffer holds after the list's
// updates so far (valid: registers holding the device's bytes in 'bytes'; literal_regs: registers holding 'literals''s
// literal values), reset with each list (Bound). A draw pushes each 64-byte group holding a register the shader reads
// that is not valid or whose guest bytes differ; within a command buffer replay, a valid group the replay has not
// written since (its dirty mask) needs no compare. Then the shader's literal registers, unless they hold them already.
// Every register a shader reads holds what UploadFloatConstants would have written: the device's bytes, byte-swapped,
// or the literal. SharedConstants: the 64-byte lines that differ from what the buffer holds; plume reads bound
// textures from the renderer's copy (Deko3DSetSharedWords). Draws whose point_sprite_gs reads SharedConstants through
// its address keep the ring uploads (an update is ordered for constant-bank reads, not for memory reads).
#if defined(PLUME_DEKO3D)
struct PushedBuffers {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint64_t address = 0;
};
PushedBuffers g_pushed_buffers;
constexpr uint64_t kPushedFloatStride = (kFloatConstantsUboBytes + 255u) & ~uint64_t(255);
constexpr uint64_t kPushedSharedOffset = 2 * kPushedFloatStride;

bool EnsurePushedBuffers() {
  if (g_pushed_buffers.address)
    return true;
  auto &s = State();
  const uint64_t size = kPushedSharedOffset + ((sizeof(SharedConstants) + 255u) & ~uint64_t(255));
  g_pushed_buffers.buffer = s.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(size));
  if (!g_pushed_buffers.buffer)
    return false;
  if (void *cpu = g_pushed_buffers.buffer->map()) {
    std::memset(cpu, 0, size);  // register 256 (and everything not pushed yet) zero
    g_pushed_buffers.buffer->unmap();
  }
  g_pushed_buffers.address = g_pushed_buffers.buffer->getDeviceAddress();
  return g_pushed_buffers.address != 0;
}

// The registers of the groups in a byte of a group mask (group word * 8 + k at bit 7 - k, as SummaryOf takes them),
// as a 'valid' word.
constexpr std::array<u32, 256> kGroupByteRegisters = [] {
  std::array<u32, 256> t{};
  for (u32 b = 0; b < 256; ++b) {
    for (u32 k = 0; k < 8; ++k) {
      if ((b >> (7 - k)) & 1)
        t[b] |= 0xFu << (k * 4);
    }
  }
  return t;
}();

// One stage's float constants (see above); 'ubo' its buffer's address.
void PushFloatConstants(plume::RenderCommandList *cmd, const FloatConstants &constants, const u32 used[8],
                        ConstantUpload &last, const ShaderCacheEntry *entry, uint64_t ubo) {
  const GroupSummary &summary = SummaryOf(used);
  const ShaderCacheEntry *literals = entry && entry->literalCount ? entry : nullptr;
  // The shader's literal registers, when the buffer holds them (literal_regs is exactly one shader's): a shader that
  // indexes reads every register, its literal ones included, and those hold the literal, not the device's bytes.
  const u32 *own_literals = literals && last.literals == literals ? last.literal_regs : nullptr;
  const bool tracked = constants.replay && last.replay == constants.replay;
  const uint64_t dirty = tracked ? *constants.dirty : ~uint64_t(0);
  // The groups to update (bit 63 - group, as UsedGroups).
  uint64_t push = 0;
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_snapshot_deltas_constants): within a tracked replay (the chain's draws are one) with
  // none of the shader's groups dirty, a group is pushed only for a register the buffer does not hold: when every
  // register the shader reads (but its own literal ones) is held, the loop below would push nothing. Word by word.
  bool all_held = tracked && (dirty & summary.groups) == 0;
  for (u32 w = 0; all_held && w < 8; ++w)
    all_held = (used[w] & ~(own_literals ? own_literals[w] : 0u) & ~last.valid[w]) == 0;
  for (uint64_t check = all_held ? 0 : summary.groups; check; check &= check - 1) {
    const u32 bit = u32(std::countr_zero(check));
    const u32 group = 63 - bit;
    const u32 lits = own_literals ? (own_literals[group / 8] >> (group % 8 * 4)) & 0xFu : 0u;
    const u32 registers = GroupRegisters(used, group) & ~lits;
    const u32 held = (last.valid[group / 8] >> (group % 8 * 4)) & 0xFu;
    if (registers & ~held) {
      push |= uint64_t(1) << bit;
      continue;
    }
    if (!((dirty >> bit) & 1))
      continue;  // valid, and the replay has not written it since
    const u8 *cached = last.bytes + group * kConstantGroupBytes;
    const u8 *guest = constants.Register(group * 4);
    uint64_t diff = 0;
    for (u32 i = 0; i < 4; ++i) {
      if (!((registers >> i) & 1))
        continue;
      uint64_t a[2], b[2];
      std::memcpy(a, cached + i * 16, 16);
      std::memcpy(b, guest + i * 16, 16);
      diff |= (a[0] ^ b[0]) | (a[1] ^ b[1]);
    }
    if (diff)
      push |= uint64_t(1) << bit;
  }
  // The updates, a run of consecutive groups at a time.
  uint64_t pushed_bytes = 0;
  bool overwrote_literal = false;
  for (uint64_t remaining = push; remaining;) {
    const u32 first = u32(std::countl_zero(remaining));  // the lowest group left
    u32 count = 0;
    while (first + count < 64 && ((remaining >> (63 - (first + count))) & 1))
      ++count;
    alignas(16) u32 lanes[64 * kConstantGroupBytes / 4];
    for (u32 k = 0; k < count; ++k) {
      const u32 group = first + k;
      const u8 *guest = constants.Register(group * 4);
      std::memcpy(last.bytes + group * kConstantGroupBytes, guest, kConstantGroupBytes);
      std::memcpy(lanes + k * (kConstantGroupBytes / 4), guest, kConstantGroupBytes);
      last.valid[group / 8] |= 0xFu << (group % 8 * 4);
      const u32 lits = (last.literal_regs[group / 8] >> (group % 8 * 4)) & 0xFu;
      if (lits) {
        overwrote_literal = true;
        last.literal_regs[group / 8] &= ~(0xFu << (group % 8 * 4));
      }
      remaining &= ~(uint64_t(1) << (63 - group));
    }
    for (u32 k = 0; k < count * (kConstantGroupBytes / 4); ++k)
      lanes[k] = std::byteswap(lanes[k]);
    plume::Deko3DPushUniform(cmd, ubo, kFloatConstantsUboBytes, first * kConstantGroupBytes, lanes,
                             count * kConstantGroupBytes);
    pushed_bytes += count * kConstantGroupBytes;
  }
  // The literal registers: after the groups (a group may hold one, and its update overwrote it).
  if (overwrote_literal && last.literals != literals)
    last.literals = nullptr;  // another shader's literals, no longer all in place
  if (literals && (last.literals != literals || overwrote_literal)) {
    if (last.literals != literals) {
      // Another shader's literal registers left in the buffer are out of 'valid' already; literal_regs is this one's.
      for (u32 &word : last.literal_regs)
        word = 0;
    }
    for (u32 i = 0; i < literals->literalCount; ++i) {
      const ShaderLiteral &l = g_shaderLiterals[literals->literalOffset + i];
      if (l.reg >= 256)
        continue;
      plume::Deko3DPushUniform(cmd, ubo, kFloatConstantsUboBytes, l.reg * 16, l.value, 16);
      pushed_bytes += 16;
      last.valid[l.reg / 32] &= ~(1u << (l.reg % 32));
      last.literal_regs[l.reg / 32] |= 1u << (l.reg % 32);
    }
    last.literals = literals;
  }
  // The replay's writes from here: the next draw of this replay skips a valid group the replay has not written since,
  // so 'valid' keeps only registers that hold the device's bytes now: those of the groups pushed, those this draw
  // compared, and (tracked) those of groups the replay has not written since this replay's last draw. A register of a
  // group compared for another (a shader reads some of a group's registers) is not known (emulator verify run 61: 8 %
  // of the draws read one stale).
  if (constants.replay) {
    const uint64_t unsure = (tracked ? dirty : ~uint64_t(0)) & ~push;
    // fnxf_xiii (2026-10-20, from sa2-nx): with no group unsure (a chain draw whose groups are all clean) the loop changes nothing.
    for (u32 word = 0; unsure && word < 8; ++word)
      last.valid[word] &= ~kGroupByteRegisters[u32(unsure >> (56 - word * 8)) & 0xFFu] | used[word];
    last.replay = constants.replay;
    *constants.dirty = 0;
  } else {
    last.replay = 0;
  }
  fnxf_xiii::renut_gpu::CountPushedConstants(pushed_bytes, 0);
  if (fnxf_xiii::renut_gpu::PushConstantsVerifyOn()) {
    bool ok = true;
    const u32 *literal_regs = literals && last.literals == literals ? last.literal_regs : nullptr;
    for (u32 r = 0; r < 256 && ok; ++r) {
      if (!((used[r / 32] >> (r % 32)) & 1))
        continue;
      if (literal_regs && ((literal_regs[r / 32] >> (r % 32)) & 1))
        continue;  // the literal (checked below)
      ok = ((last.valid[r / 32] >> (r % 32)) & 1) &&
           std::memcmp(last.bytes + r * 16, constants.Register(r), 16) == 0;
    }
    if (literals) {
      for (u32 i = 0; i < literals->literalCount && ok; ++i) {
        const u32 reg = g_shaderLiterals[literals->literalOffset + i].reg;
        ok = reg >= 256 || (last.literals == literals && ((last.literal_regs[reg / 32] >> (reg % 32)) & 1));
      }
    }
    if (!ok) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        RGPU_WARN("[pushed] a register the shader reads is not what the model says the GPU holds (fnxf_xiii_deko3d_push_"
                  "constants_verify)");
      fnxf_xiii::renut_gpu::CountPushedConstants(1, 3);
    }
  }
}

// SharedConstants: the lines that differ from what the buffer holds.
// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_inline_line_compare): whether a line of the shared constants differs from the held
// one: a whole 64-byte line as fixed-size loads the compiler keeps inline (a memcmp call per line before: 21 calls a
// draw, the render thread's memcmp of the first console profile); the 16-byte tail by memcmp.
inline bool SharedLineDiffers(const u8 *a, const u8 *b, u32 n) {
  static const bool inline_on = REXCVAR_GET(fnxf_xiii_renut_inline_line_compare);
  static const bool neon = REXCVAR_GET(fnxf_xiii_renut_neon_compare);  // fnxf_xiii (2026-10-20, from sa2-nx): sa2_line_compare.h
  if (n != 64 || !inline_on)
    return std::memcmp(a, b, n) != 0;
  return fnxf_xiii::renut_gpu::Line64Differs(a, b, neon);
}

// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): the pushes counted; 'equal_lines' (bit i = 64-byte line i) are lines the
// buffer holds already: the texture words of a chain draw that are the previous draw's when it pushed last, and the
// vertex fetch slots a draw without fetches leaves zero when the last push's were zero too.
uint64_t g_shared_pushes = 0;
bool g_held_fetch_slots_zero = false;  // the buffer's vertex fetch slots (lines 9-14) are zero
constexpr u32 kTextureLines = 0xFFu;      // lines 0-7: the texture and sampler words
constexpr u32 kFetchSlotLines = 0x7E00u;  // lines 9-14: vertex_fetch_slots
static_assert(offsetof(SharedConstants, vertex_fetch_slots) == 9 * 64 &&
              sizeof(SharedConstants::vertex_fetch_slots) == 6 * 64);

void PushSharedConstants(plume::RenderCommandList *cmd, const SharedConstants &sc, u32 equal_lines = 0) {
  constexpr u32 kLine = 64;
  constexpr u32 kSize = u32(sizeof(SharedConstants));
  ++g_shared_pushes;
  const uint64_t ubo = g_pushed_buffers.address + kPushedSharedOffset;
  const u8 *now = reinterpret_cast<const u8 *>(&sc);
  u8 *held = reinterpret_cast<u8 *>(&g_bound.pushed_shared);
  uint64_t pushed_bytes = 0;
  if (!g_bound.pushed_shared_valid) {
    plume::Deko3DPushUniform(cmd, ubo, kSize, 0, now, kSize);
    std::memcpy(held, now, kSize);
    g_bound.pushed_shared_valid = true;
    pushed_bytes = kSize;
  } else {
    for (u32 at = 0; at < kSize;) {
      const u32 n = std::min(kLine, kSize - at);
      if (((equal_lines >> (at / kLine)) & 1u) || !SharedLineDiffers(held + at, now + at, n)) {
        at += n;
        continue;
      }
      // A run of lines that differ.
      u32 end = at + n;
      while (end < kSize) {
        const u32 m = std::min(kLine, kSize - end);
        if (((equal_lines >> (end / kLine)) & 1u) || !SharedLineDiffers(held + end, now + end, m))
          break;
        end += m;
      }
      plume::Deko3DPushUniform(cmd, ubo, kSize, at, now + at, end - at);
      std::memcpy(held + at, now + at, end - at);
      pushed_bytes += end - at;
      at = end;
    }
  }
  fnxf_xiii::renut_gpu::CountPushedConstants(pushed_bytes, 1);
}
#endif  // PLUME_DEKO3D

TextureFetch DeviceTextureFetch(const d3d::Device &dev, u32 r) {
  return TextureFetch::FromGuest(dev.m_Constants.TextureFetch[r]);
}

// fnxf_xiii (deko3d renderer, docs/deko3d-renderer.md): the sampler words hold the sampler id where a texture handle has
// it (bit 20 up), so the deko3d shaders make the handle as (texture word & 0xFFFFF) | (sampler word & 0xFFF00000): no
// shift, and the form Ryujinx's shader translator follows back to the constant buffer (the emulator tests).
inline u32 SamplerWord(u32 slot) { return IsDeko3D() ? slot << 20 : slot; }

// fnxf_xiii (2026-10-19, CPU proposal 2, fnxf_xiii_renut_cpu_memos): FillTextures' answer per fetch slot (the shared-constant
// index and sampler words), keyed by the texture address, the device's fetch constant and the guest header's words,
// valid for the frame (the first bind of a frame runs the content check) while g_texture_epoch is unchanged. A hit does
// the bind's transitions and counters again.
struct TextureSlotMemo {
  u32 va = 0;
  u32 device[6] = {};
  u32 header[6] = {};
  uint64_t frame = 0;
  uint64_t epoch = 0;
  uint64_t depth_epoch = 0;  // fnxf_xiii (2026-10-19, render-thread proposal 3): for binds through depth words
  u32 dim = 0;
  u32 index = 0;
  u32 sampler = 0;
  BindEffects fx;
};
// fnxf_xiii (2026-10-19, render-thread proposal 3): by texture (its header address and the fetch constant's first
// words), not by fetch slot: a texture bound earlier in the frame, through any slot, is a hit.
constexpr uint32_t kTextureMemoEntries = 512;
TextureSlotMemo g_texture_slot_memo[kTextureMemoEntries];

// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): 'slot_fx' (32 entries, or null) receives each bound slot's effects (its
// memo entry's on a hit, the bind's otherwise) and 'all_memo' whether every bound slot went through the memo (a
// texture header to key it), for the chain's reuse of the next draw.
// fnxf_xiii (2026-11-07): set while a draw's colour target is drawn larger than the guest's (the 1280x720 targets at 1.5x
// for a 1080p output): its point-magnified textures (the HUD's and the menus' pictures and fonts, made for a texel a
// pixel at 1280x720) are magnified linearly instead: point sampling at 1.5x repeated every other texel (blocky,
// 2026-11-07 on a TV). FillTextures puts it in the fetch's words (the memo and the sampler key on them).
bool g_mag_linear = false;

void FillTextures(const GuestDevice &device, u32 fetch_mask, SharedConstants &sc, BindEffects *slot_fx = nullptr,
                  bool *all_memo = nullptr, const FetchFilterOverrides *filters = nullptr) {
  if (all_memo)
    *all_memo = fnxf_xiii::renut_gpu::CpuMemosOn();
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_texture_defaults_copy): the null words as one copy of a block made once (160 stores a
  // draw before).
  struct DefaultWords {
    u32 indices[3][32];
    u32 samplers[32];
  };
  static const DefaultWords kDefaults = [] {
    DefaultWords w{};
    for (u32 r = 0; r < 32; ++r) {
      w.indices[0][r] = kNullTextureSlot;
      w.indices[1][r] = kNullTexture3DSlot;
      w.indices[2][r] = kNullTextureCubeSlot;
      w.samplers[r] = SamplerWord(kLinearClampSamplerSlot);
    }
    return w;
  }();
  static const bool defaults_copy = REXCVAR_GET(fnxf_xiii_renut_texture_defaults_copy);
  if (defaults_copy) {
    std::memcpy(sc.texture_indices, kDefaults.indices, sizeof(sc.texture_indices));
    std::memcpy(sc.sampler_indices, kDefaults.samplers, sizeof(sc.sampler_indices));
  } else {
    for (u32 r = 0; r < 32; ++r) {
      sc.texture_indices[0][r] = kNullTextureSlot;
      sc.texture_indices[1][r] = kNullTexture3DSlot;
      sc.texture_indices[2][r] = kNullTextureCubeSlot;
      sc.sampler_indices[r] = SamplerWord(kLinearClampSamplerSlot);
    }
  }
  const d3d::Device &dev = device.dev();
  for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
    if (!(fetch_mask & (1u << r)))
      continue;
    TextureFetch fetch = DeviceTextureFetch(dev, r);
    // fnxf_xiii (2026-10-31): the filters the shaders' fetches of this constant give (fetch_filters.h). In the fetch's
    // words, so the memo below and the sampler's own (SamplerSlotLocked) key on them.
    if (filters && ((filters->slots >> r) & 1u)) {
      const u32 word3 = filters->Apply(r, fetch.dword[3]);
      if (word3 != fetch.dword[3]) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
          if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[filters] fetch constant {}: filters mag {} min {} mip {} aniso {} -> the shader's mag {} min {} "
                    "mip {} aniso {}", r, (fetch.dword[3] >> 19) & 3, (fetch.dword[3] >> 21) & 3,
                    (fetch.dword[3] >> 23) & 3, (fetch.dword[3] >> 25) & 7, (word3 >> 19) & 3, (word3 >> 21) & 3,
                    (word3 >> 23) & 3, (word3 >> 25) & 7);
        fetch.dword[3] = word3;
      }
    }
    if (g_mag_linear && ((fetch.dword[3] >> 19) & 3u) == 0u)  // mag filter point -> linear
      fetch.dword[3] = (fetch.dword[3] & ~(3u << 19)) | (1u << 19);
    const u32 texture_va = device.U32(d3d::kDeviceOffsetTextures + r * 4);
    const auto *header = texture_va ? GuestPtr<const d3d::BaseTexture>(texture_va) : nullptr;
    TextureSlotMemo &m =
        g_texture_slot_memo[((texture_va ^ (fetch.dword[0] * 0x85EBCA6Bu) ^ fetch.dword[1]) * 0x9E3779B1u) >> 23];
    const bool memo = fnxf_xiii::renut_gpu::CpuMemosOn() && header;
    if (all_memo && !memo)
      *all_memo = false;
    if (memo && m.frame == TextureFrameLocked() && m.epoch == g_texture_epoch && m.va == texture_va &&
        (!m.fx.depth_source || m.depth_epoch == g_depth_epoch) &&
        std::memcmp(m.device, fetch.dword, sizeof(m.device)) == 0 &&
        std::memcmp(m.header, &header->Format, sizeof(m.header)) == 0 &&
        !(m.fx.handover_surface && g_handover_targets && g_handover_targets->depth == m.fx.handover_surface)) {
      for (HostTexture *t : m.fx.transition)
        if (t)
          TransitionTextureLocked(t, plume::RenderTextureLayout::SHADER_READ);
      // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover): the target image it was sampled through.
      if (m.fx.handover_surface)
        TransitionSurfaceLocked(m.fx.handover_surface, plume::RenderTextureLayout::SHADER_READ);
      if (m.fx.handover_image)
        TransitionDepthImageLocked(*m.fx.handover_image, plume::RenderTextureLayout::SHADER_READ);
      if (m.fx.float_sampled)
        fnxf_xiii::renut_gpu::CountDepthFloatSampled();
      if (m.fx.words_viewed)
        fnxf_xiii::renut_gpu::CountDepthWords(true);
      sc.texture_indices[m.dim][r] = m.index;
      sc.sampler_indices[r] = m.sampler;
      if (slot_fx)
        slot_fx[r] = m.fx;
      continue;
    }
    BindEffects fx;
    const TextureBinding binding = BindTextureLocked(texture_va, fetch, &fx);
    if (slot_fx)
      slot_fx[r] = fx;
    const uint32_t dim = binding.dimension == TextureDimension::Tex3D  ? 1
                         : binding.dimension == TextureDimension::Cube ? 2
                                                                       : 0;
    // Stacked textures go through the 3D fetches, flagged (tfetchStacked in
    // shader_common.h), with the fetch constant's filter between layers and
    // its Z address mode.
    u32 index = binding.slot | (binding.scale.code & 7u) << kResolutionScaleShift;  // fnxf_xiii (2026-11-08): 3 bits
    if (binding.scale.drs)
      index |= std::min(binding.drs_code, kDrsCodeMax) << kDrsCodeShift;  // fnxf_xiii (2026-11-07, dynamic resolution)
    if (binding.stacked) {
      index |= kStackedTextureBit;
      if (fetch.VolumeMagFilter() != TextureFilter::Point ||
          fetch.VolumeMinFilter() != TextureFilter::Point) {
        index |= kStackedLayerLinearBit;
      }
      if (fetch.Address(2) == TextureAddress::Wrap)
        index |= kStackedLayerWrapBit;
    }
    sc.texture_indices[dim][r] = index;
    sc.sampler_indices[r] = SamplerWord(SamplerSlotLocked(fetch, binding.lod_scale));
    if (memo) {
      m.va = texture_va;
      std::memcpy(m.device, fetch.dword, sizeof(m.device));
      std::memcpy(m.header, &header->Format, sizeof(m.header));
      m.frame = TextureFrameLocked();
      m.epoch = g_texture_epoch;  // after the bind (its own bumps)
      m.depth_epoch = g_depth_epoch;
      m.dim = dim;
      m.index = index;
      m.sampler = sc.sampler_indices[r];
      m.fx = fx;
    }
  }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Geometry {
  bool indexed = false;
  u32 index_count = 0;
  plume::RenderIndexBufferView index_view;  // indices from first_vertex
  u32 vertex_count = 0;  // non-indexed host draw count
  u32 first_vertex = 0;  // first guest vertex converted
  u32 vertex_range = 0;  // vertices converted per stream
  // Tessellated draws: one instance per patch, the streams per instance.
  u32 instance_count = 1;
  bool per_instance = false;
};

// Reads 'count' guest indices into g_indices.
bool ReadIndices(const u8 *src, u32 count, bool is32, u32 endian) {
  if (!src || count > kMaxDrawIndices)
    return false;
  g_indices.resize(count);
  if (is32) {
    SwapEndian(endian, src, g_indices.data(), size_t(count) * 4);
  } else {
    const bool swap = endian == 1 || endian == 2;
    for (u32 i = 0; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, src + i * 2, 2);
      g_indices[i] = swap ? uint16_t((v >> 8) | (v << 8)) : v;
    }
  }
  return true;
}

// Rewrites g_indices for fans and quads as triangle lists.
void ExpandIndices(u32 primitive) {
  // fnxf_xiii (2026-10-19, render-thread proposal 20): a scratch vector kept between draws (swapped with g_indices, so
  // both keep their capacity), not a new one per fan or quad draw.
  static std::vector<u32> out;
  if (primitive == kTriangleFan) {
    const size_t n = g_indices.size() > 2 ? g_indices.size() - 2 : 0;
    out.resize(n * 3);
    for (size_t i = 2, o = 0; i < g_indices.size(); ++i, o += 3) {
      out[o] = g_indices[0];
      out[o + 1] = g_indices[i - 1];
      out[o + 2] = g_indices[i];
    }
    g_indices.swap(out);
  } else if (primitive == kQuadList) {
    const size_t n = g_indices.size() / 4;
    out.resize(n * 6);
    for (size_t i = 0, o = 0; i + 3 < g_indices.size(); i += 4, o += 6) {
      const u32 a = g_indices[i], b = g_indices[i + 1], c = g_indices[i + 2], d = g_indices[i + 3];
      out[o] = a;
      out[o + 1] = b;
      out[o + 2] = c;
      out[o + 3] = a;
      out[o + 4] = c;
      out[o + 5] = d;
    }
    g_indices.swap(out);
  }
}

// Guest index ranges kept across frames, rebased and in the host format, like
// the vertex streams (g_stream_cache): most draws read index buffers the game
// never rewrites. A range that hashes the same in kStableFlushes frames in a
// row gets a persistent copy, which draws use while the range still hashes the
// same, checked like the streams' (CacheCheckDue, off the render thread), and
// a range rewritten kMaxCacheDrops times is never cached again.
struct IndexKey {
  const u8 *src;
  u32 count, primitive, endian;
  bool is32;
  bool operator==(const IndexKey &) const = default;
};
struct IndexKeyHash {
  size_t operator()(const IndexKey &k) const {
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(k.src)) * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(k.count) << 16 | k.primitive << 3 | k.endian << 1 | u32(k.is32)) +
         0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};
struct CachedIndices {
  uint64_t hash = 0;
  u32 stable = 0;  // frames in a row the range hashed the same
  u32 drops = 0;   // copies dropped because the guest range changed
  bool dynamic = false;
  PoolSpan copy;
  u32 bytes = 0, index_count = 0, lo = 0, max_index = 0;
  bool wide = false;
  uint64_t last_used = 0;               // g_stream_cache_frame
  uint64_t checked_frame = UINT64_MAX;  // frame the range was last hashed or checked in
  uint64_t proven_flush = UINT64_MAX;   // fnxf_xiii: g_flush_serial the copy was last hashed equal in
  uint64_t exact_frame = UINT64_MAX;    // fnxf_xiii: g_stream_cache_frame of its last exact check (repeat count)
};
using IndexCache = std::unordered_map<IndexKey, CachedIndices, IndexKeyHash>;
IndexCache g_index_cache;
EntryMemo<IndexCache, IndexKeyHash, 11> g_index_memo;  // fnxf_xiii: 2,048 slots of a key each (proposal 9)

uint64_t HashIndices(const IndexKey &key) {
  return fnxf_xiii::renut_gpu::ContentHash64(key.src, size_t(key.count) * (key.is32 ? 4 : 2));  // fnxf_xiii
}

ContentVerifier<IndexKey, HashIndices> g_index_verifier;

void DropCachedIndices(CachedIndices &c) {
  if (!c.copy)
    return;
  g_stream_cache_bytes -= c.bytes;
  g_copy_pool.Free(c.copy);
  c.copy = {};
  c.bytes = 0;
}

// The draw's indices from the cache, if it has a copy of the guest range.
bool CachedIndexDraw(const IndexKey &key, const DrawCall &call, Geometry &g) {
  auto *entry = g_index_memo.Get(g_index_cache, key, false);
  if (!entry || !entry->second.copy)
    return false;
  CachedIndices &c = entry->second;
  c.last_used = g_stream_cache_frame;
  if (fnxf_xiii::renut_gpu::ExactStreamsOn()) {
    // fnxf_xiii (2026-10-17): fnxf_xiii_renut_exact_streams, as for streams. A changed range is dropped and the draw reads
    // it itself (TrackIndices hashes it again).
    if (c.proven_flush != g_flush_serial &&
        CardsProve(key.src, size_t(key.count) * (key.is32 ? 4 : 2), c.proven_flush, c.hash)) {
      c.proven_flush = g_flush_serial;  // fnxf_xiii: fnxf_xiii_renut_store_cards
    } else if (c.proven_flush != g_flush_serial) {
      const bool same = ExactHash(key.src, size_t(key.count) * (key.is32 ? 4 : 2)) == c.hash;
      g_card_diag[7] += same && c.proven_flush != UINT64_MAX && fnxf_xiii::renut_gpu::StoreCardsVerifyOn();
      fnxf_xiii::renut_gpu::CountExactCheck(uint64_t(key.count) * (key.is32 ? 4 : 2), !same,
                                        c.exact_frame == g_stream_cache_frame);
      c.exact_frame = g_stream_cache_frame;
      if (!same) {
        DropCachedIndices(c);
        c.stable = 0;
        c.checked_frame = UINT64_MAX;
        c.dynamic = ++c.drops >= kMaxCacheDrops;
        return false;
      }
      c.proven_flush = g_flush_serial;
    }
  } else if (CacheCheckDue(c.checked_frame)) {
    c.checked_frame = g_stream_cache_frame;
    g_index_verifier.Submit(key, c.hash);
  }
  g.first_vertex = u32(int64_t(call.base_vertex) + c.lo);
  g.vertex_range = c.max_index + 1;
  g.indexed = true;
  g.index_count = c.index_count;
  g.index_view = plume::RenderIndexBufferView(
      c.copy.Ref(), c.bytes,
      c.wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  return true;
}

// After a draw read the range into g_indices: count how long it has held, and
// give a range that has held long enough its persistent copy.
void TrackIndices(const IndexKey &key, u32 lo, u32 max_index) {
  CachedIndices &c = g_index_memo.Get(g_index_cache, key, true)->second;
  c.last_used = g_stream_cache_frame;
  if (c.dynamic || c.copy || c.checked_frame == g_stream_cache_frame)
    return;
  c.checked_frame = g_stream_cache_frame;
  const uint64_t hash = HashIndices(key);
  c.stable = hash == c.hash ? c.stable + 1 : 0;
  c.hash = hash;
  const bool wide = max_index > 0xFFFE;
  const u32 bytes = u32(g_indices.size()) * (wide ? 4 : 2);
  if (c.stable + 1 < kStableFlushes ||
      g_stream_cache_bytes + bytes > fnxf_xiii::renut_gpu::StreamCacheMaxBytes(kStreamCacheMaxBytes))  // fnxf_xiii
    return;
  const PoolSpan copy = g_copy_pool.Allocate(bytes);
  if (!copy)
    return;
  if (wide) {
    auto *out = reinterpret_cast<u32 *>(copy.cpu);
    for (size_t i = 0; i < g_indices.size(); ++i)
      out[i] = g_indices[i] - lo;
  } else {
    auto *out = reinterpret_cast<uint16_t *>(copy.cpu);
    for (size_t i = 0; i < g_indices.size(); ++i)
      out[i] = uint16_t(g_indices[i] - lo);
  }
  c.copy = copy;
  c.bytes = bytes;
  c.index_count = u32(g_indices.size());
  c.lo = lo;
  c.max_index = max_index;
  c.wide = wide;
  g_stream_cache_bytes += bytes;
}

bool PrepareGeometry(const GuestDevice &device, const DrawCall &call, Geometry &g) {
  const bool expand = call.primitive == kTriangleFan || call.primitive == kQuadList;
  static uint32_t rect_logged = 0;
  if (call.primitive == kRectangleList && !State().rectangle_list_gs && LogLimited(rect_logged, 1))
    RGPU_WARN("Rectangle lists are drawn as triangle lists");

  u32 lo = 0, max_index = 0;  // host indices are g_indices - lo, the largest max_index
  IndexKey index_key{};
  bool cacheable_indices = false;
  if (call.indexed) {
    const u8 *src = nullptr;
    bool is32 = false;
    u32 endian = 0;
    if (call.up_indices) {
      is32 = call.up_indices_32;
      endian = is32 ? 2 : 1;
      src = call.up_indices + size_t(call.start_index) * (is32 ? 4 : 2);
    } else {
      const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
      if (!ib)
        return false;
      const u32 common = ib->resource.Common;
      is32 = (common & d3d::kIndexBufferCommon32Bit) != 0;
      endian = d3d::IndexBufferEndian(common);
      const u8 *base = AddressPtr(ib->Address);
      if (!base)
        return false;
      src = base + size_t(call.start_index) * (is32 ? 4 : 2);
      // Draw*IndexedPrimitiveUP indices are the game's own memory, only valid
      // during the call; these are an index buffer's.
      index_key = IndexKey{src, call.count, call.primitive, endian, is32};
      cacheable_indices = call.count <= kMaxDrawIndices;
      if (cacheable_indices && rtw::Timed(rtw::kGeoLookup, [&] { return CachedIndexDraw(index_key, call, g); }))
        return true;
    }
    if (!rtw::Timed(rtw::kGeoRead, [&] { return ReadIndices(src, call.count, is32, endian); }))
      return false;
    if (expand)
      ExpandIndices(call.primitive);
    if (g_indices.empty())
      return false;
    lo = UINT32_MAX;
    u32 hi = 0;
    for (u32 index : g_indices) {
      lo = std::min(lo, index);
      hi = std::max(hi, index);
    }
    if (hi - lo >= kMaxDrawVertices)
      return false;
    g.first_vertex = u32(int64_t(call.base_vertex) + lo);
    g.vertex_range = hi - lo + 1;
    max_index = hi - lo;
  } else {
    if (!call.count || call.count > kMaxDrawVertices)
      return false;
    g.first_vertex = call.start_vertex;
    g.vertex_range = call.count;
    if (expand) {
      g_indices.resize(call.count);
      for (u32 i = 0; i < call.count; ++i)
        g_indices[i] = i;
      ExpandIndices(call.primitive);
      if (g_indices.empty())
        return false;
      max_index = call.count - 1;
    } else {
      g.vertex_count = call.count;
    }
  }

  if (call.indexed || expand) {
    // The host indices are rebased to the range rather than drawn with a base
    // vertex of -lo: D3D12's SV_VertexID leaves the base vertex out (Vulkan's
    // vertex index includes it), and vertex shaders that fetch by address
    // take the guest index from the vertex ID (renutVertexRegisters).
    // 16-bit when every index fits, like the guest's own format mostly is.
    const bool wide = max_index > 0xFFFE;
    const u32 bytes = u32(g_indices.size()) * (wide ? 4 : 2);
    UploadAllocation alloc = Video::UploadsLocked().Allocate(bytes, 16);
    if (!alloc)
      return false;
    if (wide) {
      auto *out = static_cast<u32 *>(alloc.cpu);
      for (size_t i = 0; i < g_indices.size(); ++i)
        out[i] = g_indices[i] - lo;
    } else {
      auto *out = static_cast<uint16_t *>(alloc.cpu);
      for (size_t i = 0; i < g_indices.size(); ++i)
        out[i] = uint16_t(g_indices[i] - lo);
    }
    g.indexed = true;
    g.index_count = u32(g_indices.size());
    g.index_view = plume::RenderIndexBufferView(
        alloc.ref, bytes, wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    if (cacheable_indices)
      rtw::Timed(rtw::kGeoTrack, [&] { TrackIndices(index_key, lo, max_index); });
  }
  return true;
}

// fnxf_xiii (fnxf_xiii_gpu_tess_grid, package 27): the index list of a tessellation grid, made once per kind and level: the
// triangles renutVertexRegisters' kinds 1 and 2 place, in their order, with their corners, each corner as its grid
// vertex's number (kinds 3 and 4 there). Render thread; the buffers live as long as the device.
struct TessellationGrid {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u32 index_count = 0;
  bool wide = false;
};
std::map<u32, TessellationGrid> g_tess_grids;  // key: triangles << 8 | level

const TessellationGrid *TessellationGridFor(bool triangles, u32 level) {
  TessellationGrid &grid = g_tess_grids[(triangles ? 1u : 0u) << 8 | level];
  if (grid.buffer)
    return &grid;
  std::vector<u32> indices;
  if (triangles) {
    auto number = [](u32 a, u32 b) { return a * (a + 1) / 2 + b; };
    for (u32 t = 0; t < level * level; ++t) {
      u32 row = u32(std::sqrt(double(t)));
      while (row * row > t)
        --row;
      while ((row + 1) * (row + 1) <= t)
        ++row;
      const u32 k = t - row * row, b = k / 2;
      if ((k & 1) == 0) {
        indices.insert(indices.end(), {number(row, b), number(row + 1, b), number(row + 1, b + 1)});
      } else {
        indices.insert(indices.end(), {number(row, b), number(row + 1, b + 1), number(row, b + 1)});
      }
    }
  } else {
    auto number = [level](u32 u, u32 v) { return v * (level + 1) + u; };
    for (u32 t = 0; t < 2 * level * level; ++t) {
      const u32 cell = t / 2, u = cell % level, v = cell / level;
      if ((t & 1) == 0)
        indices.insert(indices.end(), {number(u, v), number(u + 1, v), number(u + 1, v + 1)});
      else
        indices.insert(indices.end(), {number(u, v), number(u + 1, v + 1), number(u, v + 1)});
    }
  }
  const u32 points = triangles ? (level + 1) * (level + 2) / 2 : (level + 1) * (level + 1);
  grid.wide = points > 0xFFFF;
  const size_t bytes = indices.size() * (grid.wide ? 4 : 2);
  auto buffer = State().device->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(bytes, plume::RenderBufferFlag::INDEX));
  void *mapped = buffer ? buffer->map() : nullptr;
  if (!mapped)
    return nullptr;
  if (grid.wide) {
    std::memcpy(mapped, indices.data(), bytes);
  } else {
    auto *out = static_cast<uint16_t *>(mapped);
    for (size_t i = 0; i < indices.size(); ++i)
      out[i] = uint16_t(indices[i]);
  }
  buffer->unmap();
  buffer->setName(fmt::format("fnxf_xiii tessellation grid {} {}", triangles ? "triangles" : "quads", level));
  grid.buffer = std::move(buffer);
  grid.index_count = u32(indices.size());
  return &grid;
}

// Tessellated draws (DrawCall::tessellation): the patches become instances of a
// grid the vertex shader places (renutVertexRegisters in shader_common.h), at
// one level for the whole draw, the highest any patch edge asks for.
bool PrepareTessellation(const GuestDevice &device, const DrawCall &call, Geometry &g,
                         u32 tessellation[4]) {
  const bool triangles = call.primitive == kTrianglePatch;
  if (!triangles && call.primitive != kQuadPatch) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 1))
      RGPU_WARN("Tessellated primitive {} not drawn", call.primitive);
    return false;
  }
  // Segments per edge are the tessellation level plus one.
  const float min_level = std::max(device.F32(d3d::kDeviceOffsetMinTessellationLevel), 0.0f) + 1.0f;
  const float max_level =
      std::max(device.F32(d3d::kDeviceOffsetMaxTessellationLevel) + 1.0f, min_level);
  float level = max_level;
  // The count is the patch count. (N&B's water: each section's patch stream is
  // sized 36 bytes x the count it draws, and its factor pass exports one
  // patch's edge factors per invocation.)
  const u32 patches = call.count;
  if ((device.U32(d3d::kDeviceOffsetTessellationControl) & 3) == 2) {
    // Adaptive: the index buffer holds each patch's edge factors (floats), from
    // the start index on. When a memexport draw in flight writes them, these
    // are from a frame or two before (DrawNeedsExportFlush does not wait for
    // them).
    const u32 edges = triangles ? 3 : 4;
    // Without them, the maximum level.
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    const u8 *base = ib ? AddressPtr(ib->Address) : nullptr;
    if (base && ReadIndices(base + size_t(call.start_index) * 4, patches * edges, true,
                            d3d::IndexBufferEndian(ib->resource.Common))) {
      float highest = 0.0f;
      for (u32 bits : g_indices) {
        float factor;
        std::memcpy(&factor, &bits, 4);
        if (factor > highest && factor < 1024.0f)
          highest = factor;
      }
      level = std::clamp(highest + 1.0f, min_level, max_level);
    }
  }
  if (!patches || patches > kMaxDrawVertices)
    return false;
  constexpr u32 kMaxTessellatedVertices = 1u << 22;
  u32 segments = std::clamp(u32(std::ceil(level)), 1u, 64u);
  const u32 per_segment = triangles ? 3 : 6;  // vertices per segment^2
  while (segments > 1 &&
         uint64_t(patches) * segments * segments * per_segment > kMaxTessellatedVertices)
    --segments;

  g.vertex_count = segments * segments * per_segment;
  g.instance_count = patches;
  g.per_instance = true;
  g.first_vertex = u32(call.base_vertex);
  g.vertex_range = patches;
  tessellation[0] = triangles ? 1 : 2;
  tessellation[1] = segments;
  tessellation[2] = u32(call.base_vertex);
  // fnxf_xiii (fnxf_xiii_gpu_tess_grid): the same triangles, indexed by grid vertex.
  if (fnxf_xiii::renut_gpu::TessGridOn()) {
    if (const TessellationGrid *grid = TessellationGridFor(triangles, segments)) {
      g.indexed = true;
      g.index_count = grid->index_count;
      g.index_view = plume::RenderIndexBufferView(plume::RenderBufferReference(grid->buffer.get()),
                                                  grid->index_count * (grid->wide ? 4 : 2),
                                                  grid->wide ? plume::RenderFormat::R32_UINT
                                                             : plume::RenderFormat::R16_UINT);
      tessellation[0] = triangles ? 3 : 4;
      fnxf_xiii::renut_gpu::CountTessGrid(uint64_t(patches) * g.vertex_count,
                                      uint64_t(patches) * (triangles ? (segments + 1) * (segments + 2) / 2
                                                                     : (segments + 1) * (segments + 1)));
    }
  }
  return true;
}

struct StreamSource {
  const u8 *data = nullptr;  // guest vertex 0 of the stream
  u32 stride = 0;
  u32 endian = 0;
  u32 vertex_count = 0;      // vertices available from data
  u32 cpu_address = 0;       // guest VA of data, when read through the vertex buffer
};

// 'shader_strides': the strides in the vertex shader's microcode when the draw
// fetches as the shader was bound (or baked), with no declaration set on the
// device. The GPU uses those, whatever SetStreamSource was given; D3D patches
// the stream strides in only when it binds the shader to a device
// declaration.
StreamSource GetStreamSource(const GuestDevice &device, const DrawCall &call,
                             const ShaderRecord &vs, const u32 *shader_strides, u32 stream) {
  StreamSource src;
  if (stream == 0 && call.up_vertices) {
    src.data = call.up_vertices;
    src.stride = call.up_stride;
    src.endian = kUpEndian;
    src.vertex_count = call.up_vertex_count;
    return src;
  }
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  const u32 w0 = device.VertexFetchWord(fetch_index, 0);
  const u32 w1 = device.VertexFetchWord(fetch_index, 1);
  if (shader_strides)
    src.stride = shader_strides[stream];
  if (!src.stride)
    src.stride = u32(device.U8(d3d::kDeviceOffsetStreamStrides + stream)) * 4;
  if (!src.stride)
    src.stride = vs.bound_strides[stream];  // SetStreamSource(..., 0): the Bind stride
  if (!src.stride)
    src.stride = vs.fetch_strides[stream];  // or the one the shader's vfetch encodes
  src.endian = w1 & 3;
  const u32 size = w1 & 0x3FFFFFCu;
  const u32 gpu_address = w0 & ~3u;
  src.data = gpu_address ? GuestPhysicalPtr<const u8>(gpu_address) : nullptr;
  // Read through the vertex buffer's CPU address, where the game wrote it,
  // when the fetch constant is the one SetStreamSource made from that buffer
  // (its GPU address plus the stream offset).
  if (const auto *vb = GuestPtr<const d3d::VertexBuffer>(
          device.U32(d3d::kDeviceOffsetStreamSources + stream * 4))) {
    const u32 cpu_base = u32(vb->Format.dword[0]) & ~3u;
    const u32 gpu_base = d3d::CpuToGpuAddress(cpu_base);
    if (cpu_base >= 0x20000000u && gpu_address >= gpu_base &&
        gpu_address - gpu_base < (u32(vb->Format.dword[1]) & 0x3FFFFFCu)) {
      src.cpu_address = cpu_base + (gpu_address - gpu_base);
      src.data = GuestPtr<const u8>(src.cpu_address);
    }
  }
  src.vertex_count = src.stride ? size / src.stride : (size ? 1 : 0);
  return src;
}

// Host vertices of the draw's streams built on the GPU from export windows
// (ConvertExportedStreamsLocked), from its first vertex; 'bytes' 0 for the
// others.
struct GpuStreams {
  plume::RenderBufferReference ref[kMaxVertexStreams];
  u32 bytes[kMaxVertexStreams] = {};
};

// fnxf_xiii (2026-10-18, the deko3d water): fnxf_xiii_renut_diag_stream_dump_vs names a vertex shader (hex hash); its draws'
// streams are logged (first 16): the guest source, the first two vertices as the guest has them and as converted for
// the host, and whether a cached copy is bound.
bool StreamDumpLog(const ShaderRecord &vs, u32 stream, const StreamSource &src, const HostVertexStream &hs,
                   const u8 *guest, u32 first, u32 count, const DrawCall &call) {
  static const uint64_t want = [] {
    const std::string v = REXCVAR_GET(fnxf_xiii_renut_diag_stream_dump_vs);
    return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
  }();
  if (!want || vs.hash != want)
    return false;
  static uint32_t seen = 0;
  if (seen++ >= 16 && seen % 64 != 0)  // the first 16, then every 64th
    return false;
  std::string guest_words, host_words;
  if (guest && count) {
    const u32 n = std::min<u32>(src.stride / 4, 16);
    for (u32 v = 0; v < std::min<u32>(count, 2); ++v) {
      for (u32 i = 0; i < n; ++i) {
        u32 w;
        std::memcpy(&w, guest + size_t(v) * src.stride + i * 4, 4);
        guest_words += fmt::format("{:08X} ", w);
      }
      guest_words += "| ";
    }
    std::vector<u8> host(size_t(hs.host_stride) * 2 + 16);
    ConvertVertices(hs, guest, src.stride, std::min<u32>(count, 2), src.endian, host.data());
    for (u32 i = 0; i < std::min<u32>(hs.host_stride * 2 / 4, 32); ++i) {
      u32 w;
      std::memcpy(&w, host.data() + i * 4, 4);
      host_words += fmt::format("{:08X} ", w);
    }
  }
  RGPU_INFO("[streamdump] vs {:016X} stream {} cpu {:08X} stride {} host stride {} endian {} first {} count {} prim {} "
            "draw count {}: guest {}host {}",
            vs.hash, stream, src.cpu_address, src.stride, hs.host_stride, src.endian, first, count, call.primitive,
            call.count, guest_words, host_words);
  return true;
}

// fnxf_xiii (2026-10-20, fnxf_xiii_deko3d_skin_once): the fur's vertex shaders (tools/skinonce_shaders.txt), and what the
// last BindStreams bound: FNV-1a of each stream's GPU address, size, stride and slot (FurSkinLocked's key: a draw that
// binds the same vertex data has the same hash within a frame, the upload ring never handing out a range twice).
// (tools/skinonce_shaders.txt, sorted: the pass gives these constant 913; each checked by tools/difftest/
// difftest_vs_skin.py.)
constexpr uint64_t kSkinOnceVs[] = {
    // the fur (package 36g). 2026-10-20: the shadow maps' skinned vertex shaders were tried and taken out (emulator run
    // 109: one draw in five read the rows the others wrote; docs/deko3d-renderer.md section 22).
    0x12399EE21A0440AEull, 0x146DA1F10B7D746Eull, 0x7F73E48B8EA43715ull, 0xB814D7237E615990ull,
};
bool IsFurVs(uint64_t hash) {
  return hash == 0x12399EE21A0440AEull || hash == 0x146DA1F10B7D746Eull || hash == 0x7F73E48B8EA43715ull ||
         hash == 0xB814D7237E615990ull;
}
bool IsSkinOnceVs(uint64_t hash) {
  static_assert(std::is_sorted(std::begin(kSkinOnceVs), std::end(kSkinOnceVs)));
  return std::binary_search(std::begin(kSkinOnceVs), std::end(kSkinOnceVs), hash);
}
uint64_t g_bound_streams_hash = 0;
// The last UploadVertexFetchData's copies (guest source, size, endian a fetch constant), for FurSkinLocked.
struct FetchRecordCopy {
  u32 index = 0;
  const u8 *src = nullptr;
  u32 size = 0;
  u32 endian = 0;
  bool operator==(const FetchRecordCopy &o) const {
    return index == o.index && src == o.src && size == o.size && endian == o.endian;
  }
};
struct FetchRecord {
  u32 count = 0;
  bool unusable = false;  // a copy from a memexport window, or DrawUP vertices
  FetchRecordCopy copies[32];
};
FetchRecord g_fetch_record;

inline uint64_t HashMix(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i, v >>= 8)
    h = (h ^ (v & 0xFF)) * 1099511628211ull;
  return h;
}

void BindHashed(plume::RenderCommandList *cmd, const plume::RenderVertexBufferView &view,
                const plume::RenderInputSlot &slot) {
  const uint64_t address = view.buffer.ref ? view.buffer.ref->getDeviceAddress() + view.buffer.offset : 0;
  g_bound_streams_hash = HashMix(HashMix(HashMix(g_bound_streams_hash, address), uint64_t(view.size) << 32 | slot.stride),
                                 uint64_t(slot.index) << 8 | uint64_t(slot.classification));
  SetVertexBuffer(cmd, view, slot);
}

bool BindStreams(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                 const u32 *shader_strides, const HostVertexLayout &layout, uint64_t layout_hash,
                 const Geometry &g, const GpuStreams &gpu_streams) {
  plume::RenderCommandList *cmd = State().command_list;
  g_bound_streams_hash = 1469598103934665603ull;
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_stream_mask): the used streams by their mask, in the same order (16 checks a draw
  // before, 1.3 % of the render thread in the second console run).
  static const bool stream_mask = REXCVAR_GET(fnxf_xiii_renut_stream_mask);
  for (u32 left = stream_mask ? layout.used_mask : (1u << kMaxVertexStreams) - 1u; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    const HostVertexStream &hs = layout.streams[stream];
    if (!hs.used)
      continue;
    if (gpu_streams.bytes[stream]) {
      {
        static const uint64_t want = [] {
          const std::string v = REXCVAR_GET(fnxf_xiii_renut_diag_stream_dump_vs);
          return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
        }();
        static uint32_t seen = 0;
        if (want && vs.hash == want && (seen++ < 16 || seen % 64 == 0))
          RGPU_INFO("[streamdump] vs {:016X} stream {}: the GPU-converted exported stream ({} bytes)", vs.hash, stream,
                    gpu_streams.bytes[stream]);
      }
      const plume::RenderVertexBufferView view(gpu_streams.ref[stream], gpu_streams.bytes[stream]);
      const plume::RenderInputSlot slot(
          stream, hs.host_stride,
          g.per_instance ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                         : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
      BindHashed(cmd, view, slot);
      continue;
    }
    const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
    // A zero stride repeats one vertex for every index.
    const bool constant = src.stride == 0;
    const u32 first = constant ? 0 : g.first_vertex;
    u32 count = constant ? 1 : g.vertex_range;
    if (first < src.vertex_count)
      count = std::min(count, src.vertex_count - first);
    else
      count = src.data ? 0 : count;
    const u32 bytes = std::max<u32>(count, 1) * hs.host_stride;
    const bool fills = src.data && count;
    const u8 *guest = fills ? src.data + size_t(first) * src.stride : nullptr;
    const bool queued = fills && !(stream == 0 && call.up_vertices);
    const StreamKey key{guest, layout_hash, stream, src.stride, count, src.endian};
    // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_lean_diagnostics): the dump's check once, not a call per stream.
    static const bool stream_dump = !REXCVAR_GET(fnxf_xiii_renut_lean_diagnostics) ||
                                    !REXCVAR_GET(fnxf_xiii_renut_diag_stream_dump_vs).empty();
    const bool dumped = stream_dump && StreamDumpLog(vs, stream, src, hs, guest, first, count, call);
    CachedStream *cached = nullptr;
    if (queued && src.stride) {
      cached = &g_stream_memo.Get(g_stream_cache, key, true)->second;
      cached->last_used = g_stream_cache_frame;
      if (cached->copy && fnxf_xiii::renut_gpu::ExactStreamsOn()) {
        // fnxf_xiii (2026-10-17): fnxf_xiii_renut_exact_streams. The copy's guest bytes hashed whole on its first use
        // after each kick; changed, it is dropped and the stream converted below, as an uncached one.
        if (cached->proven_flush != g_flush_serial &&
            CardsProve(key.guest, size_t(key.count) * key.stride, cached->proven_flush, cached->hash)) {
          cached->proven_flush = g_flush_serial;  // fnxf_xiii: fnxf_xiii_renut_store_cards
        } else if (cached->proven_flush != g_flush_serial) {
          const bool same = ExactHash(key.guest, size_t(key.count) * key.stride) == cached->hash;
          g_card_diag[7] += same && cached->proven_flush != UINT64_MAX && fnxf_xiii::renut_gpu::StoreCardsVerifyOn();
          fnxf_xiii::renut_gpu::CountExactCheck(uint64_t(key.count) * key.stride, !same,
                                            cached->exact_frame == g_stream_cache_frame);
          cached->exact_frame = g_stream_cache_frame;
          if (same) {
            cached->proven_flush = g_flush_serial;
          } else {
            DropCachedCopy(*cached);
            cached->stable = 0;
            cached->checked_frame = UINT64_MAX;
            cached->dynamic = ++cached->drops >= kMaxCacheDrops;
          }
        }
      } else if (cached->copy && CacheCheckDue(cached->checked_frame)) {
        cached->checked_frame = g_stream_cache_frame;
        g_stream_verifier.Submit(key, cached->hash);
      }
      if (cached->copy) {
        if (dumped)
          RGPU_INFO("[streamdump] stream {}: bound the cached copy ({} bytes)", stream, cached->bytes);
        const plume::RenderVertexBufferView view(cached->copy.Ref(), cached->bytes);
        const plume::RenderInputSlot slot(
            stream, hs.host_stride,
            g.per_instance ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                           : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
        BindHashed(cmd, view, slot);
        continue;
      }
    }
    const UploadAllocation *queued_alloc = queued ? FindQueuedStream(key) : nullptr;
    const bool reused = queued_alloc != nullptr;
    UploadAllocation alloc = reused ? *queued_alloc : Video::UploadsLocked().Allocate(bytes, 16);
    if (!alloc)
      return false;
    // Conversion writes every vertex; only an unfilled buffer needs zeros.
    if (!fills)
      std::memset(alloc.cpu, 0, bytes);
    if (fills && !reused) {
      if (call.up_vertices && stream == 0) {
        // A copy taken at the call (queued.cpp).
        ConvertVertices(hs, guest, src.stride, count, src.endian, static_cast<u8 *>(alloc.cpu));
      } else {
        PendingStreamFill fill;
        fill.stream = hs;
        fill.guest = guest;
        fill.stride = src.stride;
        fill.count = count;
        fill.endian = src.endian;
        fill.host = static_cast<u8 *>(alloc.cpu);
        fill.key = key;
        fill.cacheable = cached && !cached->dynamic;
        g_stream_fills.push_back(std::move(fill));
        AddQueuedStream(key, alloc);
      }
    }
    const plume::RenderVertexBufferView view(alloc.ref, bytes);
    const plume::RenderInputSlot slot(stream, constant ? 0 : hs.host_stride,
                                      g.per_instance
                                          ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                          : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    BindHashed(cmd, view, slot);
  }

  if (!g_bound.zero_stream) {
    g_bound.zero_stream = Video::UploadsLocked().Allocate(kZeroStreamSize, 16);
    if (!g_bound.zero_stream)
      return false;
    std::memset(g_bound.zero_stream.cpu, 0, kZeroStreamSize);
  }
  const plume::RenderVertexBufferView zero_view(g_bound.zero_stream.ref, kZeroStreamSize);
  const plume::RenderInputSlot zero_slot(kZeroStreamSlot, 0);
  SetVertexBuffer(cmd, zero_view, zero_slot);
  return true;
}

// The vertex inputs the shader reads from its declaration by an index other
// than the vertex index (vfetchBound): each described from the declaration's
// element for it. Returns the fetch constants (bit i = index 64 + i) whose
// buffers those reads need copied.
u32 DescribeBoundFetches(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                         const VertexDeclarationRecord &declaration, const u32 *shader_strides,
                         SharedConstants &sc) {
  if (!vs.entry)
    return 0;
  const ShaderVertexInput *inputs = g_shaderVertexInputs + vs.entry->vertexInputOffset;
  const u32 count = std::min<u32>(vs.entry->vertexInputCount, kMaxBoundFetches);
  // fnxf_xiii (2026-10-19, render-thread proposal 21): whether the shader has any input fetched by another index, once
  // per shader entry (static): most have none, and their draws skip the scan of cold input records.
  struct BoundSummary {
    const ShaderCacheEntry *entry = nullptr;
    bool any = false;
  };
  static BoundSummary summaries[256];
  const uintptr_t a = reinterpret_cast<uintptr_t>(vs.entry);
  BoundSummary &summary = summaries[((a >> 3) ^ (a >> 11)) & 255];
  if (summary.entry != vs.entry) {
    summary.any = false;
    for (u32 i = 0; i < count; ++i)
      summary.any |= (inputs[i].flags & kVertexInputBoundFetch) != 0;
    summary.entry = vs.entry;
  }
  if (!summary.any)
    return 0;
  u32 mask = 0;
  for (u32 i = 0; i < count; ++i) {
    if (!(inputs[i].flags & kVertexInputBoundFetch))
      continue;
    for (const GuestVertexElement &e : declaration.elements) {
      if (e.Usage != inputs[i].usage || e.UsageIndex != inputs[i].usageIndex)
        continue;
      const u32 stream = e.Stream;
      if (stream >= kMaxVertexStreams)
        break;
      const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
      const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
      sc.bound_fetch[i][0] = fetch_index;
      sc.bound_fetch[i][1] = u32(e.Offset) / 4;
      sc.bound_fetch[i][2] = src.stride / 4;
      sc.bound_fetch[i][3] = BoundDeclType(u32(e.Type)) & 0x3FFFFFu;
      mask |= 1u << (fetch_index - 64);
      break;
    }
  }
  return mask;
}

bool FetchFromExportWindow(const GuestDevice &device, const DrawCall &call, u32 fetch_index,
                           u32 &slot, uint64_t &offset);
bool EnsureBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t size,
                  const plume::RenderBufferDesc &desc_template, const char *name);
bool RefillBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t &used, uint64_t need,
                  const plume::RenderBufferDesc &desc_template, const char *name);

// Copies the buffers of the fetch constants the vertex shader reads by address
// into one allocation and fills their SharedConstants slots. A buffer whose
// newest data is in a pending export's window comes from there instead: then
// the whole fetch data is put together on the GPU, in the slot's
// gpu_fetch_data, and the returned allocation's ref and gpu_address point
// there. Records copies: before the framebuffer is bound.
// fnxf_xiii (2026-10-18, the deko3d water): set by DrawStateLocked for draws of fnxf_xiii_renut_diag_stream_dump_vs's vertex
// shader; UploadVertexFetchData then logs its fetch copies.
bool g_dump_fetch = false;

UploadAllocation UploadVertexFetchData(const GuestDevice &device, const DrawCall &call, u32 mask,
                                       SharedConstants &sc) {
  struct Copy {
    u32 index;
    const u8 *src;
    u32 size;
    u32 endian;
    bool exported;        // from the export window below
    u32 export_slot;
    uint64_t export_offset;
  };
  Copy copies[32];
  u32 copy_count = 0;
  u32 total = 16;
  bool any_exported = false;
  const u32 up_fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(0);
  for (u32 bit = 0; bit < 32; ++bit) {
    if (!(mask & (1u << bit)))
      continue;
    const u32 index = 64 + bit;
    Copy c{index, nullptr, 0, 0, false, 0, 0};
    if (index == up_fetch_index && call.up_vertices) {
      c.src = call.up_vertices;
      c.size = call.up_vertex_count * call.up_stride;
      c.endian = kUpEndian;
    } else {
      const u32 w0 = device.VertexFetchWord(index, 0);
      const u32 w1 = device.VertexFetchWord(index, 1);
      c.src = (w0 & ~3u) ? GuestPhysicalPtr<const u8>(w0 & ~3u) : nullptr;
      c.size = w1 & 0x3FFFFFCu;
      c.endian = w1 & 3;
    }
    if (!c.src)
      c.size = 0;
    c.size = std::min(c.size, kMaxFetchCopyBytes);
    if (c.size && FetchFromExportWindow(device, call, index, c.export_slot, c.export_offset))
      any_exported = c.exported = true;
    copies[copy_count++] = c;
    total += (c.size + 15u) & ~15u;
  }

  // fnxf_xiii (2026-10-20, fnxf_xiii_deko3d_skin_once): the draw's fetch copies, for FurSkinLocked's key (the bone data
  // by its guest bytes: each kick copies it again, to another offset).
  g_fetch_record.count = copy_count;
  g_fetch_record.unusable = any_exported || call.up_vertices;
  for (u32 i = 0; i < copy_count; ++i)
    g_fetch_record.copies[i] = {copies[i].index, copies[i].src, copies[i].size, copies[i].endian};

  // Without exports the slots count from the start of the upload chunk, so a
  // buffer an earlier draw has queued a copy of in the same chunk is read
  // there (FetchCopy) and not copied again.
  const bool shared = !any_exported;
  // fnxf_xiii (2026-10-19, render-thread proposal 10): room only for the copies to make: those already queued in the
  // chunk the allocation goes to are read there (before, their room was taken anyway, and cleaned at the submit). If
  // the allocation lands in another chunk, none is (that chunk has none of them): then room for all.
  bool reused[32] = {};
  u32 needed = total;
  const plume::RenderBuffer *chunk = shared ? Video::UploadsLocked().CurrentBuffer() : nullptr;
  if (shared && chunk) {
    needed = 16;
    for (u32 i = 0; i < copy_count; ++i) {
      const Copy &c = copies[i];
      if (c.size && !c.exported && !(c.index == up_fetch_index && call.up_vertices)) {
        const FetchCopy &known = FetchCopyFor(c.src, c.size);
        reused[i] = known.generation == g_fetch_copy_generation && known.src == c.src && known.size == c.size &&
                    known.buffer == chunk;
      }
      if (!reused[i])
        needed += (c.size + 15u) & ~15u;
    }
  }
  UploadAllocation alloc = Video::UploadsLocked().Allocate(needed, 256);
  if (alloc && shared && chunk && alloc.ref.ref != chunk) {
    std::fill(std::begin(reused), std::end(reused), false);
    if (needed < total)
      alloc = Video::UploadsLocked().Allocate(total, 256);
  }
  if (!alloc)
    return alloc;
  const uint64_t chunk_offset = shared ? alloc.ref.offset : 0;
  u32 offset = 0;
  for (u32 i = 0; i < copy_count; ++i) {
    const Copy &c = copies[i];
    u8 *dst = static_cast<u8 *>(alloc.cpu) + offset;
    u32 at = u32(chunk_offset) + offset;
    if (reused[i]) {
      // Queued in this chunk by an earlier draw: read there, no room here.
      sc.vertex_fetch_slots[c.index] = FetchCopyFor(c.src, c.size).offset | c.endian;
      continue;
    }
    if (c.size && !c.exported) {
      if (c.index == up_fetch_index && call.up_vertices) {
        std::memcpy(dst, c.src, c.size);
      } else if (shared) {
        FetchCopy &known = FetchCopyFor(c.src, c.size);
        if (known.generation == g_fetch_copy_generation && known.src == c.src &&
            known.size == c.size && known.buffer == alloc.ref.ref) {
          at = known.offset;
        } else {
          g_copy_fills.push_back({c.src, dst, c.size});
          known = {c.src, c.size, g_fetch_copy_generation, alloc.ref.ref, at};
        }
      } else {
        g_copy_fills.push_back({c.src, dst, c.size});
      }
    }
    sc.vertex_fetch_slots[c.index] = at | c.endian;
    if (g_dump_fetch) {
      static uint32_t logged = 0;
      if (logged++ < 48) {
        u32 w[4] = {};
        if (c.src && c.size >= 16)
          std::memcpy(w, c.src, 16);
        RGPU_INFO("[fetchdump] fetch {} src {} size {} endian {} exported {} (slot {} offset {}) shared {} at {} chunk "
                  "offset {} guest {:08X} {:08X} {:08X} {:08X}",
                  c.index, fmt::ptr(c.src), c.size, c.endian, c.exported, c.export_slot, c.export_offset, shared, at,
                  chunk_offset, w[0], w[1], w[2], w[3]);
      }
    }
    offset += (c.size + 15u) & ~15u;
  }
  if (shared) {
    alloc.ref.offset = 0;
    alloc.gpu_address = alloc.gpu_address ? alloc.gpu_address - chunk_offset : 0;
    sc.vertex_fetch_data = alloc.gpu_address;
    // fnxf_xiii: the whole chunk from its start (the base), all of it allocated memory: the same for every draw of the
    // chunk, so draws keep the same SharedConstants (fnxf_xiii_renut_shared_dedupe). 0 for a draw with no copies (its
    // shaders fetch nothing from memory).
    sc.vertex_fetch_bytes = copy_count ? u32(std::min<uint64_t>(alloc.chunk_size, 0xFFFFFFF0ull)) : 0u;
    return alloc;
  }
  sc.vertex_fetch_data = alloc.gpu_address;
  sc.vertex_fetch_bytes = copy_count ? total : 0u;  // fnxf_xiii: the region below, from its start

  // The guest buffers' copies from the upload, the exported ones from their
  // windows (raw bytes, as guest memory would hold them), each into its own
  // part of the region.
  auto &s = State();
  uint64_t &used = s.gpu_fetch_data_used[s.frame];
  // The draws before still read the old buffer, retired with the slot. fnxf_xiii (2026-10-27): RefillBuffer.
  if (!RefillBuffer(s.gpu_fetch_data[s.frame], s.gpu_fetch_data_capacity[s.frame], used, total,
                    plume::RenderBufferDesc::DefaultBuffer(0, AddressableBufferFlags()), "renut gpu fetch data")) {
    return UploadAllocation{};
  }
  plume::RenderBuffer *dest = s.gpu_fetch_data[s.frame].get();
  const uint64_t region = used;
  used = (used + total + 255u) & ~uint64_t(255);
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier to_copy[kFramesInFlight + 1];
  u32 barrier_count = 0;
  to_copy[barrier_count++] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE);
  // fnxf_xiii (2026-10-30, ZeroCopyOn): the one buffer of guest memory. No L2 invalidation here: what is read in place
  // is a kernel's output (the GPU's own writes) or memory the game wrote before the window's prepare, whose fresh
  // prepare dropped the L2 (PrepareZeroCopyExportLocked; v2 dropped it once a draw: a Full barrier each, two gpfifo
  // entries, see plume_deko3d.h beforeBufferCopy).
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (plume::RenderBuffer *window = ExportBuffer(s, slot)) {
      to_copy[barrier_count++] = plume::RenderBufferBarrier(window, plume::RenderBufferAccess::READ);
      if (ZeroCopyOn())
        break;  // every slot's windows are in it
    }
  }
  cmd->barriers(plume::RenderBarrierStage::COPY, to_copy, barrier_count);
  offset = 0;
  for (u32 i = 0; i < copy_count; ++i) {
    const Copy &c = copies[i];
    if (c.size) {
      const plume::RenderBufferReference to(dest, region + offset);
      if (c.exported) {
        cmd->copyBufferRegion(to, plume::RenderBufferReference(ExportBuffer(s, c.export_slot), c.export_offset),
                              c.size);
      } else {
        cmd->copyBufferRegion(
            to, plume::RenderBufferReference(alloc.ref.ref, alloc.ref.offset + offset), c.size);
      }
    }
    offset += (c.size + 15u) & ~15u;
  }
  // Read by the vertex shader (as a root SRV on D3D12).
  to_copy[0] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ);
  for (u32 i = 1; i < barrier_count; ++i)
    to_copy[i].accessBits = plume::RenderBufferAccess::WRITE;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, to_copy, 1);
  if (barrier_count > 1)
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_copy + 1, barrier_count - 1);
  g_bound.framebuffer = nullptr;  // the copies end any render pass
  alloc.ref = plume::RenderBufferReference(dest, region);
  alloc.gpu_address = UsesVulkanBindings() ? dest->getDeviceAddress() + region : 0;  // fnxf_xiii: and deko3d
  sc.vertex_fetch_data = alloc.gpu_address;
  return alloc;
}

// ---------------------------------------------------------------------------

// fnxf_xiii (fnxf_xiii_gpu_bool_spec): the bools a shader's SPIR-V specialises, read once.
const u32 *BoolSpecMask(const ShaderRecord *r) {
  if (!r->bool_spec_read) {
    r->bool_spec_read = true;
    if (r->entry)
      ShaderBoolSpecMask(*r->entry, r->bool_spec_mask);
  }
  return r->bool_spec_mask;
}

PipelineKey MakeKey(const GuestDevice &device, const ShaderRecord *vs, const ShaderRecord *ps,
                    const VertexDeclarationRecord *declaration, const Targets &t, u32 primitive,
                    bool per_instance, const u32 *booleans, u32 alpha_test_func) {
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  // fnxf_xiii (fnxf_xiii_gpu_bool_spec): the draw's values of the bools its shaders specialise (the same words the shaders
  // read at run time, SharedConstants.booleans).
  if (fnxf_xiii::renut_gpu::BoolSpecOn()) {
    const u32 *vm = BoolSpecMask(vs);
    const u32 *pm = ps ? BoolSpecMask(ps) : nullptr;
    bool any = false;
    for (u32 w = 0; w < 8; ++w) {
      key.bool_mask[w] = vm[w] | (pm ? pm[w] : 0u);
      key.bool_values[w] = booleans[w] & key.bool_mask[w];
      any |= key.bool_mask[w] != 0;
    }
    if (any)
      fnxf_xiii::renut_gpu::CountBoolSpec(fnxf_xiii::renut_gpu::BoolSpecEvent::kDraw);
  }
  const d3d::Device &dev = device.dev();
  key.vertex_shader = vs->entry;
  key.pixel_shader = ps ? ps->entry : nullptr;
  key.layout_hash = declaration->layout_hash;
  key.topology = Topology(primitive);
  key.color_count = t.color_count;
  for (uint32_t i = 0; i < t.color_count; ++i)
    key.color_formats[i] = t.colors[i]->format;
  key.depth_format = t.depth ? t.depth->format : plume::RenderFormat::UNKNOWN;
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  key.samples = uint32_t(first->samples);
  const d3d::ControlPacket &cp = dev.m_ControlPacket;
  key.blend_control[0] = cp.BlendControl0;
  key.blend_control[1] = cp.BlendControl1;
  key.blend_control[2] = cp.BlendControl2;
  key.blend_control[3] = cp.BlendControl3;
  key.color_mask = u32(dev.m_ValuesPacket.ColorMask) & 0xFFFF;
  key.depth_control = t.depth ? u32(cp.DepthControl) : 0;
  key.stencil_ref_mask = t.depth ? u32(dev.m_ValuesPacket.StencilRefMask) & 0xFFFFFF : 0;
  key.stencil_ref_mask_bf = t.depth ? u32(dev.m_ValuesPacket.StencilRefMaskBF) & 0xFFFFFF : 0;
  key.mode_control = u32(cp.ModeControl) & 0x7;
  key.alpha_to_coverage = (u32(cp.ColorControl) >> 4) & 1;
  // fnxf_xiii: PA_SC_AA_MASK, 4 bits per pixel of a 2x2 quad (sample i of pixel p is bit 4p + i): a sample is written
  // when any pixel's bit for it is set (the D3D masks repeat it for the four).
  if (key.samples > 1) {
    const u32 aa = device.U32(0x2A00) & 0xFFFF;
    // fnxf_xiii (2026-11-06, MSAA 4x/8x): the mask is the guest's samples; a host with more (fnxf_xiii_msaa_samples) gets
    // guest sample i's bit on every host sample j with j % guest == i.
    const u32 guest = std::min<u32>(GuestSampleCount(first->info_word), key.samples);
    const u32 guest_all = guest > 1 ? (1u << guest) - 1 : 1u;
    const u32 guest_mask = (aa | aa >> 4 | aa >> 8 | aa >> 12) & guest_all;
    const u32 all = (1u << key.samples) - 1;
    u32 mask = 0;
    for (u32 j = 0; j < key.samples; ++j)
      if (guest_mask & (1u << (j % std::max<u32>(guest, 1u))))
        mask |= 1u << j;
    key.sample_mask = mask == all ? 0 : (mask ? mask : 0x80000000u);  // none: a sample bit past the count
  }
  if (primitive == kRectangleList) {
    if (State().rectangle_list_gs)
      key.rectangle_list = 1;
    // Rectangles are not culled. N&B's D3D clears through rectangles
    // (ClearRects_D3D, 0x822043D8) whose corners run clockwise on screen, under
    // whatever cull mode the game left set: with back faces culled and
    // counterclockwise front faces (after hitting some enemies) the clear of
    // the post-process surface was culled, so its resolve took the previous
    // frame's final image back into the bloom, which grew to white.
    key.mode_control &= ~3u;
  }
  // fnxf_xiii (2026-10-27): for a draw against the stand-in target too. It was left out there for a while (the game's
  // copy kernel, a point list against it, since made a buffer copy: CopyKernelAsCopyLocked), but a kernel exporting
  // from its pixel shader needs the sprite's pixels: the new area's minimap came out partly empty (green stripes).
  if (primitive == kPointList && State().point_sprite_gs)
    key.point_sprite = 1;
  if (per_instance) {
    key.per_instance_streams = 1;
    // Which way the tessellated grid winds depends on how the shader weights
    // the control points, so it is not culled.
    key.mode_control &= ~3u;
  }
  // fnxf_xiii (fnxf_xiii_gpu_blend_skip): from the fields above.
  // fnxf_xiii (2026-10-19, render-thread proposal 6): both masks again for the same fields they read (a one-entry memo:
  // OutputTrimMask built a BlendDesc per target at every draw). The fields: whether there is a pixel shader, the
  // targets' count and formats, their blend controls and write mask, alpha to coverage, the depth format and control.
  {
    struct MaskInputs {
      u32 has_ps, color_count, color_mask, alpha_to_coverage, depth_control;
      plume::RenderFormat color_formats[d3d::kMaxRenderTargets];
      plume::RenderFormat depth_format;
      u32 blend_control[d3d::kMaxRenderTargets];
    };
    MaskInputs in;
    std::memset(&in, 0, sizeof(in));
    in.has_ps = key.pixel_shader != nullptr;
    in.color_count = key.color_count;
    in.color_mask = key.color_mask;
    in.alpha_to_coverage = key.alpha_to_coverage;
    in.depth_control = key.depth_control;
    std::memcpy(in.color_formats, key.color_formats, sizeof(in.color_formats));
    in.depth_format = key.depth_format;
    std::memcpy(in.blend_control, key.blend_control, sizeof(in.blend_control));
    static MaskInputs last_in;
    static u32 last_skip = 0, last_trim = 0;
    static bool last_valid = false;
    if (!last_valid || std::memcmp(&in, &last_in, sizeof(in)) != 0) {
      last_skip = BlendSkipMode(key);
      last_trim = OutputTrimMask(key);
      last_in = in;
      last_valid = true;
    }
    if (fnxf_xiii::renut_gpu::BlendSkipOn() && !BlendSkipExcluded(key.pixel_shader))
      key.blend_skip = last_skip;
    // fnxf_xiii (fnxf_xiii_gpu_output_trim, package 28): from the fields above too (the same pipelines: they differ by them).
    if (fnxf_xiii::renut_gpu::OutputTrimOn())
      key.output_trim = last_trim;
  }
  // fnxf_xiii (fnxf_xiii_deko3d_alpha_spec, 2026-10-18): the alpha test's function as the pixel shader's constant 903 (+1):
  // without the test (7) no compare and no discard, and the GPU tests depth before the pixel shader.
  if (ps && fnxf_xiii::renut_gpu::AlphaSpecOn())
    key.alpha_func = alpha_test_func + 1;
  // fnxf_xiii (2026-10-20, in-run A/B targets 15 / 16): in the off half of fnxf_xiii_deko3d_light_branch's or
  // fnxf_xiii_deko3d_gpr32's A/B the pipelines are built without its constant (911 / 910): keys of their own (reserved
  // bits 0 / 1), so both halves' pipelines stay built. 0 otherwise (the keys as before).
  if (ps) {
    const int ab_off = fnxf_xiii::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
    if (ab_off == 15 && fnxf_xiii::renut_gpu::LightBranchOn())
      key.reserved |= 1u;
    else if (ab_off == 16 && fnxf_xiii::renut_gpu::Gpr32On())
      key.reserved |= 2u;
  }
  // fnxf_xiii (2026-10-20, in-run A/B target 17): fnxf_xiii_deko3d_skin_once's off half, for the vertex shaders it applies to
  // only (the other pipelines stay the same keys).
  if (vs && IsSkinOnceVs(vs->hash) && fnxf_xiii::renut_gpu::SkinOnceOn() &&
      fnxf_xiii::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 17)
    key.reserved |= 4u;
  return key;
}

// ---------------------------------------------------------------------------
// Memexport. N&B fills vertex buffers (and other data) from vertex shaders that
// write to memory: `mad eA, index, const0100, stream` then eM# (see memExport in
// shader_common.h). A draw whose shaders do gets a window in the export buffer
// per stream constant it uses, seeded from guest memory so unwritten bytes keep
// their value; after the draw the windows are copied to the readback buffer,
// and before anything reads that memory (a draw that reads it, a kickoff or
// present) the list is submitted and waited, and the
// results are written to guest memory, where every other path reads them from.
// Waiting at every draw instead cost a GPU round trip per export draw.

struct MemExportStream {
  u32 base = 0;  // guest physical byte address
  u32 size = 0;  // bytes
  u32 format = 0;      // color format (MemExportElementSize)
  u32 endian = 0;      // Endian128
  u32 num_format = 0;  // SurfaceNumberFormat
};

// Bytes per element of a memexport format: the game's bits per pixel for a
// format of single texels in whole dwords, which is what the shaders write
// (memExport in shader_common.h); 0 for any other.
u32 MemExportElementSize(u32 format) {
  const GpuTextureFormat f = GpuTextureFormat(format & 0x3F);
  u32 block_width, block_height;
  FormatBlockSize(f, block_width, block_height);
  const u32 bpp = FormatBitsPerPixel(f);
  return block_width == 1 && block_height == 1 && bpp % 32 == 0 && bpp <= 128 && bpp != 96
             ? bpp / 8
             : 0;
}

constexpr u32 kMaxMemExportStreams = 4;
constexpr u32 kMaxMemExportBytes = 64u << 20;

// The streams the shader's stream constants describe, laid out as N&B's own
// export passes build them (sub_82317020 before D3DDevice_EndExport): x the
// GPU address in dwords (bits 0-29) tagged 01 in bits 30-31, z the endian in
// bits 0-2, the format in 8-13 and the number format in 16-18, w the element
// count in bits 0-22 (y, z and w carry 0x4B high bits to stay floats).
void GatherMemExportStreams(const ShaderCacheEntry *entry, const FloatConstants &constants,
                            MemExportStream *streams, u32 &count) {
  if (!entry)
    return;
  // fnxf_xiii (2026-10-19, render-thread proposal 4): whether the shader exports at all, once per entry (static): the
  // 99 % of shaders without exports no longer read their eight cold memExportConstants words at every draw.
  struct ExportSummary {
    const ShaderCacheEntry *entry = nullptr;
    bool any = false;
  };
  static ExportSummary summaries[512];
  const uintptr_t a = reinterpret_cast<uintptr_t>(entry);
  ExportSummary &summary = summaries[((a >> 3) ^ (a >> 12)) & 511];
  if (summary.entry != entry) {
    u32 any = 0;
    for (u32 word = 0; word < 8; ++word)
      any |= entry->memExportConstants[word];
    summary.any = any != 0;
    summary.entry = entry;
  }
  if (!summary.any)
    return;
  // Only the registers the shader exports through (most draws have none).
  for (u32 word = 0; word < 8; ++word) {
    for (u32 bits = entry->memExportConstants[word]; bits; bits &= bits - 1) {
      const u32 r = word * 32 + u32(std::countr_zero(bits));
      const auto *words = reinterpret_cast<const be_u32 *>(constants.Register(r));
      const u32 x = words[0], z = words[2], w = words[3];
      const u32 element = MemExportElementSize((z >> 8) & 0x3F);
      const u32 elements = w & 0x7FFFFF;
      if ((x >> 30) != 1 || !element || !elements) {
        static uint32_t logged = 0;
        if ((x >> 30) == 1 && LogLimited(logged, 8)) {
          RGPU_WARN("Memexport stream in c{} not exported: {:08X} {:08X} {:08X} {:08X} (format "
                    "{}, {} elements)",
                    r, x, u32(words[1]), z, w, (z >> 8) & 0x3F, elements);
        }
        continue;
      }
      MemExportStream stream;
      stream.base = (x & 0x3FFFFFFF) * 4;
      stream.size = std::min(elements * element, kMaxMemExportBytes);
      stream.format = (z >> 8) & 0x3F;
      stream.endian = z & 7;
      stream.num_format = (z >> 16) & 7;
      if (stream.base >= 0x20000000u)
        continue;
      stream.size = std::min(stream.size, 0x20000000u - stream.base);
      bool known = false;
      for (u32 i = 0; i < count; ++i)
        known |= streams[i].base == stream.base && streams[i].size == stream.size;
      if (!known && count < kMaxMemExportStreams)
        streams[count++] = stream;
    }
  }
}

struct PendingExport {
  u32 base = 0;
  u32 size = 0;
  // Its window in the slot's export buffer, and its copy in the slot's
  // readback buffer (the same offset in both).
  uint64_t offset = 0;
  // fnxf_xiii (2026-10-21): written since its window was last copied to the readback buffer
  // (FlushExportReadbacksLocked copies it once, at the end of the slot's recording).
  bool dirty = true;
  // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): g_flush_serial when the window was seeded (a kept window:
  // when its pages were checked against guest memory): a page the cards show no game write in since equals guest
  // memory once its results are copied by the mask (CopyPage), which the kept window records (KeptWindow::version).
  u32 seed_interval = 0;
  // fnxf_xiii (2026-10-30, ZeroCopyOn): a draw of the window ran as a shader (its writes are known only to the GPU: the
  // page flags), not only as buffer copies (CopyKernelAsCopyLocked, whose destinations g_zero_copy_written has).
  bool shader = false;
};
// The exports a slot's lists made. They reach guest memory once the slot's
// fence is waited (detail::CompleteMemExportsLocked), a frame later; until
// then their windows stay in the slot's export buffer, where the next frame's
// draws seed from them on the GPU (SeedFromPendingExportsLocked).
struct SlotExports {
  std::vector<PendingExport> pending;
  uint64_t used = 0;
};
SlotExports g_exports[kFramesInFlight];
// fnxf_xiii (2026-10-24): each window's seed as the CPU copied it from guest memory (its pieces in the slot's upload
// ring, which is kept until the slot's fence is waited, after CompleteMemExportsLocked). The completion writes back only
// what the GPU changed in a page the CPU (or another completion) wrote since the seed: a whole window copied back put the
// seed's old bytes over what the game wrote in the window's 32 MB while the kernel ran (the game's character textures
// share the texture kernels' pools: transparent or black hair, at random).
struct ExportSeedPiece {
  uint64_t window = 0;  // the window's offset in the slot's export buffer
  u32 at = 0;           // offset in the window
  u32 size = 0;
  u8 *cpu = nullptr;
  u32 guest = 0;        // the physical address the piece was copied from (the window's base + at)
  u32 interval = 0;     // fnxf_xiii (2026-10-27): g_flush_serial when it was copied (HarvestPageLocked)
  // fnxf_xiii (2026-10-28): copied by the GPU from a kept window (SeedKeptWindowLocked): the CPU's cache lines of it
  // are invalidated before it is read (CopyPage), and no other slot's completion writes it.
  bool gpu = false;
};
std::vector<ExportSeedPiece> g_export_seeds[kFramesInFlight];
// fnxf_xiii (2026-10-28): the seed pieces' memory, 2 MB blocks of their own (they were in the slot's upload ring, reset
// when the slot's next frame begins, which a deferred completion outlives: DeferMemExportsLocked). A slot's blocks
// return to the pool when its completion is finished; the pool is emptied with the export buffers
// (ReleaseIdleExportBuffersLocked). One pool for the seeds the CPU copies from guest memory and those the GPU copies
// from a kept window (a window is one or the other): two pools made each burst allocate both (up to 192 MB a slot,
// 0.4 ms a MB to make, 2026-10-28). A block's lines: the CPU cleans what it writes (the seed's copy, and
// CompleteMemExportsLocked's updates of another slot's seeds), so none is dirty when the GPU writes the block; it drops
// its lines before reading what the GPU wrote (CopyPage); and the GPU's L2 is invalidated before it reads a block the
// CPU wrote that it wrote before (SeedBlocksWrittenByGpuLocked, PrepareMemExportLocked).
struct SeedBlock {
  plume::RenderBuffer *buffer = nullptr;  // its chunk's (g_seed_chunks)
  uint64_t offset = 0;                     // in the chunk
  u8 *cpu = nullptr;
  bool gpu_written = false;  // its last use was a GPU copy into it
};
constexpr u32 kSeedBlockBytes = 2u << 20;
// fnxf_xiii (2026-10-28): the blocks are made 16 at a time, as parts of one 32 MB buffer: each memory block deko3d
// makes cost about 1.5 ms however small (2026-10-28's load_detail run: 2 MB blocks 1.5-2.8 ms each, the 128 MB export
// buffer 12-15 ms), and a burst's first frame made 48 of them (about 80 of its 89-105 ms of "buffers").
constexpr u32 kSeedChunkBlocks = 16;
std::vector<std::unique_ptr<plume::RenderBuffer>> g_seed_chunks;
std::vector<std::unique_ptr<SeedBlock>> g_seed_free[1];
std::vector<std::unique_ptr<SeedBlock>> g_seed_used[kFramesInFlight][1];

// Whether a free block the GPU wrote last would be handed to the CPU's seeds next.
bool SeedBlocksWrittenByGpuLocked() {
  for (const auto &block : g_seed_free[0])
    if (block->gpu_written)
      return true;
  return false;
}

SeedBlock *SeedBlockLocked(u32 slot, bool gpu_written) {
  std::vector<std::unique_ptr<SeedBlock>> &free = g_seed_free[0];
  if (free.empty()) {
    // A chunk of 16 blocks (one block's worth when the heap cannot give 32 MB in one piece).
    for (const u32 blocks : {kSeedChunkBlocks, 1u}) {
      std::unique_ptr<plume::RenderBuffer> chunk = State().device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
          uint64_t(kSeedBlockBytes) * blocks, plume::RenderBufferFlag::HOST_CACHED));
      if (!chunk)
        continue;
      chunk->setName("renut memexport seeds");
      u8 *cpu = static_cast<u8 *>(chunk->map());
      if (!cpu)
        return nullptr;
      for (u32 i = 0; i < blocks; ++i) {
        auto block = std::make_unique<SeedBlock>();
        block->buffer = chunk.get();
        block->offset = uint64_t(i) * kSeedBlockBytes;
        block->cpu = cpu + block->offset;
        free.push_back(std::move(block));
      }
      g_seed_chunks.push_back(std::move(chunk));
      break;
    }
    if (free.empty())
      return nullptr;
  }
  std::unique_ptr<SeedBlock> block = std::move(free.back());
  free.pop_back();
  block->gpu_written = gpu_written;
  SeedBlock *raw = block.get();
  g_seed_used[slot][0].push_back(std::move(block));
  return raw;
}

void ReleaseSeedBlocksLocked(u32 slot) {
  for (u32 kind = 0; kind < 1; ++kind) {
    for (auto &block : g_seed_used[slot][kind])
      g_seed_free[kind].push_back(std::move(block));
    g_seed_used[slot][kind].clear();
  }
}
// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_parallel_seeds): one 2 MB piece of a window's seed, from guest memory into
// a seed block (then cleaned out of the CPU's cache for the GPU's copy), for the copy helpers (CopyHelpers::RunSeeds).
struct SeedJob {
  u8 *dst;
  const u8 *src;
  u32 size;
  plume::RenderBuffer *buffer;  // the block's chunk, and the piece's offset in it (for the clean)
  uint64_t offset;
};
std::vector<SeedJob> g_seed_jobs;  // RunSeedJobs (declared with the completion's, above the namespace) runs them
bool ParallelSeeds() {
  static const bool on = REXCVAR_GET(fnxf_xiii_memexport_parallel_seeds);
  return on;
}
// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the dword ranges the frame's buffer copies wrote in place of
// the game's copy kernel (CopyKernelAsCopyLocked), by slot, until FlushExportMaskRangesLocked marks them.
struct MaskRange {
  uint64_t offset;  // in the slot's export buffer (bytes, 4-aligned)
  uint64_t bytes;
};
std::vector<MaskRange> g_mask_ranges[kFramesInFlight];
// fnxf_xiii (2026-10-29): the seed interval of each of the draw's streams as PrepareMemExportLocked seeded them (0: a
// reused window), for FinishMemExportLocked (PendingExport::seed_interval).
u32 g_prep_interval[kMaxMemExportStreams];
// fnxf_xiii (2026-10-28): per physical page, bumped by every completion that takes a page of results back to guest memory
// (CompleteMemExportsLocked): a kept window's page whose version is still the one it was recorded with was not written
// by any results since (the game's writes are the cards').
uint32_t g_page_version[0x20000];
void ForgetKeptWindowsLocked(u32 slot);
uint64_t g_last_export_frame = 0;  // fnxf_xiii: ReleaseIdleExportBuffersLocked
uint64_t SlotWindowLocked(u32 slot, u32 base, u32 size);  // fnxf_xiii
bool GrowSlotExportBuffersLocked(VideoState &s, uint64_t needed);  // fnxf_xiii (2026-10-24): DrawNeedsExportFlush
// fnxf_xiii (2026-10-24): fnxf_xiii_deko3d_direct_exports (deko3d only: its buffers are all CPU-mapped).
bool DirectExports() {
  static const bool on = IsDeko3D() && REXCVAR_GET(fnxf_xiii_deko3d_direct_exports);
  return on;
}
// fnxf_xiii (2026-10-27): a window's page flags (shader_common_glsl.h, memExport: one word a 4 KB page, set to 1 when
// a kernel writes the page); read by CompleteMemExportsLocked, which takes back only the flagged pages. Only where the
// CPU reads the export buffer itself (DirectExports).
// fnxf_xiii (2026-10-28): in the first MB of the slot's export buffer, a word for each 4 KB of the buffer (the window
// at offset O has its flags at O / 1024), and the windows from 1 MB on, each 4 KB-aligned, with nothing between them:
// two windows of neighbouring guest memory drawn in order are neighbours in the buffer too, so a texture across them
// is read on the GPU from one range (FindExportWindow). The flags followed each window before, which left 32 KB
// between them (the game's 4096x4096 texture at 10B88000, across the 0EE00000 and 10E00000 windows, was waited for and
// untiled by the CPU: 42-126 ms in an area load's frames).
constexpr uint64_t kExportFlagPrefix = 1u << 20;  // flags for 1 GB of windows
uint64_t ExportFlagBytes(u32 size) {
  return DirectExports() ? (uint64_t(size) + 4095u) / 4096u * 4u : 0;
}
uint64_t ExportWindowSpan(u32 size) {
  return DirectExports() ? (uint64_t(size) + 4095u) & ~uint64_t(4095) : (uint64_t(size) + 255u) & ~uint64_t(255);
}
uint64_t ExportFlagOffset(uint64_t window, u32) {
  return window >> 10;
}
// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the write mask after the flags, a bit for each dword of the
// buffer (shader_common_glsl.h memExportMark: FNXF_XIII_MEMEXPORT_MASK_WORDS = kExportFlagPrefix / 4), 8 MB for the
// first 256 MB of the buffer (kExportMaskCover): the windows end at fnxf_xiii_memexport_slot_mb (128 MB), and a buffer
// grown past the cover for one draw (DrawNeedsExportFlush skips the cap when the slot has no window yet) has that
// draw's exports refused instead (PrepareMemExportLocked), so the shader needs no check. The windows start after both.
constexpr uint64_t kExportMaskPrefix = 8u << 20;
constexpr uint64_t kExportMaskCover = kExportMaskPrefix * 32;
bool ExportMaskOn() {
  static const bool on = DirectExports() && REXCVAR_GET(fnxf_xiii_memexport_write_mask);
  return on && !ZeroCopyOn();  // fnxf_xiii (2026-10-30): no mask when the windows are guest memory itself
}
uint64_t ExportPrefix() {
  return kExportFlagPrefix + (ExportMaskOn() ? kExportMaskPrefix : 0);
}
// The byte offset of the mask word that holds the bit of the buffer's byte 'offset' (4-aligned), and the mask bytes
// of a window's span (4 KB-aligned: 128 bytes of mask a page).
uint64_t ExportMaskWordOffset(uint64_t offset) {
  return kExportFlagPrefix + ((offset >> 7) << 2);
}
uint64_t ExportMaskBytes(uint64_t span) {
  return span >> 5;
}
// Where the slot's next fresh window may start: after its windows, and never in the flags' MB (and the mask's).
uint64_t ExportWindowsEnd(uint64_t used) {
  return DirectExports() ? std::max(used, ExportPrefix()) : used;
}
// fnxf_xiii (2026-10-29): fnxf_xiii_memexport_slot_mb as bytes (16 MB at least), and never past the write mask's cover.
uint64_t ExportSlotLimit() {
  const uint64_t limit = uint64_t(std::max(16, REXCVAR_GET(fnxf_xiii_memexport_slot_mb))) << 20;
  return ExportMaskOn() ? std::min(limit, kExportMaskCover) : limit;
}

plume::RenderBufferFlags ExportBufferFlags() {
  return StorageBufferFlags() | AddressableBufferFlags() |
         (DirectExports() ? plume::RenderBufferFlag::HOST_CACHED : plume::RenderBufferFlag::NONE);
}

// fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy; backend/guest_gpu_memory.h): the export windows are guest memory
// itself, through the GPU's mapping of all of physical memory. A window's offset in "the slot's export buffer" is then
// its physical address, in the one buffer every slot shares; its page flags are in a small buffer of the slot
// (g_zero_copy_flags), a word a physical page. No seeds, no copies of the results, no kept windows, no write mask.
bool ZeroCopyOn() {
  return guest_gpu::On();
}

// The buffer the slot's windows are in: the slot's export buffer, or the mapping of guest memory (ZeroCopyOn).
plume::RenderBuffer *ExportBuffer(VideoState &s, u32 slot) {
  if (ZeroCopyOn())
    return guest_gpu::Buffer();
  return s.memexport_buffer[slot].get();
}

// fnxf_xiii (2026-10-30, ZeroCopyOn): the slot's page flags (a word a physical page of guest memory, 512 KB; a kernel
// sets a page's to 1 when it writes the page) and the zeroes they are cleared with.
std::unique_ptr<plume::RenderBuffer> g_zero_copy_flags[kFramesInFlight];
constexpr uint64_t kZeroCopyFlagBytes = (0x20000000ull >> 12) * 4;  // 128 K pages

// fnxf_xiii (2026-10-30, ZeroCopyOn): what the GPU may read from guest memory itself, and what it must get as a copy
// taken when the draw is recorded. The game's fences complete when the render thread has recorded a batch (d3d/hooks/
// device.cpp), not when the GPU has run it: the game then writes memory the GPU is yet to read. The export buffers
// gave every window a copy of guest memory at record time (the seed), and the first zero-copy build read guest memory
// at execution time instead: the loading screen's icons, decoded by the CPU into the pool while the compaction kernel
// moved it, were read after the game had moved on (gone, fnxf_xiii_022.log). So a range the GPU reads is taken from
// guest memory in place only when it is a kernel's output no CPU write has touched since: a destination of a buffer
// copy (g_zero_copy_written, known here), or a page of a pending shader-run window the cards show no game write on
// since that window was prepared. Everything else is copied by the CPU as the draw is recorded (the streams'
// conversions, the textures' uploads, the copy kernel's source), without waiting for the kernels (guest memory has it
// as the game left it, which is what the game's fence promises it).
struct ZeroCopyRange {
  u32 base = 0, size = 0;
};
std::vector<ZeroCopyRange> g_zero_copy_written[kFramesInFlight];  // the pending buffer copies' destinations
uint64_t g_zero_copy_reads_gpu = 0, g_zero_copy_reads_snapshot = 0, g_zero_copy_reads_shader_gpu = 0;

bool ZeroCopyWrittenLocked(u32 base, u32 size) {
  for (const auto &slot : g_zero_copy_written)
    for (const ZeroCopyRange &r : slot)
      if (base < r.base + r.size && r.base < base + size)
        return true;
  return false;
}

// True: read in place on the GPU (CleanForGpu done); false: a record-time copy (the CPU paths, no wait). Only for a
// range PendingExportSlots says pending windows overlap.
bool ZeroCopyReadOnGpuLocked(u32 base, u32 size) {
  if (!size || uint64_t(base) + size > 0x20000000ull)
    return false;
  if (ZeroCopyWrittenLocked(base, size)) {
    ++g_zero_copy_reads_gpu;
    guest_gpu::CleanForGpu(base, size);
    return true;
  }
  // In a pending shader-run window: the kernel's output unless the game wrote it since the window was prepared.
  u32 interval = 0;
  bool shader = false;
  for (const SlotExports &slot : g_exports) {
    for (const PendingExport &p : slot.pending) {
      if (p.shader && base < p.base + p.size && p.base < uint64_t(base) + size) {
        shader = true;
        interval = interval ? std::min(interval, p.seed_interval) : p.seed_interval;
      }
    }
  }
  if (!shader) {
    ++g_zero_copy_reads_snapshot;
    return false;
  }
  if (!StoreCards()) {
    ++g_zero_copy_reads_snapshot;  // no cards: the game's writes are unknown
    return false;
  }
  // The pages' cards taken eight a load (a 4 MB texture's level is 1024 pages, asked about at every draw that samples
  // it while exports are pending), then their zero-copy intervals compared; the page before too (an unaligned store
  // marks its first byte's page only).
  const u32 first = base >> 12, last = u32((uint64_t(base) + size - 1) >> 12);
  HarvestPagesLocked(first ? first - 1 : 0, last);
  for (u32 page = first; page <= last; ++page) {
    if (g_game_zc_serial[page] >= interval || (page != 0 && g_game_zc_serial[page - 1] >= interval)) {
      ++g_zero_copy_reads_snapshot;
      return false;
    }
  }
  ++g_zero_copy_reads_shader_gpu;
  guest_gpu::CleanForGpu(base, size);
  return true;
}

// The 64 KiB granules of physical memory any pending export writes: most
// ranges a draw reads touch none, and skip the lists (PendingExportSlots runs
// for every draw's streams and textures while exports are pending, which is
// always).
constexpr u32 kExportGranuleShift = 16;
constexpr u32 kExportGranules = 0x20000000u >> kExportGranuleShift;
uint64_t g_export_granules[kExportGranules / 64];

void MarkExportGranules(u32 base, u32 size) {
  if (!size)
    return;
  const u32 first = (base & 0x1FFFFFFFu) >> kExportGranuleShift;
  const u32 last = std::min((((base & 0x1FFFFFFFu) + size - 1) >> kExportGranuleShift),
                            kExportGranules - 1);
  for (u32 g = first; g <= last; ++g)
    g_export_granules[g / 64] |= uint64_t(1) << (g % 64);
}

void RebuildExportGranules() {
  std::memset(g_export_granules, 0, sizeof(g_export_granules));
  for (const SlotExports &slot : g_exports) {
    for (const PendingExport &p : slot.pending)
      MarkExportGranules(p.base, p.size);
  }
}

// A word of the bitmap at a time: the bits of the first and last words that
// are in the range, the words between whole.
bool ExportGranulesTouched(u32 base, u32 size) {
  const u32 first = (base & 0x1FFFFFFFu) >> kExportGranuleShift;
  const u32 last = std::min((((base & 0x1FFFFFFFu) + size - 1) >> kExportGranuleShift),
                            kExportGranules - 1);
  if (last < first)
    return false;
  const u32 first_word = first / 64, last_word = last / 64;
  const uint64_t first_mask = ~uint64_t(0) << (first % 64);
  const uint64_t last_mask = ~uint64_t(0) >> (63 - last % 64);
  if (first_word == last_word)
    return (g_export_granules[first_word] & first_mask & last_mask) != 0;
  if (g_export_granules[first_word] & first_mask)
    return true;
  for (u32 w = first_word + 1; w < last_word; ++w) {
    if (g_export_granules[w])
      return true;
  }
  return (g_export_granules[last_word] & last_mask) != 0;
}

bool AnyPendingExports();
// fnxf_xiii (2026-11-16, fnxf_xiii_fence_waits_exports): a D3D fence tells the game its GPU work is done; the GPU's
// memexport results (the game's copy kernel moving its physical heap's blocks) must be in guest memory by then, or the
// game writes the freed memory (a loading picture) and the late results land over it. Render thread, before a fence
// completes: every pending export brought to guest memory (submit and wait when they are this frame's).
void WaitForExportsLocked(u32 slots);
}  // namespace (SettleExportsForFence is called from backend/render_thread.cpp)
void SettleExportsForFence() {
  static const bool on = REXCVAR_GET(fnxf_xiii_fence_waits_exports);
  if (!on)
    return;
  std::lock_guard lock(State().mutex);
  if (!AnyPendingExports())
    return;
  WaitForExportsLocked((1u << kFramesInFlight) - 1u);
}
namespace {

bool AnyPendingExports() {
  for (const SlotExports &slot : g_exports) {
    if (!slot.pending.empty())
      return true;
  }
  return false;
}

// A GPU physical address, or a guest VA in the physical windows, as a physical
// address (other VAs come out as unrelated addresses, which at worst flush the
// exports early).
u32 PhysicalAddress(u32 address) {
  return address >= 0x20000000u ? d3d::CpuToGpuAddress(address) & 0x1FFFFFFFu : address;
}

// The slots (a mask) with a pending export that will write memory overlapping
// [address, address + size).
u32 PendingExportSlots(u32 address, u32 size) {
  if (!size)
    return 0;
  const u32 base = PhysicalAddress(address);
  if (!ExportGranulesTouched(base, size))
    return 0;
  FinishDeferredOverLocked(base, size);  // fnxf_xiii (2026-10-28): the reader gets them in guest memory
  const int deferred = DeferredSlotLocked();  // still deferred: none of its written pages is in the range
  u32 slots = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (int(slot) == deferred)
      continue;
    for (const PendingExport &p : g_exports[slot].pending) {
      if (base < p.base + p.size && p.base < base + size) {
        slots |= 1u << slot;
        break;
      }
    }
  }
  return slots;
}

// The pending export window holding the newest data of all of [base, base +
// size) (physical): the newest pending export overlapping it, if it covers it
// whole. Its slot and the offset of 'base' in that slot's export buffer.
bool FindExportWindow(u32 base, u32 size, u32 &slot, uint64_t &offset) {
  FinishDeferredOverLocked(base, size);  // fnxf_xiii (2026-10-28): a deferred slot's windows are not the newest
  const int deferred = DeferredSlotLocked();  // still deferred: none of its written pages is in the range
  const auto &s = State();
  // Oldest slot first, and a slot's exports in the order they were drawn.
  const PendingExport *newest = nullptr;
  u32 newest_slot = 0;
  for (u32 k = 1; k <= kFramesInFlight; ++k) {
    const u32 at = (s.frame + k) % kFramesInFlight;
    if (int(at) == deferred)
      continue;
    for (const PendingExport &p : g_exports[at].pending) {
      if (base < p.base + p.size && p.base < base + size) {
        newest = &p;
        newest_slot = at;
      }
    }
  }
  if (!newest)
    return false;
  // fnxf_xiii (2026-10-30, ZeroCopyOn): the windows are guest memory itself: a range the GPU may read in place
  // (ZeroCopyReadOnGpuLocked: a kernel's output the game has not written since; its CPU lines cleaned there) is in the
  // one buffer at its physical address, whatever windows hold its parts. Any other range gets a record-time copy.
  if (ZeroCopyOn()) {
    if (uint64_t(base) + size > 0x20000000ull)
      return false;  // not physical memory
    slot = newest_slot;
    offset = base;
    return true;  // the reader decides with its exact range (ZeroCopyReadOnGpuLocked)
  }
  if (base >= newest->base && uint64_t(base) + size <= uint64_t(newest->base) + newest->size) {
    slot = newest_slot;
    offset = newest->offset + (base - newest->base);
    return true;
  }
  // fnxf_xiii (2026-10-28): a range across windows of one slot that are neighbours both in guest memory and in its
  // export buffer (kExportFlagPrefix), with no other slot's window over it: read from them as one.
  const PendingExport *chain[8];
  u32 n = 0;
  for (u32 at = 0; at < kFramesInFlight; ++at) {
    if (int(at) == deferred)
      continue;
    for (const PendingExport &p : g_exports[at].pending) {
      if (!(base < p.base + p.size && p.base < base + size))
        continue;
      if (at != newest_slot || n == 8)
        return false;
      chain[n++] = &p;
    }
  }
  std::sort(chain, chain + n, [](const PendingExport *a, const PendingExport *b) { return a->base < b->base; });
  if (n < 2 || base < chain[0]->base ||
      uint64_t(base) + size > uint64_t(chain[n - 1]->base) + chain[n - 1]->size)
    return false;
  for (u32 i = 1; i < n; ++i) {
    if (chain[i]->base != chain[i - 1]->base + chain[i - 1]->size ||
        chain[i]->offset != chain[i - 1]->offset + chain[i - 1]->size)
      return false;
  }
  slot = newest_slot;
  offset = chain[0]->offset + (base - chain[0]->base);
  static uint32_t logged = 0;
  if (logged++ < 8)
    FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] {:08X}-{:08X} read on the GPU from {} neighbouring windows of slot {}", base,
              uint64_t(base) + size, n, slot);
  return true;
}

// Whether UploadVertexFetchData copies the buffer of fetch constant
// 'fetch_index' (read by vfetchMemory / vfetchBound) from a pending export's
// window on the GPU, which it does when the window holds all of it.
bool FetchFromExportWindow(const GuestDevice &device, const DrawCall &call, u32 fetch_index,
                           u32 &slot, uint64_t &offset) {
  if (call.up_vertices && fetch_index == d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(0))
    return false;
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = std::min(device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu, kMaxFetchCopyBytes);
  return base && size && PendingExportSlots(base, size) && FindExportWindow(base, size, slot, offset) &&
         (!ZeroCopyOn() || ZeroCopyReadOnGpuLocked(base, size));  // fnxf_xiii (2026-10-30): a kernel's output
}

// A vertex stream built on the GPU from the export window that holds its
// newest data (ConvertExportedStreamsLocked).
struct ExportedStream {
  u32 slot = 0;          // whose export buffer holds the window
  uint64_t offset = 0;   // of the stream's first byte there
  u32 stride = 0;        // guest
  u32 host_stride = 0;
  u32 vertex_count = 0;  // in the stream's buffer
  u32 element_count = 0;
  u32 elements[2 * kStreamConvertMaxElements] = {};  // StreamConvertPush::elements
};

// Whether a draw can read vertex stream 'stream' from a pending export's
// window instead of guest memory: the window holds the newest data of all of
// the stream's buffer (FindExportWindow), and stream_convert_cs can build its
// host layout (word-aligned elements, few enough of them). Shader reads
// through vfetchBound get the window's bytes too (UploadVertexFetchData).
// fnxf_xiii (2026-10-30, ZeroCopyOn): with 'allow_snapshot' (CopyKernelAsCopyLocked), a stream the GPU may not read in
// place (ZeroCopyReadOnGpuLocked: the game may write it before the GPU runs), or none of whose memory has exports
// pending, is still described, with out.slot = kSnapshotSlot and out.offset its physical address: the caller copies it
// from guest memory as the draw is recorded.
constexpr u32 kSnapshotSlot = UINT32_MAX;
bool FindExportedStream(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                        const u32 *shader_strides, const VertexDeclarationRecord &declaration,
                        u32 stream, ExportedStream &out, bool allow_snapshot = false) {
  const auto &s = State();
  if (!s.stream_convert_pipeline || stream >= kMaxVertexStreams || (stream == 0 && call.up_vertices))
    return false;
  const HostVertexStream &hs = declaration.layout.streams[stream];
  if (!hs.used)
    return false;
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
  const bool snapshot = allow_snapshot && ZeroCopyOn();
  if (!base || !size || (!snapshot && !PendingExportSlots(base, size)))
    return false;
  if (snapshot && uint64_t(base) + size > 0x20000000ull)
    return false;
  const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
  if (!src.stride || src.stride % 4 || src.stride / 4 > kStreamConvertMaxStrideWords ||
      hs.host_stride % 4 || hs.elements.size() > kStreamConvertMaxElements) {
    return false;
  }
  u32 elements[2 * kStreamConvertMaxElements] = {};
  for (size_t i = 0; i < hs.elements.size(); ++i) {
    const HostVertexElement &e = hs.elements[i];
    if (e.guest_offset % 4 || e.guest_offset + 4 > src.stride)
      return false;
    const bool expand = e.conversion == ElementConversion::ExpandFloat;
    // As ConvertVertices: swapping the halves too is the endian mode ^ 3.
    const u32 endian = !expand && e.swap_halves ? src.endian ^ 3 : src.endian;
    u32 type = e.type.format | (e.type.is_signed ? 1u << 8 : 0u) | (e.type.is_integer ? 1u << 9 : 0u);
    for (u32 c = 0; c < 4; ++c)
      type |= u32(e.type.swizzle[c] & 7) << (10 + 3 * c);
    elements[2 * i] = (e.guest_offset / 4) | (e.host_offset / 4) << 8 | endian << 16 |
                      (expand ? 1u : 0u) << 18 | (e.host_size / 4 & 7) << 19;
    elements[2 * i + 1] = type;
  }
  if (snapshot && !(PendingExportSlots(base, size) && FindExportWindow(base, size, out.slot, out.offset))) {
    out.slot = kSnapshotSlot;  // no exports pending over it: guest memory as the CPU reads it now
    out.offset = base;
  } else if (!snapshot && !FindExportWindow(base, size, out.slot, out.offset)) {
    return false;
  }
  out.stride = src.stride;
  out.host_stride = hs.host_stride;
  out.vertex_count = size / src.stride;
  out.element_count = u32(hs.elements.size());
  std::memcpy(out.elements, elements, sizeof(elements));
  return true;
}

// Why a draw needs the pending exports in guest memory first (see
// DrawNeedsExportFlush).
enum class ExportRead : u32 {
  None,
  Stream,    // a declared vertex stream
  Fetch,     // a vertex fetch by address (vfetchMemory)
  Index,     // the index buffer
  Texture,
  Capacity,  // its exports would not fit the readback buffer beside theirs
};

// Whether pending exports must reach guest memory before a draw, and whose
// ('slots', a mask): it reads memory they write (its vertex streams, bound or
// read by address, indices or textures), or its exports would not fit this
// slot's buffers beside theirs (growing them would lose their windows). Any
// other draw can be recorded while they are still on the GPU, and so can one
// whose streams are built from the export windows (FindExportedStream) or
// whose own exports are seeded from them (SeedFromPendingExportsLocked).
ExportRead DrawNeedsExportFlush(const GuestDevice &device, const DrawCall &call,
                                const ShaderRecord &vs, const ShaderRecord *ps,
                                const VertexDeclarationRecord &declaration,
                                const u32 *shader_strides, const MemExportStream *exports, u32 count,
                                u32 &slots) {
  const u32 current = 1u << State().frame;
  auto fetch_slots = [&](u32 fetch_index) {
    const u32 w0 = device.VertexFetchWord(fetch_index, 0);
    const u32 w1 = device.VertexFetchWord(fetch_index, 1);
    return PendingExportSlots(w0 & ~3u, w1 & 0x3FFFFFCu);
  };
  auto found = [&](ExportRead read, u32 found_slots) {
    slots = found_slots;
    return read;
  };
  u32 streams_checked = 0;
  for (const GuestVertexElement &e : declaration.elements) {
    const u32 stream = u16(e.Stream);
    if (stream >= kMaxVertexStreams || (streams_checked & (1u << stream)))
      continue;
    streams_checked |= 1u << stream;
    // A Begin/DrawVerticesUP draw's stream 0 is its own vertices, whatever
    // buffer the fetch constant still names: nothing of it is read from
    // memory.
    if (stream == 0 && call.up_vertices)
      continue;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    if (const u32 found_slots = fetch_slots(fetch_index)) {
      // A stream the host layout reads is built from the export window; one it
      // does not (no host input) is read only by vfetchBound, whose copy the
      // window supplies (UploadVertexFetchData).
      ExportedStream exported;
      u32 slot;
      uint64_t offset;
      const bool on_gpu =
          declaration.layout.streams[stream].used
              ? FindExportedStream(device, call, vs, shader_strides, declaration, stream, exported)
              : FetchFromExportWindow(device, call, fetch_index, slot, offset);
      // fnxf_xiii (2026-10-30, ZeroCopyOn): no wait: the draw's range of the stream is read in place when it is a
      // kernel's output (ConvertExportedStreamsLocked, ZeroCopyReadOnGpuLocked) and converted from guest memory as
      // recorded otherwise, as the game's fence promises (ZeroCopyRange).
      if (!on_gpu && ZeroCopyOn())
        continue;
      if (!on_gpu)
        return found(ExportRead::Stream, found_slots);
    }
  }
  if (vs.entry) {
    for (u32 bits = vs.entry->vertexFetchMemoryMask; bits; bits &= bits - 1) {
      const u32 fetch_index = 64 + u32(std::countr_zero(bits));
      if (const u32 found_slots = fetch_slots(fetch_index)) {
        u32 slot;
        uint64_t offset;
        if (!FetchFromExportWindow(device, call, fetch_index, slot, offset)) {
          if (ZeroCopyOn())
            continue;  // fnxf_xiii (2026-10-30): copied as recorded (UploadVertexFetchData)
          return found(ExportRead::Fetch, found_slots);
        }
      }
    }
  }
  // A tessellated draw's "indices" are only adaptive edge factors, which set
  // its level (PrepareTessellation): the ones in guest memory, from a frame
  // before, set it as well. N&B's water computes them with memexport before
  // each draw, and waiting for them was two GPU round trips a frame.
  if (call.indexed && !call.up_indices && !call.tessellation) {
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    if (ib) {
      if (const u32 found_slots = PendingExportSlots(ib->Address, ib->Size)) {
        // fnxf_xiii (2026-10-30, ZeroCopyOn): read by the CPU as recorded unless they are a kernel's output.
        if (!(ZeroCopyOn() && !ZeroCopyReadOnGpuLocked(PhysicalAddress(ib->Address), ib->Size)))
          return found(ExportRead::Index, found_slots);
      }
    }
  }
  const u32 texture_mask =
      (vs.entry ? vs.entry->textureFetchMask : 0) | (ps && ps->entry ? ps->entry->textureFetchMask : 0);
  for (u32 bits = texture_mask; bits; bits &= bits - 1) {
    u32 pages[2], sizes[2];
    const TextureFetch tf = DeviceTextureFetch(device.dev(), u32(std::countr_zero(bits)));
    TextureGuestExtent(tf, pages, sizes);
    for (u32 i = 0; i < 2; ++i) {
      if (!pages[i])
        continue;
      if (const u32 found_slots = PendingExportSlots(pages[i] << 12, sizes[i])) {
        // fnxf_xiii (2026-10-27): made from the windows on the GPU at its bind (textures.cpp): no wait.
        if (ExportTextureOnGpuLocked(tf))
          continue;
        // fnxf_xiii (2026-10-30, ZeroCopyOn): not a kernel's output the GPU may read in place: uploaded from guest
        // memory as the draw is recorded, as the game's fence promises (ZeroCopyRange): no wait either.
        if (ZeroCopyOn() && !ZeroCopyReadOnGpuLocked(pages[i] << 12, sizes[i]))
          continue;
        // fnxf_xiii (2026-10-27): the textures the game's kernels make that a draw waits for (the GPU-side texture
        // path's scope: formats, sizes, tiling, mips), the first 48.
        static uint32_t logged = 0;
        if (logged++ < 48) {
          u32 w = 0, h = 0, d = 0;
          tf.Size(w, h, d);
          FNXF_XIII_BURST_LOG(RGPU_INFO, "[exporttex] VS {:016X} PS {:016X} fetch {}: format {} {}x{}x{} dim {} {} pitch {} endian {} mips "
                    "{}-{}{} {} at {:08X} ({:X} bytes)",
                    vs.hash, ps ? ps->hash : 0, std::countr_zero(bits), u32(tf.Format()), w, h, d, u32(tf.Dimension()),
                    tf.Tiled() ? "tiled" : "linear", tf.PitchTexels(), tf.Endian(), tf.MinMipLevel(), tf.MaxMipLevel(),
                    tf.PackedMips() ? " packed" : "", i ? "mips" : "base", pages[i] << 12, sizes[i]);
          // fnxf_xiii (2026-10-27): the pending windows over it (why the GPU path did not take it: one window must
          // hold a whole level).
          const uint64_t lo = uint64_t(pages[i]) << 12, hi = lo + sizes[i];
          for (u32 slot = 0; slot < kFramesInFlight; ++slot)
            for (const PendingExport &p : g_exports[slot].pending)
              if (p.base < hi && uint64_t(p.base) + p.size > lo)
                FNXF_XIII_BURST_LOG(RGPU_INFO, "[exporttex]   window of slot {}: {:08X}-{:08X} ({:X} bytes) at +{:X}", slot, p.base,
                          uint64_t(p.base) + p.size, p.size, p.offset);
        }
        return found(ExportRead::Texture, found_slots);
      }
    }
  }
  // fnxf_xiii (2026-10-21): exports that do not fit the slot's buffers beside the pending ones no longer wait for
  // them (ExportRead::Capacity, a GPU round trip: 160-240 ms in the game's area loads): PrepareMemExportLocked grows
  // the buffers and copies the pending windows over on the GPU (GrowSlotExportBuffersLocked).
  // Above fnxf_xiii_memexport_slot_mb (export window plus its readback, per slot) it still waits: the cap on what a burst of
  // exports may take (the buffers are released once exports stop, ReleaseIdleExportBuffersLocked).
  if (ZeroCopyOn())
    return ExportRead::None;  // fnxf_xiii (2026-10-30): the windows are guest memory: no buffer to fill
  uint64_t bytes = 0;
  for (u32 i = 0; i < count; ++i)
    if (SlotWindowLocked(State().frame, exports[i].base, exports[i].size) == UINT64_MAX)
      bytes += ExportWindowSpan(exports[i].size);  // fnxf_xiii (2026-10-27): with its page flags
  const uint64_t used = g_exports[State().frame].used;
  const uint64_t limit = ExportSlotLimit();
  if (bytes && used && ExportWindowsEnd(used) + bytes > limit)
    return found(ExportRead::Capacity, current);
  // fnxf_xiii (2026-10-24): the growth is made here, before anything of the draw is recorded, and when the heap cannot
  // give it the draw waits for the slot's exports instead (its windows then start again at 0, in the buffers the slot
  // has). PrepareMemExportLocked grew them itself, and a failure there dropped the draw's exports: the textures the game's
  // kernels build were left with holes (black hair in the tutorial battle).
  if (bytes && used) {
    auto &s = State();
    const uint64_t needed = ExportWindowsEnd(used) + bytes;  // fnxf_xiii (2026-10-28)
    const bool fits = s.memexport_buffer[s.frame] && s.memexport_capacity[s.frame] >= needed &&
                      (DirectExports() ||
                       (s.memexport_readback[s.frame] && s.memexport_readback_capacity[s.frame] >= needed));
    if (!fits && (!Video::EnsureCommandListLocked() || !GrowSlotExportBuffersLocked(s, needed)))
      return found(ExportRead::Capacity, current);
  }
  return ExportRead::None;
}

// fnxf_xiii: the offset of a window of this slot's export buffer that already holds [base, base + size) for an earlier
// draw of the slot, or UINT64_MAX. The game's texture kernels export many draws a frame into the same 32 MB windows (the
// texture pool): each draw had a window of its own, seeded with 32 MB from guest memory and read back whole, and the
// export buffers grew to gigabytes. A later draw of the same window writes on into it (its contents are the earlier
// draws' results over the seed), which also keeps the order of their writes.
uint64_t SlotWindowLocked(u32 slot, u32 base, u32 size) {
  if (DeferredSlotLocked() == int(slot))
    FinishDeferredLocked("an export draw of the slot");  // fnxf_xiii (2026-10-28): not this frame's windows
  for (const PendingExport &p : g_exports[slot].pending)
    if (p.base == base && p.size == size)
      return p.offset;
  return UINT64_MAX;
}

// fnxf_xiii (2026-10-24): the size a buffer grows to: doubling from 4 MB (32, 64, 128 MB), as before. Growing to what
// was needed plus a little (36, 68, 104 MB: round 1 of the memory fixes) left holes that no later block fitted (a freed
// 64 MB block cannot hold a 68 MB one): the heap's top reached 3123 of 3126 MB, where doubling had stayed near 2760.
uint64_t GrowBufferSize(uint64_t capacity, uint64_t size) {
  uint64_t grown = std::max<uint64_t>(capacity * 2, 4u << 20);
  while (grown < size)
    grown *= 2;
  return grown;
}

// fnxf_xiii (2026-10-24): a buffer the heap could not give, logged (not every time). Not a back-off: a pause made the
// draws that need the buffer go without it for its whole length (dropped draws), where a try costs ~10 ms.
uint64_t g_grow_failures = 0;

void NoteGrowFailed(uint64_t size, const char *name) {
  ++g_grow_failures;
  static uint32_t logged = 0;
  if (LogLimited(logged, 16))
    RGPU_WARN("[export] {} of {} MB could not be made (heap; {} so far)", name, size >> 20, g_grow_failures);
}

// Recreates 'buffer' with at least 'size' bytes when it is smaller. fnxf_xiii (2026-10-24): the new buffer is made
// before the old one goes, so a failure keeps the old one (it was retired first: the readback lost its 64 MB buffer
// when the 128 MB one failed).
bool EnsureBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t size,
                  const plume::RenderBufferDesc &desc_template, const char *name) {
  if (buffer && capacity >= size)
    return true;
  const uint64_t grown = GrowBufferSize(capacity, size);
  plume::RenderBufferDesc desc = desc_template;
  desc.size = grown;
  std::unique_ptr<plume::RenderBuffer> fresh = State().device->createBuffer(desc);
  if (!fresh) {
    NoteGrowFailed(grown, name);
    return false;
  }
  if (buffer)
    Video::RetireLocked(std::move(buffer));
  buffer = std::move(fresh);
  // fnxf_xiii (2026-10-27): a buffer of 64 MB or more, named in deko3d.log (written at once; the game log's last lines
  // are lost when the process stops): which one asked for the 256 MB block before the map-load crash.
#if defined(__SWITCH__)
  if (grown >= (64u << 20))
    plume::deko3dLog("renut buffer '%s' grown from %llu to %llu MB (%llu MB asked)", name,
                     (unsigned long long)(capacity >> 20), (unsigned long long)(grown >> 20),
                     (unsigned long long)(size >> 20));
#endif
  capacity = grown;
  buffer->setName(name);
  return true;
}

// fnxf_xiii (2026-10-27): room for 'need' more bytes in a buffer filled from 'used' on (its earlier regions still read
// by draws recorded before): a full one is followed by a fresh one of the same size (regions start again at 0), a
// larger one only when 'need' alone does not fit. They were grown to used + need, doubling at each refill within a
// frame (as the memexport scratch: 32, 64, 128 then 256 MB, the map-load crash of 2026-10-27).
bool RefillBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t &used, uint64_t need,
                  const plume::RenderBufferDesc &desc_template, const char *name) {
  if (buffer && used + need <= capacity)
    return true;
  if (buffer && need <= capacity) {
    plume::RenderBufferDesc desc = desc_template;
    desc.size = capacity;
    std::unique_ptr<plume::RenderBuffer> fresh = State().device->createBuffer(desc);
    if (!fresh) {
      NoteGrowFailed(desc.size, name);
      return false;
    }
    fresh->setName(name);
    Video::RetireLocked(std::move(buffer));
    buffer = std::move(fresh);
    capacity = desc.size;
#if defined(__SWITCH__)
    // fnxf_xiii (2026-10-27): refills of 32 MB or more named in deko3d.log (64 lines).
    static uint32_t logged = 0;
    if (desc.size >= (32u << 20) && logged++ < 64)
      plume::deko3dLog("renut buffer '%s' refilled: a fresh %llu MB (%llu MB asked)", name,
                       (unsigned long long)(desc.size >> 20), (unsigned long long)(need >> 20));
#endif
  } else if (!EnsureBuffer(buffer, capacity, need, desc_template, name)) {
    return false;
  }
  used = 0;
  return true;
}

// The current slot's export buffer, in its unordered-access state (created on
// first use).
plume::RenderBuffer *MemExportBufferLocked(VideoState &s, uint64_t size = 256) {
  if (ZeroCopyOn())
    return guest_gpu::Buffer();  // fnxf_xiii (2026-10-30): the mapping of guest memory, every slot's
  auto &buffer = s.memexport_buffer[s.frame];
  const bool had = buffer && s.memexport_capacity[s.frame] >= size;
  if (!had && DeferredSlotLocked() == int(s.frame))
    FinishDeferredLocked("the slot's export buffer was replaced");  // fnxf_xiii (2026-10-28)
  if (!had)
    ForgetKeptWindowsLocked(s.frame);  // fnxf_xiii (2026-10-28): a new buffer
  if (!EnsureBuffer(buffer, s.memexport_capacity[s.frame], size,
                    plume::RenderBufferDesc::DefaultBuffer(0, ExportBufferFlags()),
                    "renut memexport buffer")) {
    return nullptr;
  }
  if (!had) {
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                             plume::RenderBufferBarrier(buffer.get(),
                                                        plume::RenderBufferAccess::WRITE));
  }
  return buffer.get();
}

// A part of a draw's export window whose seed comes from a pending export's
// window instead of guest memory.
struct SeedPiece {
  u32 slot = 0;
  uint64_t source = 0;  // in the slot's export buffer
  uint64_t dest = 0;    // in the current slot's export buffer
  u32 size = 0;
  // fnxf_xiii (2026-10-27): the source window's page flags (offset in its buffer; UINT64_MAX: none) and the piece's
  // first byte from that window's start: an older slot's piece copies only the pages its kernels wrote.
  uint64_t flags = UINT64_MAX;
  u32 window_rel = 0;
};

// Guest memory lacks what the pending exports will write there, the older
// slots' and this slot's own earlier draws': copies their windows over the
// parts of the draw's windows they overlap, each byte from the newest export
// of it (the copies of one batch are unordered, so no two overlap). Waiting
// for this slot's instead cost a GPU round trip per such draw, the whole
// frame's GPU work so far. A slot's own windows pass through its scratch
// buffer, as a buffer cannot be copied into itself. The copies wait for the
// guest seed through the buffer's unordered-access state in between.
// fnxf_xiii (2026-10-27): the GPU profile's labels of the memexport copies (the slow frame report: a frame's 0.2-1 s
// of "memexport draws" held the copies before each draw too). Their amounts are bytes.
uint32_t ExportSeedLabel() {
  static const uint32_t label = gpu_profile::Label("memexport seeds from guest memory [bytes]");
  return label;
}
uint32_t ExportPendingSeedLabel() {
  static const uint32_t label = gpu_profile::Label("memexport seeds from pending exports [bytes]");
  return label;
}
uint32_t ExportReadbackLabel() {
  static const uint32_t label = gpu_profile::Label("memexport readback copies [bytes]");
  return label;
}
uint32_t ExportGrowLabel() {
  static const uint32_t label = gpu_profile::Label("memexport buffer growth copies [bytes]");
  return label;
}

plume::RenderBuffer *StreamConvertDestLocked(VideoState &s, uint64_t total);  // fnxf_xiii (2026-10-27)

void UnknownKeptPagesLocked(u32 slot, const plume::RenderBuffer *buffer, uint64_t dest, u32 size);  // fnxf_xiii

bool SeedFromPendingExportsLocked(VideoState &s, plume::RenderBuffer *buffer,
                                  const MemExportStream *streams, u32 count, const u32 *offsets) {
  static std::vector<SeedPiece> pieces;
  static std::vector<std::pair<u32, u32>> covered;  // [begin, end), newer exports
  static std::vector<std::pair<u32, u32>> parts;
  pieces.clear();
  for (u32 i = 0; i < count; ++i) {
    const u32 stream_begin = streams[i].base, stream_end = streams[i].base + streams[i].size;
    covered.clear();
    // Newest first: this slot's latest draw, back to the oldest slot's first.
    for (u32 k = kFramesInFlight; k >= 1; --k) {
      const u32 slot = (s.frame + k) % kFramesInFlight;
      if (!s.memexport_buffer[slot])
        continue;
      const auto &pending = g_exports[slot].pending;
      for (auto p = pending.rbegin(); p != pending.rend(); ++p) {
        const u32 begin = std::max(p->base, stream_begin);
        const u32 end = std::min(p->base + p->size, stream_end);
        if (begin >= end)
          continue;
        // What of [begin, end) no newer export covers.
        parts.assign(1, {begin, end});
        for (const auto &[cb, ce] : covered) {
          for (size_t j = 0; j < parts.size();) {
            const auto [pb, pe] = parts[j];
            if (ce <= pb || pe <= cb) {
              ++j;
              continue;
            }
            parts.erase(parts.begin() + j);
            if (pb < cb)
              parts.insert(parts.begin() + j++, {pb, cb});
            if (ce < pe)
              parts.insert(parts.begin() + j++, {ce, pe});
          }
        }
        for (const auto &[pb, pe] : parts) {
          SeedPiece piece{slot, p->offset + (pb - p->base), offsets[i] + uint64_t(pb - stream_begin), pe - pb};
          if (slot != s.frame && DirectExports() && ExportFlagBytes(p->size) && s.stream_convert_pipeline &&
              (piece.size & 3u) == 0 && (piece.source & 3u) == 0 && (piece.dest & 3u) == 0) {
            piece.flags = ExportFlagOffset(p->offset, p->size);
            piece.window_rel = pb - p->base;
          }
          pieces.push_back(piece);
        }
        covered.push_back({begin, end});
      }
    }
  }
  if (pieces.empty())
    return true;
  if (gpu_profile::Timed()) {  // fnxf_xiii (2026-10-27)
    uint64_t bytes = 0;
    for (const SeedPiece &piece : pieces)
      bytes += piece.size;
    gpu_profile::Mark(s, ExportPendingSeedLabel());
    gpu_profile::AddAmount(s, ExportPendingSeedLabel(), bytes);
  }

  // This slot's own pieces go to the scratch buffer first.
  uint64_t own_bytes = 0;
  for (const SeedPiece &piece : pieces) {
    if (piece.slot == s.frame)
      own_bytes += piece.size;
  }
  plume::RenderBuffer *scratch = nullptr;
  uint64_t scratch_offset = 0;
  if (own_bytes) {
    uint64_t &used = s.memexport_scratch_used[s.frame];
    if (!s.memexport_scratch[s.frame] || used + own_bytes > s.memexport_scratch_capacity[s.frame]) {
      // Copies recorded before still use the old one, retired with the slot.
      // fnxf_xiii (2026-10-27): a full scratch is followed by a fresh one of the same size (its pieces start again at
      // 0), larger only for a draw whose own pieces do not fit an empty one. It was grown to hold everything used
      // so far plus the draw's (used + own_bytes), doubling at each refill: 32, 64, 128 then 256 MB within a frame
      // of the game's texture kernels (crash of 2026-10-27, loading a map: the last blocks of deko3d.log), though no
      // draw needed more than its own pieces.
      if (!RefillBuffer(s.memexport_scratch[s.frame], s.memexport_scratch_capacity[s.frame], used, own_bytes,
                        plume::RenderBufferDesc::DefaultBuffer(0), "renut memexport scratch")) {
        return false;
      }
    }
    scratch = s.memexport_scratch[s.frame].get();
    scratch_offset = used;
    used += own_bytes;
  }

  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier barriers[kFramesInFlight + 1];
  u32 barrier_count = 0;
  if (scratch) {
    barriers[0] = plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ);
    barriers[1] = plume::RenderBufferBarrier(scratch, plume::RenderBufferAccess::WRITE);
    cmd->barriers(plume::RenderBarrierStage::COPY, barriers, 2);
    uint64_t at = scratch_offset;
    for (const SeedPiece &piece : pieces) {
      if (piece.slot != s.frame)
        continue;
      cmd->copyBufferRegion(plume::RenderBufferReference(scratch, at),
                            plume::RenderBufferReference(buffer, piece.source), piece.size);
      at += piece.size;
    }
    barriers[barrier_count++] = plume::RenderBufferBarrier(scratch, plume::RenderBufferAccess::READ);
  }
  barriers[barrier_count++] = plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE);
  for (u32 k = 1; k < kFramesInFlight; ++k) {
    const u32 slot = (s.frame + k) % kFramesInFlight;
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::READ);
    }
  }
  cmd->barriers(plume::RenderBarrierStage::COPY, barriers, barrier_count);
  uint64_t at = scratch_offset;
  // fnxf_xiii (2026-10-28): what a piece copies whole (unwritten pages too) is not guest memory's bytes as a kept
  // window records them: those pages are seeded from guest memory the next time (KeptWindow).
  for (const SeedPiece &piece : pieces)
    if (piece.slot == s.frame || piece.flags == UINT64_MAX || !s.stream_convert_pipeline)
      UnknownKeptPagesLocked(s.frame, buffer, piece.dest, piece.size);
  for (const SeedPiece &piece : pieces) {
    if (piece.slot == s.frame) {
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, piece.dest),
                            plume::RenderBufferReference(scratch, at), piece.size);
      at += piece.size;
    } else if (piece.flags == UINT64_MAX) {
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, piece.dest),
                            plume::RenderBufferReference(s.memexport_buffer[piece.slot].get(),
                                                         piece.source),
                            piece.size);
    }
  }
  // fnxf_xiii (2026-10-27): an older slot's pieces, only the pages its kernels wrote (stream_convert_cs's seed mode):
  // the rest of the fresh window keeps guest memory's bytes, which have what the game wrote since that window's seed.
  // Copied whole, a map the game loaded into its texture pool between two compaction passes went back to the pool's old
  // bytes, and the second pass moved those into the minimap's place.
  bool any_flagged = false;
  for (const SeedPiece &piece : pieces)
    any_flagged |= piece.flags != UINT64_MAX;
  if (any_flagged && StreamConvertDestLocked(s, 16)) {
    plume::RenderBufferBarrier compute[kFramesInFlight];
    u32 compute_count = 0;
    compute[compute_count++] = plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE);
    for (u32 k = 1; k < kFramesInFlight; ++k) {
      const u32 slot = (s.frame + k) % kFramesInFlight;
      if (s.memexport_buffer[slot])
        compute[compute_count++] = plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::READ);
    }
    cmd->barriers(plume::RenderBarrierStage::COMPUTE, compute, compute_count);
    cmd->setComputePipelineLayout(s.stream_convert_layout.get());
    cmd->setPipeline(s.stream_convert_pipeline.get());
    cmd->setComputeDescriptorSet(s.stream_convert_set.get(), 0);
    constexpr u32 kMaxWords = 65535u * 64u;
    for (const SeedPiece &piece : pieces) {
      if (piece.flags == UINT64_MAX)
        continue;
      for (u32 word = 0; word < piece.size / 4u; word += kMaxWords) {
        StreamConvertPush push{};
        push.source = piece.slot;
        push.source_offset = u32(piece.source + uint64_t(word) * 4u);
        push.dest_offset = u32(piece.dest + uint64_t(word) * 4u);
        push.vertices = std::min(kMaxWords, piece.size / 4u - word);
        push.host_stride_words = piece.window_rel + word * 4u;
        push.element_count = 0x40000000u;
        push.pad = u32(piece.flags);
        push.elements[0] = s.frame;
        // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): by the older window's write mask, a dword at a time,
        // not its page flags: the rest of a written page keeps the fresh seed (guest memory's bytes now).
        push.elements[1] = ExportMaskOn() ? u32(kExportFlagPrefix) : 0u;
        cmd->setComputePushConstants(0, &push, 0, sizeof(push));
        cmd->dispatch((push.vertices + 63) / 64, 1, 1);
      }
    }
    g_bound.pipeline = nullptr;  // the compute pipeline replaced the graphics one
    static uint32_t logged = 0;
    if (logged++ < 4)
      FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] a fresh window seeded from an older slot's pending one: only the pages its kernels wrote");
  } else if (any_flagged) {
    for (const SeedPiece &piece : pieces)  // no stream_convert_cs set: whole, as before
      if (piece.flags != UINT64_MAX)
        UnknownKeptPagesLocked(s.frame, buffer, piece.dest, piece.size);  // fnxf_xiii (2026-10-28)
    for (const SeedPiece &piece : pieces)
      if (piece.flags != UINT64_MAX)
        cmd->copyBufferRegion(plume::RenderBufferReference(buffer, piece.dest),
                              plume::RenderBufferReference(s.memexport_buffer[piece.slot].get(), piece.source),
                              piece.size);
  }
  // Back to unordered access, as the other export buffers were.
  barrier_count = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::WRITE);
    }
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, barrier_count);
  return true;
}

// fnxf_xiii (2026-10-21): the current slot's export and readback buffers with room for 'needed' bytes, keeping the
// pending windows: a bigger export buffer gets the old one's [0, used) by a GPU copy (recorded after the draws that
// wrote it, so it has their results), and a bigger readback buffer has every pending window copied to it again at the
// end of the recording (FlushExportReadbacksLocked). Before, a draw whose exports did not fit waited for the GPU.
bool GrowSlotExportBuffersLocked(VideoState &s, uint64_t needed) {
  if (DeferredSlotLocked() == int(s.frame))
    FinishDeferredLocked("an export draw of the slot");  // fnxf_xiii (2026-10-28)
  SlotExports &slot = g_exports[s.frame];
  auto &buffer = s.memexport_buffer[s.frame];
  uint64_t &capacity = s.memexport_capacity[s.frame];
  if (!buffer || capacity < needed) {
    // fnxf_xiii (2026-10-27): a buffer that grows goes to the frame's limit (fnxf_xiii_memexport_slot_mb) at once: 32, 64
    // then 128 MB copied the pending windows twice in one frame (100 MB, 16 ms of GPU time).
    const uint64_t limit = ExportSlotLimit();
    const uint64_t grown = buffer ? std::max(GrowBufferSize(capacity, needed), limit) : GrowBufferSize(capacity, needed);
    std::unique_ptr<plume::RenderBuffer> fresh = s.device->createBuffer(
        plume::RenderBufferDesc::DefaultBuffer(grown, ExportBufferFlags()));
    if (!fresh) {
      NoteGrowFailed(grown, "memexport buffer");
      return false;
    }
    fresh->setName("renut memexport buffer");
    plume::RenderCommandList *cmd = s.command_list;
    const uint64_t keep = buffer ? std::min<uint64_t>(slot.used, capacity) : 0;
    if (keep && gpu_profile::Timed()) {  // fnxf_xiii (2026-10-27)
      gpu_profile::Mark(s, ExportGrowLabel());
      gpu_profile::AddAmount(s, ExportGrowLabel(), keep);
    }
    if (keep) {
      plume::RenderBufferBarrier copy[2] = {plume::RenderBufferBarrier(buffer.get(), plume::RenderBufferAccess::READ),
                                            plume::RenderBufferBarrier(fresh.get(), plume::RenderBufferAccess::WRITE)};
      cmd->barriers(plume::RenderBarrierStage::COPY, copy, 2);
      cmd->copyBufferRegion(plume::RenderBufferReference(fresh.get(), 0),
                            plume::RenderBufferReference(buffer.get(), 0), keep);
      g_bound.framebuffer = nullptr;  // the copy ends any render pass
    }
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderBufferBarrier(fresh.get(), plume::RenderBufferAccess::WRITE));
    if (buffer)
      Video::RetireLocked(std::move(buffer));
    buffer = std::move(fresh);
    capacity = grown;
    ForgetKeptWindowsLocked(s.frame);  // fnxf_xiii (2026-10-28)
    static uint32_t logged = 0;
    if (keep && LogLimited(logged, 8))
      RGPU_INFO("[export] buffer grown to {} MB with {} MB of pending windows kept (no wait)", grown >> 20, keep >> 20);
  }
  if (DirectExports())
    return true;  // fnxf_xiii (2026-10-24): the results are read from the export buffer itself
  if (!s.memexport_readback[s.frame] || s.memexport_readback_capacity[s.frame] < needed) {
    if (!EnsureBuffer(s.memexport_readback[s.frame], s.memexport_readback_capacity[s.frame], needed,
                      plume::RenderBufferDesc::ReadbackBuffer(0, REXCVAR_GET(fnxf_xiii_deko3d_cached_readback)
                                                                     ? plume::RenderBufferFlag::HOST_CACHED
                                                                     : plume::RenderBufferFlag::NONE),
                      "renut memexport readback"))
      return false;
    for (PendingExport &p : slot.pending)
      p.dirty = true;  // copied to the new one at the end of the recording
  }
  return true;
}

// Seeds the draw's export windows from guest memory (and the older slots'
// pending exports) and fills its stream table. The windows follow the slot's
// earlier ones in its export buffer, which keeps them for later draws' seeds;
// returns their offsets.
void CardWriterSampleLocked(u32 base, u32 size);  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_card_writers)

// fnxf_xiii (2026-10-28): kept windows. A slot's export buffer still holds a window's results after the slot's
// completion, which put them in guest memory: the next time the slot exports to the same window at the same place in the
// same buffer, its pages guest memory has not changed since are already seeded. Each window was copied whole from guest
// memory by the CPU before, 32 MB, 30-60 ms of the render thread in an area load's bursts (the 2026-10-27 [burst] run).
// A page counts as unchanged when no results were taken back to it since (g_page_version) and the cards show no write
// by the game or a host writer since (CardsUnwrittenSince). One in 64 such pages is compared with guest memory all the
// same: on a difference, the window is seeded whole, and so is every window after it (the log says so).
struct KeptWindow {
  u32 base = 0, size = 0;
  uint64_t offset = 0;
  const plume::RenderBuffer *buffer = nullptr;
  std::vector<u32> version;   // a page's g_page_version when the buffer's page last equalled guest memory's (0: unknown)
  std::vector<u32> interval;  // g_flush_serial then (CardsUnwrittenSince)
  // fnxf_xiii (2026-10-28): seeded in the slot's open recording: its copies have not run on the GPU yet (until the
  // slot's completion), so the buffer cannot be compared with guest memory. A draw that returns after its windows were
  // prepared (and before FinishMemExportLocked) leaves them fresh, and the next draw prepares them again: the 2026-10-28
  // run's "differs" warnings were such a second preparation comparing the buffer before the first one's seed reached it.
  bool unexecuted = false;
};
std::vector<KeptWindow> g_kept_windows[kFramesInFlight];
struct KeptStats {
  uint64_t kept_bytes = 0, copied_bytes = 0, windows = 0, checked = 0, mismatched = 0;
} g_kept_stats;  // this frame's, for the [burst] line
uint64_t g_kept_checks = 0;
bool g_kept_off = false;
std::vector<u8> g_kept_stale;

void ForgetKeptWindowsLocked(u32 slot) {
  g_kept_windows[slot].clear();
}

// [dest, dest + size) of the slot's buffer written with other bytes than guest memory's: those pages unknown.
// fnxf_xiii (2026-10-29): marked kUnknownVersion, not 0, so the completion can tell them from a page never completed
// (version 0): with the write mask the completion copies only the kernel's dwords to guest memory, so such a page's other
// bytes (an older window's seed, from before a game write) stay different from guest memory's and the page must be seeded
// again next time, where a page the results were taken to whole was equal to guest memory (FinishCompletionWindowLocked).
constexpr u32 kUnknownVersion = UINT32_MAX;
void UnknownKeptPagesLocked(u32 slot, const plume::RenderBuffer *buffer, uint64_t dest, u32 size) {
  for (KeptWindow &k : g_kept_windows[slot]) {
    if (k.buffer != buffer || dest + size <= k.offset || k.offset + k.size <= dest)
      continue;
    const uint64_t begin = std::max(dest, k.offset) - k.offset, end = std::min(dest + size, k.offset + k.size) - k.offset;
    for (uint64_t p = begin >> 12; p < k.version.size() && (p << 12) < end; ++p)
      k.version[p] = kUnknownVersion;
  }
}

KeptWindow *FindKeptWindowLocked(u32 slot, const plume::RenderBuffer *buffer, u32 base, u32 size, uint64_t offset) {
  for (KeptWindow &k : g_kept_windows[slot])
    if (k.base == base && k.size == size && k.offset == offset && k.buffer == buffer)
      return &k;
  return nullptr;
}

// A fresh window of the current slot at 'offset' in 'buffer': true when it was kept (its changed pages copied from guest
// memory here; its seed pieces are the GPU's copies, PrepareMemExportLocked); false when it is to be seeded whole, as
// before (and is recorded as kept from then on).
bool SeedKeptWindowLocked(VideoState &s, plume::RenderBuffer *buffer, const MemExportStream &stream, u32 offset) {
  std::vector<KeptWindow> &list = g_kept_windows[s.frame];
  const bool usable = !g_kept_off && DirectExports() && fnxf_xiii::renut_gpu::StoreCardsOn() && !(stream.base & 4095u) &&
                      !(stream.size & 4095u) && stream.size != 0;
  KeptWindow *kw = usable ? FindKeptWindowLocked(s.frame, buffer, stream.base, stream.size, offset) : nullptr;
  const u32 first = stream.base >> 12, pages = stream.size >> 12;
  if (!kw) {
    // Seeded whole: no other record of the slot may claim this part of the buffer any more.
    const uint64_t end = offset + ExportWindowSpan(stream.size);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const KeptWindow &k) {
                                return k.buffer != buffer || (k.offset < end && offset < k.offset + ExportWindowSpan(k.size));
                              }),
               list.end());
    if (usable) {
      KeptWindow k;
      k.base = stream.base;
      k.size = stream.size;
      k.offset = offset;
      k.buffer = buffer;
      k.version.resize(pages);
      k.interval.assign(pages, u32(g_flush_serial));
      for (u32 p = 0; p < pages; ++p)
        k.version[p] = g_page_version[first + p];
      k.unexecuted = true;
      list.push_back(std::move(k));
    }
    return false;
  }
  const u8 *guest = GuestPhysicalPtr<const u8>(stream.base);
  const plume::RenderRange none(0, 0);
  const u8 *cpu = static_cast<const u8 *>(buffer->map(0, &none));
  if (!guest || !cpu)
    return false;
  g_kept_stale.assign(pages, 0);
  bool mismatch = false;
  for (u32 p = 0; p < pages; ++p) {
    const u32 page = first + p;
    bool same = kw->version[p] != 0 && kw->version[p] != kUnknownVersion && kw->version[p] == g_page_version[page] &&
                HarvestPageLocked(page) < kw->interval[p] && (page == 0 || HarvestPageLocked(page - 1) < kw->interval[p]);
    if (same && !kw->unexecuted && (g_kept_checks++ & 63u) == 0) {
      const uint64_t at = offset + uint64_t(p) * 4096u;
      const plume::RenderRange range(at, at + 4096u);
      buffer->map(0, &range);  // dc civac of the page's lines (the GPU wrote them last)
      ++g_kept_stats.checked;
      if (std::memcmp(guest + uint64_t(p) * 4096u, cpu + at, 4096u) != 0) {
        same = false;
        mismatch = true;
        ++g_kept_stats.mismatched;
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("[export] kept window {:08X}: page {:08X} differs from guest memory though nothing recorded a "
                    "write (version {}, now {}; recorded in interval {}, the cards' last write in {}): windows are "
                    "seeded whole from now on",
                    stream.base, page << 12, kw->version[p], g_page_version[page], kw->interval[p],
                    g_game_serial[page]);
      }
    }
    g_kept_stale[p] = !same;
  }
  if (mismatch) {
    g_kept_off = true;
    std::fill(g_kept_stale.begin(), g_kept_stale.end(), u8(1));
  }
  // The changed pages from guest memory, in runs of at most 2 MB (each its own upload allocation).
  constexpr u32 kRun = 512;  // pages
  plume::RenderCommandList *cmd = s.command_list;
  const bool parallel_seeds = ParallelSeeds();  // fnxf_xiii (2026-10-29)
  g_seed_jobs.clear();
  for (u32 p = 0; p < pages;) {
    if (!g_kept_stale[p]) {
      ++p;
      continue;
    }
    u32 q = p + 1;
    while (q < pages && q - p < kRun && g_kept_stale[q])
      ++q;
    const u32 n = (q - p) * 4096u;
    UploadAllocation seed = Video::UploadsLocked().Allocate(n, 256);
    if (!seed) {
      RunSeedJobs(g_seed_jobs.size());  // fnxf_xiii (2026-10-29): the runs recorded so far get their bytes
      g_seed_jobs.clear();
      return false;  // (seeded whole: the full path's allocations fail the same way)
    }
    // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_parallel_seeds): the runs' memcpys together after the loop, on the
    // render thread and the copy helpers (a mismatched window is all of its 32 MB). The ring's lines are cleaned at the
    // submit (UploadRing::Publish), so no clean here (SeedJob::buffer null).
    if (parallel_seeds) {
      g_seed_jobs.push_back({static_cast<u8 *>(seed.cpu), guest + uint64_t(p) * 4096u, n, nullptr, 0});
    } else {
      const auto seed_start = std::chrono::steady_clock::now();
      std::memcpy(seed.cpu, guest + uint64_t(p) * 4096u, n);
      g_burst.seed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - seed_start).count();
      ++g_burst.seeds;
    }
    g_burst.seed_bytes += n;
    g_kept_stats.copied_bytes += n;
    cmd->copyBufferRegion(plume::RenderBufferReference(buffer, offset + uint64_t(p) * 4096u), seed.ref, n);
    p = q;
  }
  if (!g_seed_jobs.empty()) {
    const auto seed_start = std::chrono::steady_clock::now();
    RunSeedJobs(g_seed_jobs.size());
    g_burst.seed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - seed_start).count();
    g_burst.seeds += g_seed_jobs.size();
    g_burst.parallel_seeds += g_seed_jobs.size();
    g_seed_jobs.clear();
  }
  for (u32 p = 0; p < pages; ++p) {
    if (!g_kept_stale[p])
      g_kept_stats.kept_bytes += 4096u;
    kw->version[p] = g_page_version[first + p];
    kw->interval[p] = u32(g_flush_serial);
  }
  kw->unexecuted = true;
  ++g_kept_stats.windows;
  return true;
}

// fnxf_xiii (2026-10-30, ZeroCopyOn): PrepareMemExportLocked when the windows are guest memory itself. A window's
// offset is its physical address; its page flags are the slot's flags buffer's words of its pages, cleared here (a
// window pending already in this slot keeps accumulating them); the CPU's lines of the window are cleaned so the GPU
// reads the game's writes (CleanForGpu: the pages the cards say changed), and the GPU's L2 is invalidated once a draw.
// Nothing is seeded and nothing is kept: guest memory is the window.
bool PrepareZeroCopyExportLocked(VideoState &s, const MemExportStream *streams, u32 count, SharedConstants &sc,
                                 u32 *offsets) {
  if (DeferredSlotLocked() == int(s.frame))
    FinishDeferredLocked("an export draw of the slot");  // the slot's flags are read by it, and cleared below
  plume::RenderBuffer *guest = guest_gpu::Buffer();
  if (!guest || !s.command_list)
    return false;
  std::unique_ptr<plume::RenderBuffer> &flags = g_zero_copy_flags[s.frame];
  if (!flags) {
    // CPU-cached whatever fnxf_xiii_deko3d_direct_exports says: the completion reads it (CompleteZeroCopyLocked).
    flags = s.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(
        kZeroCopyFlagBytes, StorageBufferFlags() | AddressableBufferFlags() | plume::RenderBufferFlag::HOST_CACHED));
    if (!flags) {
      NoteGrowFailed(kZeroCopyFlagBytes, "renut memexport page flags");
      return false;
    }
    flags->setName("renut memexport page flags");
  }
  g_last_export_frame = TextureFrameForLog();
  plume::RenderCommandList *cmd = s.command_list;
  for (u32 i = 0; i < count; ++i) {
    const u32 base = streams[i].base, size = streams[i].size;
    if (!size || uint64_t(base) + size > 0x20000000ull) {
      static uint32_t logged = 0;
      if (LogLimited(logged, 8))
        RGPU_WARN("[export] a window outside physical memory ({:08X}, {:X} bytes): the draw's exports are dropped", base,
                  size);
      return false;
    }
    // The window's cards read now (eight pages a load), so what the game writes from here on is found in a later
    // interval. The CPU's lines of the window are cleaned for the GPU once per submit instead (CleanPendingExportsForGpu-
    // Locked: the kernels run after the submit, so a clean per draw caught fewer writes for more work; v2's clean of
    // every window page at every kernel draw was 319 ms of one frame, fnxf_xiii_002.log).
    HarvestPagesLocked(base >> 12, (base + size - 1) >> 12);
  }
  // A new zero-copy interval: a game write found in it or later came after this draw's record, so a reader of the page
  // gets a record-time copy, not the kernel's output in place (ZeroCopyReadOnGpuLocked, PendingExport::seed_interval).
  ++g_zc_serial;
  bool any_fresh = false;
  for (u32 i = 0; i < count; ++i) {
    const u32 base = streams[i].base, size = streams[i].size;
    offsets[i] = base;
    sc.memexport_streams[i][0] = base;
    sc.memexport_streams[i][1] = size;
    sc.memexport_streams[i][2] = base;  // the store's offset in the buffer: the physical address
    sc.memexport_streams[i][3] = 2u;    // bit 1: the page flags by physical page, in the flags buffer (memExport)
    g_prep_interval[i] = 0;
    if (SlotWindowLocked(s.frame, base, size) != UINT64_MAX)
      continue;  // pending in this slot already: its flags accumulate
    g_prep_interval[i] = g_zc_serial;  // the zero-copy interval (not g_flush_serial: ZeroCopyReadOnGpuLocked)
    const u32 first_page = base >> 12, pages = ((base + size - 1) >> 12) - first_page + 1;
    if (!any_fresh) {
      cmd->barriers(plume::RenderBarrierStage::COPY,
                    plume::RenderBufferBarrier(flags.get(), plume::RenderBufferAccess::WRITE));
      any_fresh = true;
    }
    UploadAllocation zero = Video::UploadsLocked().Allocate(pages * 4u, 256);
    if (!zero)
      return false;
    std::memset(zero.cpu, 0, size_t(pages) * 4u);
    cmd->copyBufferRegion(plume::RenderBufferReference(flags.get(), uint64_t(first_page) * 4u), zero.ref, pages * 4u);
    ++g_burst.zero_copy_windows;
  }
  if (any_fresh) {
    // The kernels' flag writes after the zeroes have landed: a full barrier (the COPY stage: plume_deko3d makes it
    // DkBarrier_Full, which waits for the copy engine too; a primitives barrier orders the 3D engine's work only).
    cmd->barriers(plume::RenderBarrierStage::COPY | plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderBufferBarrier(flags.get(), plume::RenderBufferAccess::WRITE));
    g_bound.framebuffer = nullptr;  // the copies end any render pass
    // The GPU's L2 lines of guest memory dropped once per fresh window: every range the GPU reads in place from here
    // on is a kernel's output, or memory the game wrote before this window's prepare (ZeroCopyReadOnGpuLocked), which
    // this drop makes visible. v2 dropped the L2 at every kernel draw (Deko3DFlushToMemory is a Full barrier, two
    // gpfifo entries: 675 kernel draws a frame filled the channel, fnxf_xiii_002.log).
    if (guest_gpu::InvalidateGpuCachesLocked(s))
      g_bound.framebuffer = nullptr;  // the full barrier ends any render pass
  }
  return true;
}

bool PrepareMemExportLocked(VideoState &s, const MemExportStream *streams, u32 count,
                            SharedConstants &sc, u32 *offsets) {
  const rtw::Scope rtw_mx(rtw::kMxPrepare);  // fnxf_xiii (2026-11-05, [rtwork])
  if (ZeroCopyOn())
    return PrepareZeroCopyExportLocked(s, streams, count, sc, offsets);  // fnxf_xiii (2026-10-30)
  if (DeferredSlotLocked() == int(s.frame))
    FinishDeferredLocked("an export draw of the slot");  // fnxf_xiii (2026-10-28): its buffers and seeds are reused
  const uint64_t used = g_exports[s.frame].used;
  uint64_t total = ExportWindowsEnd(used);  // fnxf_xiii (2026-10-28): the windows start after the flags' MB
  bool reused[kMaxMemExportStreams] = {};
  bool kept[kMaxMemExportStreams] = {};  // fnxf_xiii (2026-10-28): SeedKeptWindowLocked
  for (u32 i = 0; i < count; ++i) {
    const uint64_t window = SlotWindowLocked(s.frame, streams[i].base, streams[i].size);  // fnxf_xiii
    if (window != UINT64_MAX) {
      offsets[i] = u32(window);
      reused[i] = true;
      continue;
    }
    offsets[i] = u32(total);
    // fnxf_xiii (2026-10-28): where the slot's buffer still has the window from its last results (KeptWindow), when
    // that place is free and fits: the windows of a burst do not always come in the same order.
    if (const plume::RenderBuffer *current = s.memexport_buffer[s.frame].get()) {
      for (const KeptWindow &k : g_kept_windows[s.frame]) {
        if (k.buffer == current && k.base == streams[i].base && k.size == streams[i].size && k.offset >= total &&
            k.offset + ExportWindowSpan(k.size) <= s.memexport_capacity[s.frame] && k.offset <= UINT32_MAX) {
          offsets[i] = u32(k.offset);
          total = k.offset;
          break;
        }
      }
    }
    total += ExportWindowSpan(streams[i].size);  // fnxf_xiii (2026-10-27): with its page flags
  }
  // fnxf_xiii (2026-10-29): a window past the write mask's cover (a buffer grown past it for one draw's windows,
  // DrawNeedsExportFlush) would mark nothing: its exports are refused (logged) rather than lost in guest memory.
  const bool mask = ExportMaskOn();
  if (mask && total > kExportMaskCover) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 8))
      RGPU_WARN("[export] a draw's {} window(s) end at {} MB of the slot's buffer, past the write mask's {} MB: its "
                "exports are dropped",
                count, total >> 20, kExportMaskCover >> 20);
    return false;
  }
  g_last_export_frame = TextureFrameForLog();
  if (!GrowSlotExportBuffersLocked(s, total))  // fnxf_xiii (2026-10-21)
    return false;
  plume::RenderBuffer *buffer = MemExportBufferLocked(s, total);
  if (!buffer)
    return false;
  plume::RenderCommandList *cmd = s.command_list;
  static const bool direct_seeds = REXCVAR_GET(fnxf_xiii_deko3d_direct_seeds);
  const bool direct = DirectExports() && direct_seeds;
  const bool parallel_seeds = ParallelSeeds();
  // fnxf_xiii (2026-10-24, fnxf_xiii_deko3d_direct_seeds): the CPU writes the seeds into the export buffer itself (its
  // fresh windows: no command recorded before uses them), cleans them out of its cache, and the GPU's L2 is flushed
  // and invalidated before the draw reads them (Deko3DFlushToMemory), so no line of an earlier use of the memory is
  // read instead.
  u8 *direct_cpu = nullptr;
  if (direct) {
    const plume::RenderRange none(0, 0);
    direct_cpu = static_cast<u8 *>(buffer->map(0, &none));
    if (!direct_cpu)
      return false;
  } else {
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  }
  MemExportStream fresh[kMaxMemExportStreams];
  u32 fresh_offsets[kMaxMemExportStreams];
  u32 fresh_count = 0;
  for (u32 i = 0; i < count; ++i) {
    sc.memexport_streams[i][0] = streams[i].base;
    sc.memexport_streams[i][1] = streams[i].size;
    sc.memexport_streams[i][2] = offsets[i];
    // fnxf_xiii (2026-10-27): its page flags' offset (0: none).
    // fnxf_xiii (2026-10-29): with bit 0 set when the kernels are to mark the write mask too (memExport in
    // shader_common_glsl.h: the flag offsets are multiples of 4).
    const uint64_t flag_bytes = ExportFlagBytes(streams[i].size);
    const uint64_t flags = ExportFlagOffset(offsets[i], streams[i].size);
    sc.memexport_streams[i][3] = flag_bytes ? u32(flags) | (mask ? 1u : 0u) : 0u;
    g_prep_interval[i] = 0;  // fnxf_xiii (2026-10-29): a reused window keeps its seed's interval
    if (reused[i])
      continue;  // fnxf_xiii: the window holds the slot's earlier draws' results already
    g_prep_interval[i] = u32(g_flush_serial);  // fnxf_xiii (2026-10-29): seeded in this interval (FinishMemExportLocked)
    // fnxf_xiii (2026-10-27): a fresh window's page flags cleared (nothing written yet).
    // fnxf_xiii (2026-10-29): and its write mask (fnxf_xiii_memexport_write_mask), 128 bytes a page.
    if (flag_bytes) {
      const uint64_t mask_at = ExportMaskWordOffset(offsets[i]);
      const uint64_t mask_bytes = mask ? ExportMaskBytes(ExportWindowSpan(streams[i].size)) : 0;
      if (direct) {
        std::memset(direct_cpu + flags, 0, size_t(flag_bytes));
        const plume::RenderRange written(flags, flags + flag_bytes);
        buffer->unmap(0, &written);
        if (mask_bytes) {
          std::memset(direct_cpu + mask_at, 0, size_t(mask_bytes));
          const plume::RenderRange written_mask(mask_at, mask_at + mask_bytes);
          buffer->unmap(0, &written_mask);
        }
      } else {
        UploadAllocation zero = Video::UploadsLocked().Allocate(u32(flag_bytes + mask_bytes), 256);
        if (!zero)
          return false;
        std::memset(zero.cpu, 0, size_t(flag_bytes + mask_bytes));
        cmd->copyBufferRegion(plume::RenderBufferReference(buffer, flags), zero.ref, flag_bytes);
        if (mask_bytes)
          cmd->copyBufferRegion(plume::RenderBufferReference(buffer, mask_at),
                                plume::RenderBufferReference(zero.ref.ref, zero.ref.offset + flag_bytes), mask_bytes);
      }
    }
    fresh[fresh_count] = streams[i];
    fresh_offsets[fresh_count++] = offsets[i];
    if (gpu_profile::Timed()) {  // fnxf_xiii (2026-10-27)
      gpu_profile::Mark(s, ExportSeedLabel());
      gpu_profile::AddAmount(s, ExportSeedLabel(), streams[i].size);
    }
    const u8 *guest = GuestPhysicalPtr<const u8>(streams[i].base);
    if (!guest)
      return false;
    if (direct) {
      std::memcpy(direct_cpu + offsets[i], guest, streams[i].size);
      const plume::RenderRange written(offsets[i], uint64_t(offsets[i]) + streams[i].size);
      buffer->unmap(0, &written);  // the lines cleaned to memory
      continue;
    }
    // fnxf_xiii (2026-10-28): a window this slot's buffer still holds from the slot's last results: only its pages guest
    // memory changed since are copied from guest memory (KeptWindow), and its seed for the results' merge is a GPU copy
    // of the window once seeded (after SeedFromPendingExportsLocked, below).
    if (SeedKeptWindowLocked(s, buffer, streams[i], offsets[i])) {
      kept[i] = true;
      CardWriterSampleLocked(streams[i].base, streams[i].size);  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_card_writers)
      continue;
    }
    // fnxf_xiii (2026-10-24): the seed in pieces of 2 MB, each its own upload allocation and copy: a 32 MB window took
    // 32 MB of one upload chunk, which a fragmented heap could not give.
    constexpr u32 kSeedPiece = 2u << 20;
#if defined(PLUME_DEKO3D)
    // fnxf_xiii (2026-10-28): a block the GPU wrote last may be in its L2 still: dropped before the copies read it.
    if (SeedBlocksWrittenByGpuLocked()) {
      plume::Deko3DFlushToMemory(cmd);
      g_bound.framebuffer = nullptr;  // the barrier ends any render pass
    }
#endif
    // fnxf_xiii (2026-10-29): the pieces are copied by the helpers with the render thread
    // (fnxf_xiii_memexport_parallel_seeds): the copies into the window are recorded now, the memcpys run before this
    // returns, and the list is submitted later. With the write mask the blocks are the copies' staging only (no seed
    // piece kept for the completion).
    g_seed_jobs.clear();
    for (u32 at = 0; at < streams[i].size; at += kSeedPiece) {
      const u32 n = std::min(kSeedPiece, streams[i].size - at);
      SeedBlock *seed = SeedBlockLocked(s.frame, false);  // fnxf_xiii (2026-10-28)
      if (!seed) {
        // fnxf_xiii (2026-10-29): the pieces recorded so far get their bytes all the same (their copies are in the
        // list), and the window's kept record forgets every page: the rest of it was never seeded.
        RunSeedJobs(g_seed_jobs.size());
        g_seed_jobs.clear();
        UnknownKeptPagesLocked(s.frame, buffer, offsets[i], u32(ExportWindowSpan(streams[i].size)));
        return false;
      }
      if (parallel_seeds) {
        g_seed_jobs.push_back({seed->cpu, guest + at, n, seed->buffer, seed->offset});
      } else {
        const auto seed_start = std::chrono::steady_clock::now();
        std::memcpy(seed->cpu, guest + at, n);
        const plume::RenderRange written(seed->offset, seed->offset + n);
        seed->buffer->unmap(0, &written);  // cleaned to memory for the GPU's copy
        g_burst.seed_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - seed_start).count();
        g_burst.seed_bytes += n;
        ++g_burst.seeds;
      }
      if (!mask)
        g_export_seeds[s.frame].push_back({offsets[i], at, n, seed->cpu, streams[i].base + at,
                                           u32(g_flush_serial)});  // fnxf_xiii
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, offsets[i] + at),
                            plume::RenderBufferReference(seed->buffer, seed->offset), n);
    }
    if (!g_seed_jobs.empty()) {
      const auto seed_start = std::chrono::steady_clock::now();
      RunSeedJobs(g_seed_jobs.size());
      g_burst.seed_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - seed_start).count();
      g_burst.seed_bytes += streams[i].size;
      g_burst.seeds += u32(g_seed_jobs.size());
      g_burst.parallel_seeds += u32(g_seed_jobs.size());
      g_seed_jobs.clear();
    }
    CardWriterSampleLocked(streams[i].base, streams[i].size);  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_card_writers)
  }
#if defined(PLUME_DEKO3D)
  if (direct && fresh_count) {
    plume::Deko3DFlushToMemory(cmd);
    g_bound.framebuffer = nullptr;  // the barrier ends any render pass
  }
#endif
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  if (fresh_count != 0 && !SeedFromPendingExportsLocked(s, buffer, fresh, fresh_count, fresh_offsets))
    return false;
  // fnxf_xiii (2026-10-28): the kept windows' seeds, as the GPU now has them (guest memory's bytes, and the older
  // slots' pending results over them, as a seed copied by the CPU gets them in CompleteMemExportsLocked).
  // fnxf_xiii (2026-10-29): not with the write mask: the completion needs no seed to compare with (32 MB of GPU copy
  // and of seed blocks a kept window a frame before).
  bool any_kept = false;
  for (u32 i = 0; i < count; ++i)
    any_kept |= kept[i];
  if (any_kept && !mask) {
    cmd->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ));
    constexpr u32 kSeedPiece = 2u << 20;
    for (u32 i = 0; i < count; ++i) {
      if (!kept[i])
        continue;
      for (u32 at = 0; at < streams[i].size; at += kSeedPiece) {
        const u32 n = std::min(kSeedPiece, streams[i].size - at);
        SeedBlock *seed = SeedBlockLocked(s.frame, true);
        if (!seed)
          return false;
        cmd->copyBufferRegion(plume::RenderBufferReference(seed->buffer, seed->offset),
                              plume::RenderBufferReference(buffer, offsets[i] + at), n);
        ExportSeedPiece piece{offsets[i], at, n, seed->cpu, streams[i].base + at, u32(g_flush_serial)};
        piece.gpu = true;
        g_export_seeds[s.frame].push_back(piece);
      }
    }
    // The copies done before the kernels write the windows (Full: the copy engine waited too).
    cmd->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
    g_bound.framebuffer = nullptr;  // the copies end any render pass
  }
  return true;
}

// After the draw: copies its windows to the slot's readback buffer, pending
// until the slot's fence is waited.
// fnxf_xiii (2026-10-27, fnxf_xiii_diag_cpu_reads): see the cvar. The guest memory window's page protection (sdk
// core/guest_memory_switch.h; RexGmAccess: 0 none, 3 read and write). fnxf_xiii (2026-10-30): its own header, not
// declarations of our own with int for RexGmAccess (the LTO link compares the two: -Wlto-type-mismatch).
#if defined(__SWITCH__)
#include "guest_memory_switch.h"
#endif
struct CpuReadPage {
  u32 page = 0;           // physical, 4 KB aligned (0: free)
  uint64_t frame = 0;     // when it was unmapped
  u32 window = 0;         // the export window's base
  std::atomic<u32> read{0};  // set by the fault handler
  uint64_t read_pc = 0;   // its faulting instruction (host)
  u32 read_va = 0;        // the guest address read
};
struct CpuReadDiag {
  bool installed = false;
  CpuReadPage pages[1024];  // fnxf_xiii (2026-11-16): 256 -> 1024 (the resolves' pages too)
  u32 next = 0;           // the ring's next slot
  std::mutex mutex;       // the handler's lookups against the render thread's changes
  // Totals since the last report.
  uint64_t sampled = 0, read = 0, written = 0, untouched = 0, protect_failed = 0;
  std::string events;     // the first read events (render thread, from the handler's records)
  u32 events_logged = 0;
} g_cpu_reads;

bool CpuReadsOn() {
#if defined(__SWITCH__)
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_cpu_reads) || REXCVAR_GET(fnxf_xiii_diag_resolve_reads);
  return on;
#else
  return false;
#endif
}

// The three guest views of a physical page (A and C at its address, E 4 KB lower: PhysicalHeap::GetPhysicalAddress).
u32 CpuReadViews(u32 page, u32 *va) {
  u32 n = 0;
  va[n++] = 0xA0000000u + page;
  va[n++] = 0xC0000000u + page;
  if (page >= 0x1000u)
    va[n++] = 0xE0000000u + page - 0x1000u;
  return n;
}

bool CpuReadProtect(u32 page, bool unmap) {
#if defined(__SWITCH__)
  u32 va[3];
  const u32 n = CpuReadViews(page, va);
  bool ok = true;
  for (u32 i = 0; i < n; ++i)
    ok &= RexGmProtect(detail::GuestVirtualBase() + va[i], 0x1000, unmap ? REX_GM_NONE : REX_GM_WRITE, nullptr);
  return ok;
#else
  (void)page;
  (void)unmap;
  return false;
#endif
}

// Whether any view of the page was mapped again (a write: the SDK's handler restores a written page's view).
bool CpuReadRemapped(u32 page) {
#if defined(__SWITCH__)
  u32 va[3];
  const u32 n = CpuReadViews(page, va);
  for (u32 i = 0; i < n; ++i) {
    size_t len = 0x1000;
    RexGmAccess access = REX_GM_NONE;
    if (RexGmQueryProtect(detail::GuestVirtualBase() + va[i], &len, &access) && access == REX_GM_WRITE)
      return true;
  }
#else
  (void)page;
#endif
  return false;
}

// The fault handler (any guest thread): a read of a sampled page maps it again in every view, records it, and
// retries the instruction. Writes are the SDK's (it maps the written view again); other faults are not ours.
bool CpuReadFault(rex::arch::Exception *ex, void *) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation ||
      ex->access_violation_operation() != rex::arch::Exception::AccessViolationOperation::kRead)
    return false;
  const uint64_t base = reinterpret_cast<uint64_t>(detail::GuestVirtualBase());
  const uint64_t far = ex->fault_address();
  if (far < base + 0xA0000000ull || far >= base + 0x100000000ull)
    return false;
  const u32 va = u32(far - base);
  const u32 physical = va >= 0xE0000000u ? va - 0xE0000000u + 0x1000u : va >= 0xC0000000u ? va - 0xC0000000u
                                                                                          : va - 0xA0000000u;
  const u32 page = physical & ~0xFFFu;
  std::lock_guard lock(g_cpu_reads.mutex);
  for (CpuReadPage &p : g_cpu_reads.pages) {
    if (p.page != page)
      continue;
    if (!CpuReadProtect(page, false))
      return false;
    if (!p.read.load(std::memory_order_relaxed)) {
      p.read_pc = ex->pc();
      p.read_va = va;
      p.read.store(1, std::memory_order_release);
    }
    return true;  // retried: the page is mapped again
  }
  return false;
}

// A sampled page leaves the ring: what happened to it is counted (read, written, or untouched) and its views are
// mapped again.
void CpuReadRetire(CpuReadPage &p) {
  if (!p.page)
    return;
  if (p.read.load(std::memory_order_acquire)) {
    ++g_cpu_reads.read;
    if (g_cpu_reads.events_logged < 40) {
      ++g_cpu_reads.events_logged;
      uint64_t module = 0;
#if defined(__SWITCH__)
      MemoryInfo mi{};
      u32 pi = 0;
      if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, p.read_pc)))
        module = mi.addr;
#endif
      g_cpu_reads.events += fmt::format("\n  page {:08X} (window {:08X}) read at guest {:08X} by host pc {:X} "
                                        "(module {:X} + {:X}), {} frames after its copy",
                                        p.page, p.window, p.read_va, p.read_pc, module, p.read_pc - module,
                                        TextureFrameForLog() - p.frame);
    }
  } else if (CpuReadRemapped(p.page)) {
    ++g_cpu_reads.written;
  } else {
    ++g_cpu_reads.untouched;
  }
  CpuReadProtect(p.page, false);
  p.page = 0;
  p.read.store(0, std::memory_order_relaxed);
}

// After a window's results reached guest memory: one in 32 of its written pages unmapped in the guest's views.
void CpuReadSampleLocked(const PendingExport &p, const u32 *flags) {
  if (!CpuReadsOn() || !flags || !REXCVAR_GET(fnxf_xiii_diag_cpu_reads))
    return;
  if (!g_cpu_reads.installed) {
    g_cpu_reads.installed = true;
    rex::arch::ExceptionHandler::Install(CpuReadFault, nullptr);
    RGPU_INFO("[cpureads] watching a sample of the memexport results' pages for CPU reads (fnxf_xiii_diag_cpu_reads)");
  }
  static u32 counter = 0;
  for (u32 at = 0; at < p.size; at += 4096u) {
    if (!flags[at >> 12] || (counter++ & 31u))
      continue;
    const u32 page = (p.base + at) & ~0xFFFu;
    bool known = false;
    for (const CpuReadPage &q : g_cpu_reads.pages)
      known |= q.page == page;
    if (known)
      continue;
    std::lock_guard lock(g_cpu_reads.mutex);
    CpuReadPage &slot = g_cpu_reads.pages[g_cpu_reads.next];
    g_cpu_reads.next = (g_cpu_reads.next + 1) % 1024u;
    CpuReadRetire(slot);
    if (!CpuReadProtect(page, true)) {
      ++g_cpu_reads.protect_failed;
      CpuReadProtect(page, false);
      continue;
    }
    slot.page = page;
    slot.window = p.base;
    slot.frame = TextureFrameForLog();
    ++g_cpu_reads.sampled;
  }
}

// Every 10 s while on: the totals since the last report. Pages still in the ring are counted when they leave it.
void CpuReadReportLocked() {
  if (!CpuReadsOn() || !g_cpu_reads.installed)
    return;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10))
    return;
  last = now;
  // The pages unmapped more than 300 frames ago leave the ring now, so a quiet stretch reports them too.
  {
    std::lock_guard lock(g_cpu_reads.mutex);
    for (CpuReadPage &p : g_cpu_reads.pages)
      if (p.page && TextureFrameForLog() - p.frame > 300)
        CpuReadRetire(p);
  }
  if (!g_cpu_reads.sampled && !g_cpu_reads.read && !g_cpu_reads.written && !g_cpu_reads.untouched)
    return;
  RGPU_INFO("[cpureads] in 10 s: {} written pages sampled; of those leaving the watch: {} read by the CPU, {} written "
            "by it (not read first), {} untouched; {} could not be unmapped{}",
            g_cpu_reads.sampled, g_cpu_reads.read, g_cpu_reads.written, g_cpu_reads.untouched,
            g_cpu_reads.protect_failed, g_cpu_reads.events);
  g_cpu_reads.sampled = g_cpu_reads.read = g_cpu_reads.written = g_cpu_reads.untouched =
      g_cpu_reads.protect_failed = 0;
  g_cpu_reads.events.clear();
}

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_resolve_reads): before a resolve into dest_va (level 0 of the destination
// texture): its pages unmapped (all of a resolve of up to 8 pages, else one in 8 from the first, at most 32), so a later
// CPU read of them is caught by CpuReadFault and logged by CpuReadReportLocked with the destination as its window.
void ResolveReadWatchLocked(u32 dest_va, const TextureFetch &fetch) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_resolve_reads);
  if (!on || !CpuReadsOn() || !dest_va || !fetch.IsTexture())
    return;
  if (!g_cpu_reads.installed) {
    g_cpu_reads.installed = true;
    rex::arch::ExceptionHandler::Install(CpuReadFault, nullptr);
    RGPU_INFO("[cpureads] watching the resolves' destination pages for CPU reads (fnxf_xiii_diag_resolve_reads)");
  }
  const TextureLevelLayout layout = GetTextureLevelLayout(fetch, 0);
  if (!layout.page)
    return;
  const u32 start = (layout.page << 12) + layout.offset;
  u32 bytes = layout.slice_bytes ? layout.slice_bytes : layout.pitch_texels * layout.height * 4u;
  bytes = std::min<u32>(bytes, 64u << 20);
  const u32 first = start & ~0xFFFu, last = (start + std::max(bytes, 1u) - 1) & ~0xFFFu;
  const u32 count = (last - first) / 0x1000u + 1;
  const u32 step = count <= 8 ? 1 : 8;
  u32 watched = 0;
  for (u32 i = 0; i < count && watched < 32; i += step) {
    const u32 page = first + i * 0x1000u;
    bool known = false;
    for (const CpuReadPage &q : g_cpu_reads.pages)
      known |= q.page == page;
    if (known)
      continue;
    std::lock_guard lock(g_cpu_reads.mutex);
    CpuReadPage &slot = g_cpu_reads.pages[g_cpu_reads.next];
    g_cpu_reads.next = (g_cpu_reads.next + 1) % 1024u;
    CpuReadRetire(slot);
    if (!CpuReadProtect(page, true)) {
      ++g_cpu_reads.protect_failed;
      CpuReadProtect(page, false);
      continue;
    }
    slot.page = page;
    slot.window = dest_va;
    slot.frame = TextureFrameForLog();
    ++g_cpu_reads.sampled;
    ++watched;
  }
}

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): the glare sprites of this frame (render thread).
}  // namespace
std::atomic<u32> g_glare_texture_va{0};  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): see GlareNoteLocked
namespace {
struct GlareDiag {
  uint64_t frame = ~0ull;    // the frame being counted
  u32 count = 0;             // its sprites so far
  std::string alphas;        // their c177.w
  u32 last_count = ~0u;      // the last frame's number
  uint64_t since = 0;        // the frame it became that
  float first_alpha = 0;     // fnxf_xiii (2026-11-16): this frame's first sprite's alpha (0: none)
  float history[16] = {};    // the last frames' first alphas
  float frame_ms[16] = {};   // fnxf_xiii (2026-11-16): and those frames' lengths (render thread, ms)
  std::chrono::steady_clock::time_point last_end{};
  uint64_t frames = 0;
} g_glare;

void GlareFrameEndLocked(uint64_t frame) {
  if (g_glare.frame == ~0ull) {  // the first frame: nothing before it to report
    g_glare.frame = frame;
    return;
  }
  {
    const auto now = std::chrono::steady_clock::now();
    const float ms = g_glare.last_end.time_since_epoch().count()
                         ? float(std::chrono::duration<double, std::milli>(now - g_glare.last_end).count())
                         : 0.0f;
    g_glare.last_end = now;
    g_glare.frame_ms[g_glare.frames % 16] = ms;
  }
  g_glare.history[g_glare.frames++ % 16] = g_glare.count ? g_glare.first_alpha : 0.0f;
  if (g_glare.count != g_glare.last_count) {
    std::string past;  // fnxf_xiii (2026-11-16): the alphas of the 16 frames up to this one (0: no sprite)
    for (uint64_t i = g_glare.frames > 16 ? g_glare.frames - 16 : 0; i < g_glare.frames; ++i)
      past += fmt::format(" {:.4f}@{:.0f}ms", g_glare.history[i % 16], g_glare.frame_ms[i % 16]);
    RGPU_INFO("[glare] frame {}: {} sprites{}{} (was {} for {} frames); last frames' alpha:{}", g_glare.frame,
              g_glare.count, g_glare.alphas.empty() ? "" : ", alpha", g_glare.alphas,
              g_glare.last_count == ~0u ? 0u : g_glare.last_count, g_glare.frame - g_glare.since, past);
    g_glare.last_count = g_glare.count;
    g_glare.since = g_glare.frame;
  }
  g_glare.frame = frame;
  g_glare.count = 0;
  g_glare.alphas.clear();
}

void GlareNoteLocked(const GuestDevice &device, const ShaderRecord *vs, const FloatConstants &vs_constants) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on)
    return;
  const uint64_t frame = TextureFrameForLog();
  if (g_glare.frame != frame)
    GlareFrameEndLocked(frame);
  if (!vs || vs->hash != 0xDA634FFC86F321E1ull)
    return;
  const TextureFetch fetch = DeviceTextureFetch(device.dev(), 0);
  u32 w = 0, h = 0, d = 0;
  if (!fetch.IsTexture())
    return;
  fetch.Size(w, h, d);
  if (u32(fetch.Format()) != 20 || w != 256 || h != 256)
    return;
  u32 word;
  std::memcpy(&word, vs_constants.Register(177) + 12, 4);
  word = std::byteswap(word);
  float alpha;
  std::memcpy(&alpha, &word, 4);
  if (!g_glare.count++)
    g_glare.first_alpha = alpha;
  g_glare.alphas += fmt::format(" {:.4f}", alpha);
  // fnxf_xiii (2026-11-16): [glarevs] every 30th frame, the first glare draw's non-zero VS constants (its real camera,
  // to set against the game's culling camera, [glarecam])
  if (alpha < 1.0f && frame % 30 == 0 && g_glare.count == 1) {
    std::string cs;
    for (u32 r = 0; r < 256; ++r) {
      float f[4];
      bool nz = false;
      for (int k = 0; k < 4; ++k) {
        u32 wv;
        std::memcpy(&wv, vs_constants.Register(r) + 4 * k, 4);
        wv = std::byteswap(wv);
        std::memcpy(&f[k], &wv, 4);
        nz |= wv != 0;
      }
      if (nz)
        cs += fmt::format(" c{}:{:.5g},{:.5g},{:.5g},{:.5g}", r, f[0], f[1], f[2], f[3]);
    }
    RGPU_INFO("[glarevs] frame {}:{}", frame, cs);
  }
  // fnxf_xiii (2026-11-16): the crystal glare's texture header (alpha below 1: not the title's 4.0 sprites), for the
  // game-side hooks to count only its draws (d3d/hooks/draw.cpp).
  if (alpha < 1.0f)
    g_glare_texture_va.store(device.U32(d3d::kDeviceOffsetTextures), std::memory_order_relaxed);
}

// fnxf_xiii (2026-10-27, fnxf_xiii_diag_card_writers): see the cvar. The SDK's fault probe (exception_handler_switch.cpp)
// gives the unmapped pages' faults to CardWriterFault before anything else.
#if defined(__SWITCH__)
extern "C" bool (*g_rex_fault_probe_hook)(uint64_t far, uint64_t pc, bool is_write);
#endif
struct CardWriterPage {
  u32 page = 0;            // physical (0: free)
  u32 writes = 0;          // g_game_writes of the page and the one before when it was unmapped
  u32 writes_before = 0;
  uint64_t frame = 0;
  std::atomic<u32> fault{0};  // 1 read, 2 write (the first access), set by the handler
  uint64_t pc = 0;
  u32 va = 0;
};
struct CardWriterDiag {
  bool installed = false;
  CardWriterPage pages[512];
  u32 next = 0;
  std::mutex mutex;
  uint64_t sampled = 0, reads = 0, writes = 0, unmarked = 0, untouched = 0;
  std::map<uint64_t, std::pair<u32, u32>> by_pc;  // a write's code: (writes, of them unmarked)
  std::map<uint64_t, u32> example_va;
} g_card_writers;

bool CardWritersOn() {
#if defined(__SWITCH__)
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_card_writers);
  return on;
#else
  return false;
#endif
}

bool CardWriterFault(uint64_t far, uint64_t pc, bool is_write) {
  const uint64_t base = reinterpret_cast<uint64_t>(detail::GuestVirtualBase());
  if (far < base + 0xA0000000ull || far >= base + 0x100000000ull)
    return false;
  const u32 va = u32(far - base);
  const u32 physical = va >= 0xE0000000u ? va - 0xE0000000u + 0x1000u : va >= 0xC0000000u ? va - 0xC0000000u
                                                                                          : va - 0xA0000000u;
  const u32 page = physical & ~0xFFFu;
  std::lock_guard lock(g_card_writers.mutex);
  for (CardWriterPage &p : g_card_writers.pages) {
    if (p.page != page)
      continue;
    if (!CpuReadProtect(page, false))
      return false;
    if (!p.fault.load(std::memory_order_relaxed)) {
      p.pc = pc;
      p.va = va;
      p.fault.store(is_write ? 2u : 1u, std::memory_order_release);
    }
    return true;
  }
  return false;
}

// A sampled page leaves the watch: a write is counted by its code, and as unmarked when the cards (taken now) still
// say the page was not written since it was unmapped.
void CardWriterRetire(CardWriterPage &p) {
  if (!p.page)
    return;
  const u32 fault = p.fault.load(std::memory_order_acquire);
  if (fault == 2) {
    ++g_card_writers.writes;
    const u32 n = p.page >> 12;
    HarvestPageLocked(n);
    if (n)
      HarvestPageLocked(n - 1);
    const bool unmarked = g_game_writes[n] == p.writes && (!n || g_game_writes[n - 1] == p.writes_before);
    g_card_writers.unmarked += unmarked;
    uint64_t offset = p.pc;
#if defined(__SWITCH__)
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, p.pc)))
      offset = p.pc - mi.addr;
#endif
    auto &e = g_card_writers.by_pc[offset];
    ++e.first;
    e.second += unmarked;
    g_card_writers.example_va.try_emplace(offset, p.va);
  } else if (fault == 1) {
    ++g_card_writers.reads;
  } else {
    ++g_card_writers.untouched;
  }
  CpuReadProtect(p.page, false);
  p.page = 0;
  p.fault.store(0, std::memory_order_relaxed);
}

// A fresh export window (seeded now): one page in 256 unmapped.
void CardWriterSampleLocked(u32 base, u32 size) {
  if (!CardWritersOn())
    return;
#if defined(__SWITCH__)
  if (!g_card_writers.installed) {
    g_card_writers.installed = true;
    g_rex_fault_probe_hook = CardWriterFault;
    RGPU_INFO("[cardwriters] watching a sample of the memexport windows' pages for writes (fnxf_xiii_diag_card_writers)");
  }
#endif
  std::lock_guard lock(g_card_writers.mutex);
  for (u32 at = 0; at < size; at += 256u * 4096u) {
    const u32 page = (base + at) & ~0xFFFu;
    bool known = false;
    for (const CardWriterPage &q : g_card_writers.pages)
      known |= q.page == page;
    if (known)
      continue;
    CardWriterPage &slot = g_card_writers.pages[g_card_writers.next];
    g_card_writers.next = (g_card_writers.next + 1) % 512u;
    CardWriterRetire(slot);
    const u32 n = page >> 12;  // the cards taken now: what is written from here on counts
    HarvestPageLocked(n);
    if (n)
      HarvestPageLocked(n - 1);
    if (!CpuReadProtect(page, true)) {
      CpuReadProtect(page, false);
      continue;
    }
    slot.page = page;
    slot.writes = g_game_writes[n];
    slot.writes_before = n ? g_game_writes[n - 1] : 0;
    slot.frame = TextureFrameForLog();
    ++g_card_writers.sampled;
  }
}

void CardWriterReportLocked() {
  if (!CardWritersOn() || !g_card_writers.installed)
    return;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10))
    return;
  last = now;
  std::lock_guard lock(g_card_writers.mutex);
  for (CardWriterPage &p : g_card_writers.pages)
    if (p.page && (p.fault.load(std::memory_order_acquire) || TextureFrameForLog() - p.frame > 300))
      CardWriterRetire(p);
  if (!g_card_writers.sampled && !g_card_writers.writes && !g_card_writers.reads && !g_card_writers.untouched)
    return;
  std::string lines;
  for (const auto &[offset, counts] : g_card_writers.by_pc)
    lines += fmt::format("\n  code +{:X}: {} writes, {} of them unmarked (first at guest {:08X})", offset, counts.first,
                         counts.second, g_card_writers.example_va[offset]);
  RGPU_INFO("[cardwriters] in 10 s: {} pages unmapped; of those leaving the watch: {} written ({} without marking "
            "their card), {} read, {} untouched{}",
            g_card_writers.sampled, g_card_writers.writes, g_card_writers.unmarked, g_card_writers.reads,
            g_card_writers.untouched, lines);
  g_card_writers.sampled = g_card_writers.writes = g_card_writers.unmarked = g_card_writers.reads =
      g_card_writers.untouched = 0;
  g_card_writers.by_pc.clear();
  g_card_writers.example_va.clear();
}

// fnxf_xiii (2026-10-27, fnxf_xiii_diag_watch): the watched range, its last hash, the snapshots written.
struct WatchState {
  bool parsed = false;
  u32 begin = 0, end = 0;
  uint64_t hash = 0;
  bool hashed = false;
  u32 snapshots = 0;
  uint64_t vs = 0, ps = 0;  // the export draw being finished (FinishMemExportLocked's log)
} g_watch;

bool WatchOn() {
  if (!g_watch.parsed) {
    g_watch.parsed = true;
    const std::string v = REXCVAR_GET(fnxf_xiii_diag_watch);
    unsigned long a = 0, b = 0;
    if (std::sscanf(v.c_str(), "%lx-%lx", &a, &b) == 2 && b > a) {
      g_watch.begin = u32(a & 0x1FFFFFFFu);
      g_watch.end = g_watch.begin + u32(b - a);
      RGPU_INFO("[watch] guest physical {:08X}-{:08X} watched (fnxf_xiii_diag_watch)", g_watch.begin, g_watch.end);
    }
  }
  return g_watch.end != 0;
}

bool WatchHit(u32 base, u32 size) {
  base &= 0x1FFFFFFFu;
  return WatchOn() && base < g_watch.end && uint64_t(base) + size > g_watch.begin;
}

// The range hashed: a change is logged with 'when' and the bytes written to watch_dump/watch_<n>_f<frame>.bin.
void WatchCheckLocked(const char *when) {
  if (!WatchOn())
    return;
  const u8 *p = GuestPhysicalPtr<const u8>(g_watch.begin);
  if (!p)
    return;
  const size_t n = g_watch.end - g_watch.begin;
  const uint64_t hash = XXH3_64bits(p, n);
  if (g_watch.hashed && hash == g_watch.hash)
    return;
  const bool first = !g_watch.hashed;
  g_watch.hashed = true;
  g_watch.hash = hash;
  // Its first 16 bytes and how many 4 KB pages are all zero, for the log.
  u32 zero_pages = 0;
  for (size_t at = 0; at < n; at += 4096) {
    const size_t len = std::min<size_t>(4096, n - at);
    bool zero = true;
    for (size_t i = 0; i < len && zero; i += 8) {
      uint64_t v;
      std::memcpy(&v, p + at + i, 8);
      zero = v == 0;
    }
    zero_pages += zero;
  }
  const uint64_t frame = TextureFrameForLog();
  RGPU_INFO("[watch] frame {}: {:08X}-{:08X} {} ({}): hash {:016X}, {} of {} pages zero, starts {:02X}{:02X}{:02X}{:02X} "
            "{:02X}{:02X}{:02X}{:02X} {:02X}{:02X}{:02X}{:02X} {:02X}{:02X}{:02X}{:02X}",
            frame, g_watch.begin, g_watch.end, first ? "first seen" : "changed", when, hash, zero_pages,
            (n + 4095) / 4096, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13],
            p[14], p[15]);
  if (g_watch.snapshots < 32) {
    std::error_code ec;
    const std::filesystem::path dir = fnxf_xiii::renut_gpu::NextToExecutable("watch_dump");
    if (g_watch.snapshots == 0)
      std::filesystem::remove_all(dir, ec);  // the previous run's
    std::filesystem::create_directories(dir, ec);
    const std::string name = fmt::format("watch_{:02}_f{}.bin", g_watch.snapshots++, frame);
    if (FILE *f = std::fopen((dir / name).string().c_str(), "wb")) {
      const std::string head = fmt::format("RNWATCH1 begin {:08X} end {:08X} frame {} when {}\n", g_watch.begin,
                                           g_watch.end, frame, when);
      std::fwrite(head.data(), 1, head.size(), f);
      std::fwrite(p, 1, n, f);
      std::fclose(f);
    }
  }
}

void FinishMemExportLocked(VideoState &s, const MemExportStream *streams, u32 count,
                           const u32 *offsets, bool shader) {
  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_watch): the export draws whose windows hold the watched range, the first of each
  // vertex shader in a frame.
  if (WatchOn()) {
    static uint64_t last_frame = ~0ull;
    static std::unordered_set<uint64_t> seen;
    static uint32_t logged = 0;
    if (TextureFrameForLog() != last_frame) {
      last_frame = TextureFrameForLog();
      seen.clear();
    }
    for (u32 i = 0; i < count; ++i) {
      if (WatchHit(streams[i].base, streams[i].size) && seen.insert(g_watch.vs).second && logged++ < 300)
        RGPU_INFO("[watch] frame {}: export draw VS {:016X} PS {:016X}, window {:08X}-{:08X} (slot {} +{:X})",
                  TextureFrameForLog(), g_watch.vs, g_watch.ps, streams[i].base,
                  uint64_t(streams[i].base) + streams[i].size, s.frame, offsets[i]);
    }
  }
  // fnxf_xiii (2026-10-21): the windows are copied to the readback buffer once, at the end of the slot's recording
  // (FlushExportReadbacksLocked): the game's texture kernels write one 32 MB window with many draws a frame, and each
  // draw copied the whole window again.
  SlotExports &slot = g_exports[s.frame];
  for (u32 i = 0; i < count; ++i) {
    bool known = false;
    for (PendingExport &p : slot.pending) {
      if (p.base == streams[i].base && p.size == streams[i].size && p.offset == offsets[i]) {
        p.dirty = true;
        p.shader |= shader;  // fnxf_xiii (2026-10-30)
        known = true;
        break;
      }
    }
    if (known)
      continue;
    // fnxf_xiii (2026-10-29): with the interval it was seeded in (PrepareMemExportLocked, the same draw).
    slot.pending.push_back({streams[i].base, streams[i].size, offsets[i], true, g_prep_interval[i], shader});
    MarkExportGranules(streams[i].base, streams[i].size);
    if (!ZeroCopyOn())  // fnxf_xiii (2026-10-30): the offsets are physical addresses then, in no buffer of the slot
      slot.used = std::max<uint64_t>(slot.used, offsets[i] + ExportWindowSpan(streams[i].size));  // with its flags
  }
  (void)s;
}

// fnxf_xiii (2026-10-21): the current slot's windows written since their last copy, to its readback buffer. Before the
// list is submitted (FinishRecordingLocked): CompleteMemExportsLocked reads them once the slot's fence is waited.
void FlushExportReadbacksLocked(VideoState &s) {
  if (DirectExports())
    return;  // fnxf_xiii (2026-10-24): read from the export buffer itself (CompleteMemExportsLocked)
  SlotExports &slot = g_exports[s.frame];
  plume::RenderBuffer *buffer = s.memexport_buffer[s.frame].get();
  plume::RenderBuffer *readback = s.memexport_readback[s.frame].get();
  if (!buffer || !readback || !s.command_list)
    return;
  bool any = false;
  for (const PendingExport &p : slot.pending)
    any |= p.dirty;
  if (!any)
    return;
  plume::RenderCommandList *cmd = s.command_list;
  if (gpu_profile::Timed())  // fnxf_xiii (2026-10-27)
    gpu_profile::Mark(s, ExportReadbackLabel());
  cmd->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ));
  for (PendingExport &p : slot.pending) {
    if (!p.dirty)
      continue;
    if (gpu_profile::Timed())
      gpu_profile::AddAmount(s, ExportReadbackLabel(), p.size);
    cmd->copyBufferRegion(plume::RenderBufferReference(readback, p.offset),
                          plume::RenderBufferReference(buffer, p.offset), p.size);
    p.dirty = false;
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  g_bound.framebuffer = nullptr;  // the copies end any render pass
}

// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the dword ranges the frame's buffer copies wrote in place of
// the game's copy kernel (CopyKernelAsCopyLocked), into the slot's write mask in one compute pass at the end of the
// recording (stream_convert_cs's mask mode, OR-ed into what the kernels marked). Only the CPU reads the mask, after the
// slot's fence, so the pass may come after every copy and kernel of the frame.
void FlushExportMaskRangesLocked(VideoState &s) {
  std::vector<MaskRange> &ranges = g_mask_ranges[s.frame];
  if (ranges.empty())
    return;
  plume::RenderBuffer *buffer = s.memexport_buffer[s.frame].get();
  if (!buffer || !s.command_list) {
    ranges.clear();
    return;
  }
  plume::RenderCommandList *cmd = s.command_list;
  if (!s.stream_convert_pipeline || !StreamConvertDestLocked(s, 16)) {
    // No compute set (the heap could not give the exported-streams buffer or the set): the mask words of each range
    // are copied whole from an upload of ones instead. The two words at a range's ends then mark up to 31 dwords
    // beside it as written, so the completion copies those too (the seed's bytes, or results: not a loss of the
    // copy's results, which dropping the ranges would be).
    static uint32_t logged = 0;
    if (LogLimited(logged, 4))
      RGPU_WARN("[export] no compute set for the write-mask ranges of {} buffer copies: their mask words are set "
                "whole (up to 31 dwords beside each range copied too)",
                ranges.size());
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
    for (const MaskRange &r : ranges) {
      const uint64_t first = r.offset >> 2, end = (r.offset + r.bytes + 3) >> 2;
      if (end <= first || end > kExportMaskCover / 4)
        continue;
      const uint64_t first_word = first >> 5, words = ((end + 31) >> 5) - first_word;
      UploadAllocation ones = Video::UploadsLocked().Allocate(u32(words * 4), 256);
      if (!ones)
        continue;
      std::memset(ones.cpu, 0xFF, size_t(words * 4));
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, kExportFlagPrefix + first_word * 4), ones.ref,
                            words * 4);
    }
    ranges.clear();
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
    g_bound.framebuffer = nullptr;
    return;
  }
  cmd->barriers(plume::RenderBarrierStage::COMPUTE,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  cmd->setComputePipelineLayout(s.stream_convert_layout.get());
  cmd->setPipeline(s.stream_convert_pipeline.get());
  cmd->setComputeDescriptorSet(s.stream_convert_set.get(), 0);
  u32 dispatches = 0;
  for (const MaskRange &r : ranges) {
    const uint64_t first = r.offset >> 2, end = (r.offset + r.bytes + 3) >> 2;  // dwords of the buffer
    if (end <= first || end > kExportMaskCover / 4)
      continue;
    const uint64_t first_word = first >> 5, words = ((end + 31) >> 5) - first_word;
    StreamConvertPush push{};
    push.source_offset = u32(first);
    push.vertices = u32(end - first);
    push.host_stride_words = u32(first_word);
    push.element_count = 0x20000000u;
    push.pad = u32(kExportFlagPrefix);  // the mask's first byte
    push.elements[0] = s.frame;
    cmd->setComputePushConstants(0, &push, 0, sizeof(push));
    cmd->dispatch(u32((words + 63) / 64), 1, 1);
    ++dispatches;
  }
  static uint32_t logged = 0;
  if (logged++ < 4)
    FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] write mask: {} buffer copies' ranges marked in {} dispatches", ranges.size(),
                        dispatches);
  ranges.clear();
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  g_bound.pipeline = nullptr;      // the compute pipeline replaced the graphics one
  g_bound.framebuffer = nullptr;   // the barriers end any render pass
}

// Brings the pending exports of 'slots' to guest memory, with those of every
// older slot (so a later export of the same memory lands last): waits for the
// older slots' fences, and submits the open list and waits for it when the
// current slot is one of them.
// fnxf_xiii (2026-10-27): the last WaitForExportsLocked's parts (the "Draw waited" line): the GPU's, the results'.
double g_export_wait_gpu_ms = 0.0, g_export_wait_complete_ms = 0.0;
// fnxf_xiii (2026-10-27): and what the results' part did (CompleteMemExportsLocked): windows, pages written, of them
// merged byte by byte (the game wrote them too).
uint64_t g_export_wait_windows = 0, g_export_wait_pages = 0, g_export_wait_merged = 0;

void WaitForExportsLocked(u32 slots) {
  auto &s = State();
  g_export_wait_gpu_ms = g_export_wait_complete_ms = 0.0;
  g_export_wait_windows = g_export_wait_pages = g_export_wait_merged = 0;
  u32 last = 0;  // in age order: 1 is the oldest slot, kFramesInFlight the current one
  for (u32 k = 1; k <= kFramesInFlight; ++k) {
    if (slots & (1u << ((s.frame + k) % kFramesInFlight)))
      last = k;
  }
  for (u32 k = 1; k <= last; ++k) {
    const u32 slot = (s.frame + k) % kFramesInFlight;
    if (DeferredSlotLocked() == int(slot)) {
      // fnxf_xiii (2026-10-28): its fence was waited already: only the copy (not the open list's submit and wait).
      const auto t1 = std::chrono::steady_clock::now();
      FinishDeferredLocked("a draw waited for them");
      g_export_wait_complete_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
      continue;
    }
    if (g_exports[slot].pending.empty())
      continue;
    const auto t0 = std::chrono::steady_clock::now();
    if (slot == s.frame)
      detail::SubmitAndWaitLocked(s);
    else
      detail::WaitSlotLocked(s, slot);
    const auto t1 = std::chrono::steady_clock::now();
    detail::CompleteMemExportsLocked(slot);
    g_export_wait_gpu_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_export_wait_complete_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  }
}

// The slot's stream buffer (stream_convert_cs's destination) with room for 'total' more bytes from
// exported_streams_used on, and the set binding it with the export buffers. A buffer too small is replaced; the
// draws before still read the old one, retired with the slot. fnxf_xiii (2026-10-27): out of
// ConvertExportedStreamsLocked, for UntileFromExportLocked too.
plume::RenderBuffer *StreamConvertDestLocked(VideoState &s, uint64_t total) {
  uint64_t &used = s.exported_streams_used[s.frame];
  // fnxf_xiii (2026-10-30): a full buffer is used again from its start, after a full barrier (the GPU runs a slot's
  // lists in order: every dispatch, draw and copy that read the regions before is done once the barrier passes), not
  // replaced by a fresh one (RefillBuffer: a 16 MB buffer made, 5 ms, for every 16 MB of untiled texture levels; an
  // area load's frame with three 4096x4096 levels made two and spent 70 ms on buffers, and with the levels untiled from
  // guest memory on the GPU (textures.cpp PlanExportUntile, fnxf_xiii_memexport_zero_copy) it would make thirty).
  if (s.exported_streams[s.frame] && used + total > s.exported_streams_capacity[s.frame] &&
      total <= s.exported_streams_capacity[s.frame] && s.command_list) {
    s.command_list->barriers(plume::RenderBarrierStage::COPY,
                             plume::RenderBufferBarrier(s.exported_streams[s.frame].get(),
                                                        plume::RenderBufferAccess::WRITE));
    g_bound.framebuffer = nullptr;  // the barrier ends any render pass
    used = 0;
  }
  // fnxf_xiii (2026-10-27): RefillBuffer (grows the buffer when 'total' alone does not fit).
  // fnxf_xiii (2026-11-05): made at 32 MB at least (it was made at what the first use asked, 8 MB, and grown to 16 MB in
  // the area entry's burst: two blocks made on the render thread there).
  constexpr uint64_t kStreamBufferMin = 32ull << 20;
  const uint64_t ask = s.exported_streams[s.frame] ? total : std::max(total, kStreamBufferMin);
  if (!RefillBuffer(s.exported_streams[s.frame], s.exported_streams_capacity[s.frame], used, ask,
                    plume::RenderBufferDesc::DefaultBuffer(0, StorageBufferFlags() | plume::RenderBufferFlag::VERTEX),
                    "renut exported streams")) {
    return nullptr;
  }
  plume::RenderBuffer *dest = s.exported_streams[s.frame].get();
  const plume::RenderBuffer *buffers[kStreamConvertBuffers];
  for (u32 slot = 0; slot < kFramesInFlight; ++slot)
    buffers[slot] = ExportBuffer(s, slot) ? ExportBuffer(s, slot) : dest;  // fnxf_xiii (2026-10-30): ExportBuffer
  buffers[kFramesInFlight] = dest;
  if (!s.stream_convert_set ||
      std::memcmp(buffers, s.stream_convert_set_buffers, sizeof(buffers)) != 0) {
    // Dispatches recorded before still use the old set.
    Video::RetireLocked(std::move(s.stream_convert_set));
    s.stream_convert_set = detail::CreateStreamConvertSet(s);
    if (!s.stream_convert_set)
      return nullptr;
    for (u32 i = 0; i < kStreamConvertBuffers; ++i)
      s.stream_convert_set->setBuffer(i, buffers[i]);
    std::memcpy(s.stream_convert_set_buffers, buffers, sizeof(buffers));
  }
  return dest;
}

// Builds the host vertices of the draw's streams whose newest data is in a
// pending export's window (FindExportedStream; DrawNeedsExportFlush did not
// wait for those) with stream_convert_cs, into the slot's stream buffer.
// Records dispatches: before the framebuffer is bound.
bool ConvertExportedStreamsLocked(VideoState &s, const GuestDevice &device, const DrawCall &call,
                                  const ShaderRecord &vs, const u32 *shader_strides,
                                  const VertexDeclarationRecord &declaration, const Geometry &g,
                                  GpuStreams &out) {
  if (!s.stream_convert_pipeline || !AnyPendingExports() || REXCVAR_GET(fnxf_xiii_renut_diag_no_stream_convert))
    return true;
  struct Job {
    u32 stream;
    ExportedStream src;
    u32 first, count;
  };
  // fnxf_xiii (2026-10-19, render-thread proposal 4): storage only (ExportedStream's member initialisers zeroed all 16
  // jobs, about 1.8 KB, at every draw while exports were pending); a job is made for each stream that has one.
  alignas(Job) unsigned char job_storage[sizeof(Job) * kMaxVertexStreams];
  Job *jobs = reinterpret_cast<Job *>(job_storage);
  u32 job_count = 0;
  uint64_t total = 0;
  for (u32 stream = 0; stream < kMaxVertexStreams; ++stream) {
    if (!declaration.layout.streams[stream].used)
      continue;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
    const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
    if (!PendingExportSlots(base, size))
      continue;
    Job &job = *new (&jobs[job_count]) Job{stream};  // made in place, kept by the ++job_count below
    if (!FindExportedStream(device, call, vs, shader_strides, declaration, stream, job.src))
      continue;
    // The vertices BindStreams would convert.
    job.first = g.first_vertex;
    if (job.first >= job.src.vertex_count)
      continue;
    job.count = std::min(g.vertex_range, job.src.vertex_count - job.first);
    if (!job.count)
      continue;
    // fnxf_xiii (2026-10-30, ZeroCopyOn): only a kernel's output is read in place; anything else BindStreams converts
    // from guest memory now, as the game's fence promises (ZeroCopyRange).
    if (ZeroCopyOn() && !ZeroCopyReadOnGpuLocked(base + job.first * job.src.stride, job.count * job.src.stride))
      continue;
    total += (uint64_t(job.count) * job.src.host_stride + 255u) & ~uint64_t(255);
    // fnxf_xiii (2026-10-27, fnxf_xiii_renut_diag_stream_dump_vs): the stream's pending-export source and conversion, for
    // The game's copy kernel (whether its draws are a plain copy from one export window to another).
    {
      static const uint64_t want = [] {
        const std::string v = REXCVAR_GET(fnxf_xiii_renut_diag_stream_dump_vs);
        return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
      }();
      static uint32_t logged = 0;
      if (want && vs.hash == want && logged++ < 24) {
        std::string el;
        for (u32 e = 0; e < job.src.element_count && e < kStreamConvertMaxElements; ++e)
          el += fmt::format(" {:08X}/{:08X}", job.src.elements[2 * e], job.src.elements[2 * e + 1]);
        FNXF_XIII_BURST_LOG(RGPU_INFO, "[copykernel] stream {} guest {:08X} size {:X}: window of slot {} at +{:X}, stride {} host stride {}, "
                  "{} vertices there, draw first {} count {}; elements (word, conversion):{}",
                  stream, base, size, job.src.slot, job.src.offset, job.src.stride, job.src.host_stride,
                  job.src.vertex_count, job.first, job.count, el);
      }
    }
    ++job_count;
  }
  if (!job_count)
    return true;

  uint64_t &used = s.exported_streams_used[s.frame];
  plume::RenderBuffer *dest = StreamConvertDestLocked(s, total);  // fnxf_xiii (2026-10-27): shared with the textures
  if (!dest)
    return false;

  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier barriers[kFramesInFlight + 1];
  u32 barrier_count = 0;
  barriers[barrier_count++] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE);
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (plume::RenderBuffer *window = ExportBuffer(s, slot)) {  // fnxf_xiii (2026-10-30): ExportBuffer
      barriers[barrier_count++] = plume::RenderBufferBarrier(window, plume::RenderBufferAccess::WRITE);
      if (ZeroCopyOn())
        break;
    }
  }
  // fnxf_xiii (2026-10-30, ZeroCopyOn): no L2 invalidation (UploadVertexFetchData says why).
  static const uint32_t kProfileLabel = gpu_profile::Label("exported stream conversion");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE, barriers, barrier_count);
  cmd->setComputePipelineLayout(s.stream_convert_layout.get());
  cmd->setPipeline(s.stream_convert_pipeline.get());
  cmd->setComputeDescriptorSet(s.stream_convert_set.get(), 0);
  for (u32 i = 0; i < job_count; ++i) {
    const Job &job = jobs[i];
    const u32 bytes = job.count * job.src.host_stride;
    StreamConvertPush push{};
    push.source = job.src.slot;
    push.source_offset = u32(job.src.offset + uint64_t(job.first) * job.src.stride);
    push.dest_offset = u32(used);
    push.vertices = job.count;
    push.guest_stride_words = job.src.stride / 4;
    push.host_stride_words = job.src.host_stride / 4;
    push.element_count = job.src.element_count;
    std::memcpy(push.elements, job.src.elements, sizeof(push.elements));
    cmd->setComputePushConstants(0, &push, 0, sizeof(push));  // fnxf_xiii: the size (the record worker copies it)
    cmd->dispatch((job.count * job.src.element_count + 63) / 64, 1, 1);
    out.ref[job.stream] = plume::RenderBufferReference(dest, used);
    out.bytes[job.stream] = bytes;
    used += (uint64_t(bytes) + 255u) & ~uint64_t(255);
  }
  barriers[0] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, barrier_count);
  // The compute pipeline replaced the graphics one (D3D12), and the barriers
  // end any render pass.
  g_bound.pipeline = nullptr;
  g_bound.framebuffer = nullptr;
  return true;
}

// Stands in for a null vertex declaration: the vertex shader fetches its data
// by address (vfetchMemory), and any input it declares reads the zero stream.
VertexDeclarationRecord &NoDeclaration() {
  static VertexDeclarationRecord record = [] {
    VertexDeclarationRecord r;
    r.layout = BuildHostVertexLayout(nullptr, 0);
    r.layout_hash = 0x4E4F44454300ull;  // any value no FNV hash of a real layout hits
    return r;
  }();
  return record;
}

} // namespace

namespace detail {

void FinishRecordingLocked(VideoState &s) {
  FlushVertexFillsLocked();
  FlushTextureFillsLocked();
  FlushExportReadbacksLocked(s);  // fnxf_xiii (2026-10-21)
  FlushExportMaskRangesLocked(s);  // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask)
  CleanPendingExportsForGpuLocked();  // fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy): before the submit
}

} // namespace detail

// fnxf_xiii (2026-11-05, seen15): the kick's queued vertex conversions and fetch copies, split over the render thread and
// the two copy helpers (textures.h RunOnCopyHelpers) when they come to 512 KB or more. Run d484a129 ([rtwork]): the
// kicks' vertex fills were 14-25 ms of the render thread's 42-54 ms in the burst frames, the game waiting for it in
// its D3D calls. The pieces: a stream fill cut at whole vertices (ConvertVertices converts each vertex on its own, so
// a range of vertices converts as it does inside the whole), about 64 KB of host bytes each; a fetch copy cut at 64 KB.
// Every fill writes its own upload allocation (a fresh Allocate per queued fill: a reused one is not queued again) and
// every copy its own part of its draw's chunk, so no two pieces write the same bytes; the stream fills all end before
// the copies start, as they did one after the other. Under 512 KB: on the render thread
// alone, in order, as before.
static void ConvertQueuedFills() {
  constexpr size_t kPieceBytes = 64u << 10, kSplitBytes = 512u << 10;
  size_t stream_bytes = 0, copy_bytes = 0;
  for (const PendingStreamFill &f : g_stream_fills)
    stream_bytes += size_t(f.count) * std::max<u32>(f.stream.host_stride, f.stride);
  for (const PendingCopy &c : g_copy_fills)
    copy_bytes += c.size;
  rtw::g_fill_stream_bytes += stream_bytes;  // fnxf_xiii (2026-11-05, seen18, [rtwork])
  rtw::g_fill_copy_bytes += copy_bytes;
  struct Piece {
    u32 index, first, count;
  };
  static std::vector<Piece> pieces;  // render thread only
  if (stream_bytes >= kSplitBytes) {
    pieces.clear();
    for (u32 i = 0; i < u32(g_stream_fills.size()); ++i) {
      const PendingStreamFill &f = g_stream_fills[i];
      const u32 stride = std::max<u32>(1, std::max<u32>(f.stream.host_stride, f.stride));
      const u32 per = f.stream.host_stride ? std::max<u32>(1, u32(kPieceBytes / stride)) : std::max<u32>(1, f.count);
      for (u32 first = 0; first < f.count; first += per)
        pieces.push_back({i, first, std::min(per, f.count - first)});
    }
    std::atomic<u32> next{0};
    RunOnCopyHelpers([&](u32) {
      for (u32 k; (k = next.fetch_add(1, std::memory_order_relaxed)) < pieces.size();) {
        const Piece &p = pieces[k];
        const PendingStreamFill &f = g_stream_fills[p.index];
        ConvertVertices(f.stream, f.guest + size_t(p.first) * f.stride, f.stride, p.count, f.endian,
                        f.host + size_t(p.first) * f.stream.host_stride);
      }
    });
  } else {
    for (const PendingStreamFill &f : g_stream_fills)
      ConvertVertices(f.stream, f.guest, f.stride, f.count, f.endian, f.host);
  }
  if (copy_bytes >= kSplitBytes) {
    pieces.clear();
    for (u32 i = 0; i < u32(g_copy_fills.size()); ++i)
      for (u32 off = 0; off < g_copy_fills[i].size; off += u32(kPieceBytes))
        pieces.push_back({i, off, std::min<u32>(u32(kPieceBytes), g_copy_fills[i].size - off)});
    std::atomic<u32> next{0};
    RunOnCopyHelpers([&](u32) {
      for (u32 k; (k = next.fetch_add(1, std::memory_order_relaxed)) < pieces.size();) {
        const Piece &p = pieces[k];
        const PendingCopy &c = g_copy_fills[p.index];
        std::memcpy(c.dst + p.first, c.src + p.first, p.count);
      }
    });
  } else {
    for (const PendingCopy &c : g_copy_fills)
      std::memcpy(c.dst, c.src, c.size);
  }
}

void FlushVertexFillsLocked() {
  ++g_flush_serial;  // fnxf_xiii: fnxf_xiii_renut_exact_streams' intervals
  fnxf_xiii::renut_gpu::CountKick();
  ConvertQueuedFills();  // fnxf_xiii (2026-11-05, seen15): over the copy helpers when there is much of it
  for (const PendingStreamFill &f : g_stream_fills) {
    if (f.cacheable)
      TrackConvertedStream(f);
    else if (f.stride)
      RedeemQuietStream(f);  // fnxf_xiii (2026-11-06, seen19)
    if (!f.cacheable)
      rtw::g_fill_untracked_bytes += size_t(f.count) * f.stride;  // fnxf_xiii (2026-11-06, seen20, [rtwork])
  }
  g_stream_fills.clear();
  ++g_queued_generation;  // fnxf_xiii (2026-10-19): the table emptied
  g_copy_fills.clear();
  ++g_fetch_copy_generation;
}

void SetTilesLocked(const ClearRect *rects, u32 count) {
  g_tiles.assign(rects, rects + (rects ? count : 0));
}

u32 SetReplayPredicationLocked(u32 mask) {
  const u32 previous = g_replay_predication;
  g_replay_predication = mask;
  return previous;
}

// fnxf_xiii (fnxf_xiii_gpu_depth_only, package 28; Unleashed / Marathon's depth-only pipelines): a draw that writes no
// colour, into a depth target, without the alpha test or alpha to coverage, keeps nothing of its pixel shader unless the
// shader kills pixels, writes depth or exports to memory (ShaderCacheEntry::shaderFlags, which the translator checks
// against the shader's own text, and memExportConstants): it is drawn without it. Depth and stencil come from the
// rasterizer either way (no SV_Depth), so the targets get the same values; the shader's constants and textures are not
// set up, and its draws share one pipeline per vertex shader and state, whose vertex outputs NVK drops (linked stages).
bool DrawsWithoutPixelShader(const GuestDevice &device, const Targets &t, const ShaderRecord &ps) {
  if (!t.depth || !ps.entry || !fnxf_xiii::renut_gpu::DepthOnlyOn())
    return false;
  // fnxf_xiii (2026-10-20, from sa2-nx): the colour mask first (SA2: every draw writes colour; N&B's shadow draws do not: one word instead of the
  // shader's export constants either way); all the checks only read, so their order does not change the answer.
  const d3d::Device &dev = device.dev();
  const u32 color_mask = u32(dev.m_ValuesPacket.ColorMask) & 0xFFFF;  // MakeKey's; 4 bits per target
  if (t.color_count && (color_mask & ((1u << (4 * t.color_count)) - 1u)))
    return false;
  const ShaderCacheEntry &e = *ps.entry;
  if (e.shaderFlags & (kShaderFlagPixelKill | kShaderFlagPixelDepth))
    return false;
  for (const u32 m : e.memExportConstants) {
    if (m)
      return false;
  }
  const u32 color_control = dev.m_ControlPacket.ColorControl;
  if ((color_control & 8) && (color_control & 7) != 7)  // the alpha test, as sc.alpha_test_func
    return false;
  return ((color_control >> 4) & 1) == 0;  // alpha to coverage, as MakeKey's
}

// fnxf_xiii (2026-10-18, matching RenderDoc captures of Ryujinx to the game's shaders): fnxf_xiii_renut_diag_draw_log_s
// seconds after the first draw, every draw of the next two frames: its shaders, counts, target and state, in order.
extern bool g_output_phase;  // graphics_options.cpp
bool g_load_log_active = false;  // fnxf_xiii (2026-11-08): fnxf_xiii_diag_load_draw_log's window (surfaces.cpp logs builds)
void DrawLog(const DrawCall &call, const Geometry &geometry, const ShaderRecord *vs, const ShaderRecord *ps,
             const Targets &targets, const PipelineKey &key, const SharedConstants &sc, u32 export_count,
             const GuestDevice &device) {
  static const int32_t at = REXCVAR_GET(fnxf_xiii_renut_diag_draw_log_s);
  static const int32_t load_frames = REXCVAR_GET(fnxf_xiii_diag_load_draw_log);
  if (at <= 0 && load_frames <= 0)
    return;
  static const auto start = std::chrono::steady_clock::now();
  static uint64_t first_frame = 0;
  static uint32_t ordinal = 0;
  static uint64_t ordinal_frame = 0;
  const uint64_t frame = TextureFrameLocked();
  // fnxf_xiii (2026-11-08, fnxf_xiii_diag_load_draw_log): every frame one [frames] line: its target runs in order (colour
  // VA:host size/code(d: dynamic) + depth, draws), FSR before the post-processing, the output phase; every draw is kept
  // as a line, and a frame whose run of targets differs from the frame before's (with FSR run in it) is written draw by
  // draw, with the next ones up to this many frames (12 times at most).
  if (load_frames > 0) {
    g_load_log_active = true;
    static uint64_t sig_frame = ~uint64_t(0), flush_until = 0;
    static std::string sig, last_sig, runs;
    std::vector<std::string> &lines = g_load_lines;
    static u32 run_rt = 0, run_z = 0, run_draws = 0, draw_no = 0;
    static uint32_t windows = 0;
    static bool early_seen = false, phase_seen = false;
    auto close_run = [&] {
      if (run_draws)
        runs += fmt::format(" {:08X}{}#{}", run_rt, run_z ? fmt::format("/z{:08X}", run_z) : std::string(), run_draws);
      run_draws = 0;
    };
    if (frame != sig_frame) {
      if (sig_frame != ~uint64_t(0)) {
        close_run();
        RGPU_INFO("[frames] {} early {} (reason {}, {} resolves noted) phase {}:{}", sig_frame, early_seen,
                  g_early_reason, g_early_noted, phase_seen, runs);
        g_early_reason = 0;
        g_early_noted = 0;
        if (sig != last_sig && early_seen && windows < 12) {
          ++windows;
          flush_until = sig_frame + uint64_t(load_frames);
          RGPU_INFO("[loadlog] frame {}: the targets' run changed: it and the next draw by draw", sig_frame);
        }
        if (sig_frame < flush_until)
          for (const std::string &l : lines)
            RGPU_INFO("[drawlog] {}", l);
        last_sig = sig;
      }
      sig_frame = frame;
      early_seen = phase_seen = false;
      sig.clear();
      runs.clear();
      lines.clear();
      run_rt = run_z = 0;
      draw_no = 0;
    }
    const HostSurface *c = targets.color_count ? targets.colors[0] : nullptr;
    const HostSurface *zs = targets.depth;
    const u32 rt = device.U32(d3d::kDeviceOffsetRenderTargets), z = device.U32(d3d::kDeviceOffsetDepthStencil);
    if (rt != run_rt || z != run_z) {
      close_run();
      run_rt = rt;
      run_z = z;
      runs += fmt::format(" [{}x{}c{}{}]", c ? c->scale.Size(c->width) : 0, c ? c->scale.Size(c->height) : 0,
                          c ? c->scale.code : 0, c && c->scale.drs ? "d" : "");
      sig += fmt::format("{:08X}{:08X}{}", rt, z, c ? c->size_word : 0);
    }
    ++run_draws;
    early_seen |= g_early_fsr.ran_frame == g_motion_target.frame;
    phase_seen |= g_output_phase;
    lines.push_back(fmt::format(
        "f{} #{} vs {:016X} ps {:016X} n {} | rt {:08X} {}x{} hdr {}x{} c{}{} | z {:08X} {}x{} c{}{} | t {:08X} {:08X} "
        "{:08X} {:08X} | dc {:08X} bl {:08X} m {:X} | ph {} early {} drs {:.3f}",
        frame, draw_no++, vs ? vs->hash : 0, ps ? ps->hash : 0, call.count, rt, c ? c->scale.Size(c->width) : 0,
        c ? c->scale.Size(c->height) : 0, c ? d3d::SurfaceWidth(c->size_word) : 0,
        c ? d3d::SurfaceHeight(c->size_word) : 0, c ? c->scale.code : 0, c && c->scale.drs ? "d" : "", z,
        zs ? zs->scale.Size(zs->width) : 0, zs ? zs->scale.Size(zs->height) : 0, zs ? zs->scale.code : 0,
        zs && zs->scale.drs ? "d" : "", sc.texture_indices[0][0], sc.texture_indices[0][1], sc.texture_indices[0][2],
        sc.texture_indices[0][3], key.depth_control, key.blend_control[0], key.color_mask, g_output_phase,
        g_early_fsr.ran_frame == g_motion_target.frame, g_drs_factor));
    if (at <= 0)
      return;
  }
  const bool in_load = false;
  if (!in_load) {
    if (at <= 0)
      return;
    if (!first_frame) {
      if (std::chrono::steady_clock::now() - start < std::chrono::seconds(at))
        return;
      first_frame = frame + 1;  // from the next whole frame
    }
    if (frame < first_frame || frame > first_frame + 1)
      return;
  }
  if (frame != ordinal_frame) {
    ordinal_frame = frame;
    ordinal = 0;
  }
  const HostSurface *c0 = targets.color_count ? targets.colors[0] : nullptr;
  RGPU_INFO("[drawlog] frame {} draw {}: vs {:016X} ps {:016X} prim {} count {} -> {} {} inst {}{} target {}x{} fmt {} "
            "x{} depth {} dc {:08X} blend {:08X} mask {:X} a2c {} vfetch {} bytes, exports {}",
            frame, ordinal++, vs ? vs->hash : 0, ps ? ps->hash : 0, call.primitive, call.count,
            geometry.indexed ? "indices" : "vertices", geometry.indexed ? geometry.index_count : geometry.vertex_count,
            geometry.instance_count, call.tessellation ? " tess" : "", c0 ? c0->width : 0, c0 ? c0->height : 0,
            c0 ? int(c0->format) : -1, targets.color_count, targets.depth ? int(targets.depth->format) : -1,
            key.depth_control, key.blend_control[0], key.color_mask, key.alpha_to_coverage, sc.vertex_fetch_bytes,
            export_count);
  // fnxf_xiii (2026-11-08, dynamic resolution): the 2D descriptor words of fetches 0-3, the targets' scale, the step.
  RGPU_INFO("[drawlog]   t2d {:08X} {:08X} {:08X} {:08X} scale {} drs {} resolution_scale {} drs_step {}",
            sc.texture_indices[0][0], sc.texture_indices[0][1], sc.texture_indices[0][2], sc.texture_indices[0][3],
            targets.scale.code, targets.scale.drs, sc.resolution_scale, sc.drs_step);
  if (in_load) {
    const HostSurface *z = targets.depth;
    RGPU_INFO("[drawlog]   rt {:08X} {}x{} hdr {}x{} code {} drs {} | depth {:08X} {}x{} hdr {}x{} code {} drs {} | "
              "phase {} early {} motion {} cleared {} drs_factor {}",
              device.U32(d3d::kDeviceOffsetRenderTargets), c0 ? c0->width : 0, c0 ? c0->height : 0,
              c0 ? d3d::SurfaceWidth(c0->size_word) : 0, c0 ? d3d::SurfaceHeight(c0->size_word) : 0,
              c0 ? c0->scale.code : 0, c0 ? c0->scale.drs : false, device.U32(d3d::kDeviceOffsetDepthStencil),
              z ? z->width : 0, z ? z->height : 0, z ? d3d::SurfaceWidth(z->size_word) : 0,
              z ? d3d::SurfaceHeight(z->size_word) : 0, z ? z->scale.code : 0, z ? z->scale.drs : false,
              g_output_phase, g_early_fsr.ran_frame == g_motion_target.frame, g_motion_target.frame,
              g_motion_target.cleared_frame, g_drs_factor);
  }
}

#if defined(PLUME_DEKO3D)
// ---------------------------------------------------------------------------
// fnxf_xiii (2026-10-20, fnxf_xiii_deko3d_skin_once): the fur skinned once a frame
// ---------------------------------------------------------------------------
// The fur family draws each body part once a shell (12 shells and more a frame, shell by shell), and each draw's vertex
// shader skins the same vertices again: 12 bone rows loaded and blended a vertex, about half its work. With the key on
// the listed vertex shaders (tools/skinonce_shaders.txt) have constant 913 (tools/deko3d_glsl_opt.py skinonce): the
// draw's shared word v[84].z (pad_1352[0]) 1 skins and also writes each vertex's three skinned rows to the storage
// buffer at binding 13 (gl_VertexID * 3), 2 reads them there instead. A part's first draw of the frame writes; a later
// draw reads only when everything the skinning reads is the same: the vertex shader, the bound vertex data (GPU
// addresses: the upload ring never hands out a range twice in a frame), the index buffer and draw range, the fetch
// window (the bone data's GPU address) and the vertex-fetch part of SharedConstants (kSkinShared: bone offsets, fetch
// slots, bools). The skinning reads nothing else: the pass refuses shaders whose skinning reads the float constants,
// textures, the instance or another shared word, so draws with other pixel shaders on the same mesh share rows too.
// The rows are stored and loaded as the bits the writer computed, which are the bits the reader would have computed.
// A barrier (vertex work done, caches invalidated) comes before the first read after writes.
// The shared constants the skinning may read (the pass refuses any other word): the booleans (v[32-33]), the fetch
// data, its slots and size (v[35-60]), tessellation and the bound fetches (v[65-81]) and flt_max (v[83].w). The rest
// (the pixel shader's textures, the viewport) may differ between draws that share rows.
constexpr struct {
  u32 offset, size;
} kSkinShared[] = {{512, 32}, {560, 416}, {1040, 272}, {1340, 4}};
constexpr u32 kSkinSharedBytes = 32 + 416 + 272 + 4;
static_assert(offsetof(SharedConstants, booleans) == 512 && offsetof(SharedConstants, vertex_fetch_data) == 560 &&
              offsetof(SharedConstants, memexport_streams) == 976 && offsetof(SharedConstants, tessellation) == 1040 &&
              offsetof(SharedConstants, point_state) == 1312 && offsetof(SharedConstants, flt_max) == 1340);

void SkinSharedKey(const SharedConstants &sc, u8 *out) {
  // Where the fetch copies sit in the upload ring (the data's address, its size, the slots' offsets) is left out: each
  // kick copies the bone data again to another offset. The key compares the copies' guest bytes instead (FurSkinLocked);
  // the slots' endian bits stay.
  SharedConstants k = sc;
  k.vertex_fetch_data = 0;
  k.vertex_fetch_bytes = 0;
  for (u32 &slot : k.vertex_fetch_slots)
    slot &= 3u;
  const u8 *b = reinterpret_cast<const u8 *>(&k);
  for (const auto &r : kSkinShared) {
    std::memcpy(out, b + r.offset, r.size);
    out += r.size;
  }
}

struct SkinEntry {
  uint64_t frame = UINT64_MAX;
  const ShaderRecord *vs = nullptr;
  uint64_t streams = 0, fetch_window = 0, index_address = 0, region = 0;
  u32 index_size = 0, index_format = 0, index_count = 0, vertex_count = 0, vertex_range = 0, first_vertex = 0;
  bool indexed = false;
  u8 shared[kSkinSharedBytes] = {};
  u32 copy_count = 0;
  FetchRecordCopy copies[32];
  std::vector<u8> bytes;  // the copies' guest bytes when the rows were written
};
constexpr u32 kSkinMaxFetchBytes = 64u << 10;
SkinEntry g_skin_entries[64];
std::unique_ptr<plume::RenderBuffer> g_skin_buffer[kFramesInFlight];
uint64_t g_skin_capacity[kFramesInFlight] = {};
uint64_t g_skin_used[kFramesInFlight] = {};
uint64_t g_skin_frame[kFramesInFlight] = {};
bool g_skin_writes_unseen = false;  // rows written since the last barrier

// The draw's skin mode (0 as before, 1 write, 2 read), its region bound at slot 13.
u32 FurSkinLocked(VideoState &s, plume::RenderCommandList *cmd, const ShaderRecord *vs, const Geometry &g,
                  const SharedConstants &sc, uint64_t fetch_window) {
  const uint64_t frame = TextureFrameLocked();
  const u32 slot = s.frame;
  const u32 other = IsFurVs(vs->hash) ? 0u : 4u;  // the report's counts: the fur, the other skinned draws
  if (g_skin_frame[slot] != frame + 1) {  // + 1: the zero-initialised slots start unused
    g_skin_frame[slot] = frame + 1;
    g_skin_used[slot] = 0;
  }
  const uint64_t index_address =
      g.indexed && g.index_view.buffer.ref ? g.index_view.buffer.ref->getDeviceAddress() + g.index_view.buffer.offset : 0;
  const u32 index_size = g.indexed ? g.index_view.size : 0;
  const u32 index_format = g.indexed ? u32(g.index_view.format) : 0;
  const u32 vertices = g.indexed ? g.vertex_range : g.vertex_count;
  if (!vertices || (g.indexed && !index_address)) {
    fnxf_xiii::renut_gpu::CountSkinOnce(other + 2);
    return 0;
  }
  u8 shared[kSkinSharedBytes];
  SkinSharedKey(sc, shared);
  const FetchRecord &fr = g_fetch_record;
  u32 fetch_bytes = 0;
  for (u32 i = 0; i < fr.count; ++i)
    fetch_bytes += fr.copies[i].src ? fr.copies[i].size : 0;
  if (fr.unusable || fetch_bytes > kSkinMaxFetchBytes) {
    fnxf_xiii::renut_gpu::CountSkinOnce(other + 2);
    return 0;
  }
  // The copies match an entry's: the same sources, sizes and endian modes, and the same bytes there now as when the
  // entry's rows were written.
  auto same_fetch = [&](const SkinEntry &e) {
    if (e.copy_count != fr.count)
      return false;
    size_t at = 0;
    for (u32 i = 0; i < fr.count; ++i) {
      if (!(e.copies[i] == fr.copies[i]))
        return false;
      if (fr.copies[i].src && fr.copies[i].size) {
        if (at + fr.copies[i].size > e.bytes.size() ||
            std::memcmp(e.bytes.data() + at, fr.copies[i].src, fr.copies[i].size) != 0)
          return false;
        at += fr.copies[i].size;
      }
    }
    return true;
  };
  SkinEntry *hit = nullptr, *free_entry = nullptr;
  for (SkinEntry &e : g_skin_entries) {
    if (e.frame != frame) {
      if (!free_entry)
        free_entry = &e;
      continue;
    }
    if (e.vs == vs && e.streams == g_bound_streams_hash && e.index_address == index_address && e.index_size == index_size && e.index_format == index_format &&
        e.index_count == g.index_count && e.vertex_count == g.vertex_count && e.vertex_range == g.vertex_range &&
        e.first_vertex == g.first_vertex && e.indexed == g.indexed &&
        std::memcmp(e.shared, shared, sizeof(shared)) == 0 && same_fetch(e)) {
      hit = &e;
      break;
    }
  }
  const u32 bytes = vertices * 48u;
  if (hit) {
    if (g_skin_writes_unseen) {
      const plume::RenderBufferBarrier barrier(g_skin_buffer[slot].get(), plume::RenderBufferAccess::READ);
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &barrier, 1);
      g_skin_writes_unseen = false;
    }
    plume::Deko3DBindStorage(cmd, 13, hit->region, bytes);
    fnxf_xiii::renut_gpu::CountSkinOnce(other + 1);
    return 2;
  }
  // Diagnostic: a miss where the frame has an entry of the same shader and draw size (the same part, it would seem):
  // which of the key's fields differed (the "[renut] skin once" line's near misses).
  for (const SkinEntry &e : g_skin_entries) {
    if (e.frame != frame || e.vs != vs || e.index_count != g.index_count || e.vertex_count != g.vertex_count ||
        e.vertex_range != g.vertex_range || e.indexed != g.indexed)
      continue;
    u32 fields = 0;
    fields |= e.streams != g_bound_streams_hash ? 1u : 0u;
    fields |= !same_fetch(e) ? 2u : 0u;
    fields |= e.index_address != index_address || e.index_size != index_size || e.index_format != index_format ? 4u : 0u;
    fields |= e.first_vertex != g.first_vertex ? 8u : 0u;
    u32 first_shared = UINT32_MAX;
    for (u32 i = 0; i < kSkinSharedBytes; ++i) {
      if (e.shared[i] != shared[i]) {
        first_shared = i;
        break;
      }
    }
    fields |= first_shared != UINT32_MAX ? 16u : 0u;
    fnxf_xiii::renut_gpu::CountSkinMiss(fields);
    static uint32_t logged = 0;
    if ((fields & 16u) && logged++ < 8) {
      // the key byte back to its SharedConstants offset
      u32 at = first_shared, offset = 0;
      for (const auto &r : kSkinShared) {
        if (at < r.size) {
          offset = r.offset + at;
          break;
        }
        at -= r.size;
      }
      RGPU_INFO("[skin] vs {:016X}: a part's shared constants differ from its first draw's at byte {} (other fields "
                "{:X})", vs->hash, offset, fields & 15u);
    }
    break;
  }
  if (!free_entry) {
    fnxf_xiii::renut_gpu::CountSkinOnce(other + 2);
    return 0;
  }
  const uint64_t need = (uint64_t(bytes) + 255u) & ~uint64_t(255);
  // A fresh buffer: the regions written before stay in the old one, retired with the slot. fnxf_xiii (2026-10-27):
  // RefillBuffer.
  if (!RefillBuffer(g_skin_buffer[slot], g_skin_capacity[slot], g_skin_used[slot], need,
                    plume::RenderBufferDesc::DefaultBuffer(0, StorageBufferFlags() | AddressableBufferFlags()),
                    "renut skin cache")) {
    fnxf_xiii::renut_gpu::CountSkinOnce(other + 3);
    return 0;
  }
  const uint64_t region = g_skin_buffer[slot]->getDeviceAddress() + g_skin_used[slot];
  g_skin_used[slot] += need;
  SkinEntry &e = *free_entry;
  e.frame = frame;
  e.vs = vs;
  e.streams = g_bound_streams_hash;
  e.fetch_window = fetch_window;
  e.index_address = index_address;
  e.index_size = index_size;
  e.index_format = index_format;
  e.index_count = g.index_count;
  e.vertex_count = g.vertex_count;
  e.vertex_range = g.vertex_range;
  e.first_vertex = g.first_vertex;
  e.indexed = g.indexed;
  e.region = region;
  std::memcpy(e.shared, shared, sizeof(shared));
  e.copy_count = fr.count;
  std::copy(fr.copies, fr.copies + fr.count, e.copies);
  e.bytes.clear();
  for (u32 i = 0; i < fr.count; ++i)
    if (fr.copies[i].src && fr.copies[i].size)
      e.bytes.insert(e.bytes.end(), fr.copies[i].src, fr.copies[i].src + fr.copies[i].size);
  plume::Deko3DBindStorage(cmd, 13, region, bytes);
  g_skin_writes_unseen = true;
  fnxf_xiii::renut_gpu::CountSkinOnce(other + 0);
  return 1;
}
#endif  // PLUME_DEKO3D

// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): the previous chain draw's targets and viewport (DrawStateLocked).
struct TargetsReuse {
  uint64_t seq = 0, epoch = 0;
  u32 va[d3d::kMaxRenderTargets + 1] = {};       // the device's render target words and its depth word
  u32 desc[d3d::kMaxRenderTargets + 1][4] = {};  // each one's Size, SurfaceInfo, Format (GetHostSurfaceLocked's)
  Targets targets;
};
TargetsReuse g_targets_reuse;
struct ViewportReuse {
  uint64_t seq = 0;
  u32 width = 0, height = 0, guest_width = 0, guest_height = 0, scale = 0, predication = 0;
  float drs = 0.0f;  // fnxf_xiii (2026-11-07): the dynamic resolution's factor it was made with
  size_t tiles = 0;
  ViewportState viewport;
};
ViewportReuse g_viewport_reuse;

// The descriptor words GetHostSurfaceLocked reads for a surface word of the device (0 0 0 for none).
void SurfaceDescWords(u32 va, u32 out[4]) {  // fnxf_xiii (2026-11-11, from FNXF XIII-LR): ColorInfo too
  out[0] = out[1] = out[2] = out[3] = 0;
  if (!va)
    return;
  if (const auto *desc = SurfaceDescLocked(va)) {  // fnxf_xiii: the queued snapshot's
    out[0] = desc->Size;
    out[1] = desc->SurfaceInfo;
    out[2] = desc->Format;
    out[3] = desc->ColorInfo;
  }
}

bool SameTargets(const Targets &a, const Targets &b) {
  if (a.color_count != b.color_count || a.depth != b.depth || a.width != b.width || a.height != b.height ||
      a.scale.Key() != b.scale.Key() || a.guest_width != b.guest_width || a.guest_height != b.guest_height)
    return false;
  for (u32 i = 0; i < a.color_count; ++i)
    if (a.colors[i] != b.colors[i])
      return false;
  return true;
}

// fnxf_xiii: a vertex declaration as the XDK's D3D lays it out (sub_826C8050 / SetFVF's sub_826C6F88): Common (type 5)
// at +0, the element count at +24, the elements (D3DVERTEXELEMENT9, no D3DDECL_END) from +52. Parsed once per
// address and content: SetFVF rewrites the device's own one in place. Requires State().mutex.
// fnxf_xiii (fnxf_xiii_frame_dump): see the cvar. The draw's line goes to draws.txt; its textures are read back at the next
// draw (FrameDumpFlushLocked), before that one records anything, as the readback submits and waits.
struct FrameDump {
  bool active = false;
  uint64_t frame = 0;
  int dumps = 0;
  std::string dir;
  FILE *file = nullptr;
  uint32_t draw = 0;
  std::unordered_set<u32> dumped;
  std::unordered_map<u32, uint64_t> dumped_signature;  // fnxf_xiii (2026-10-22): each texture's content when last dumped
  struct Pending {
    u32 va;
    TextureFetch fetch;
    int draw;
  };
  std::vector<Pending> pending;
  std::unordered_set<uint64_t> shaders;  // fnxf_xiii (2026-10-31): the shaders written to shaders/ this frame
  // fnxf_xiii (2026-10-31, fnxf_xiii_frame_dump_probe): the last draw's first colour target, read back at the next draw.
  HostSurface *probe_target = nullptr;
  int probe_draw = -1;
};
FrameDump g_dump;

// fnxf_xiii (2026-10-31, fnxf_xiii_frame_dump): a draw's shader as the store has it (its GLSL, the translation of the
// game's microcode) in the frame's folder, shaders/<vs|ps>_<hash>.glsl, once a frame.
void FrameDumpShaderLocked(const ShaderRecord *shader) {
  if (!shader || !shader->hash || !g_dump.shaders.insert(shader->hash).second)
    return;
  auto &store = renut::nxs::ShaderStore::Get();
  const renut::nxs::StoredShader *stored = store.IsOpen() ? store.Find(shader->hash) : nullptr;
  std::string glsl;
  if (!stored || !store.ReadGlsl(*stored, glsl))
    glsl = "// not in the shader store\n";
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(g_dump.dir) / "shaders";
  std::filesystem::create_directories(dir, ec);
  if (FILE *f = std::fopen((dir / fmt::format("{}_{:016X}.glsl", shader->pixel() ? "ps" : "vs", shader->hash))
                               .string().c_str(), "wb")) {
    std::fwrite(glsl.data(), 1, glsl.size(), f);
    std::fclose(f);
  }
}

void FrameDumpCloseLocked() {
  if (!g_dump.active)
    return;
  g_dump.active = false;
  if (g_dump.file) {
    std::fclose(g_dump.file);
    g_dump.file = nullptr;
  }
  RGPU_INFO("[fnxf_xiii] frame dump written to {} ({} draws, {} textures)", g_dump.dir, g_dump.draw,
            g_dump.dumped.size());
}

bool FrameDumpOn() {
  if (!REXCVAR_GET(fnxf_xiii_frame_dump) || !fnxf_xiii::renut_gpu::ResolveTraceOn()) {
    FrameDumpCloseLocked();
    return false;
  }
  // fnxf_xiii (2026-10-28): ZL + ZR dumps frames in a row; each new traced frame starts its own frame_N. The previous
  // one's last textures were read back into its folder by FrameDumpFlushLocked before this draw.
  static uint32_t seq = 0;
  if (g_dump.active && seq != fnxf_xiii::renut_gpu::ResolveTraceSeq())
    FrameDumpCloseLocked();
  if (!g_dump.active) {
    seq = fnxf_xiii::renut_gpu::ResolveTraceSeq();
    g_dump.active = true;
    ++g_dump.dumps;
    g_dump.draw = 0;
    g_dump.dumped.clear();
    g_dump.dumped_signature.clear();
    g_dump.shaders.clear();
    g_dump.pending.clear();
    g_dump.dir = fnxf_xiii::renut_gpu::NextToExecutable(fmt::format("frame_dump/frame_{}", g_dump.dumps)).string();
    std::error_code ec;
    std::filesystem::remove_all(g_dump.dir, ec);  // fnxf_xiii (2026-10-27): an earlier run's frame of the same number
    std::filesystem::create_directories(g_dump.dir, ec);
    g_dump.file = std::fopen((std::filesystem::path(g_dump.dir) / "draws.txt").string().c_str(), "w");
  }
  return g_dump.file != nullptr;
}

// fnxf_xiii (2026-10-31, fnxf_xiii_frame_dump_probe): the probe's rectangle of the previous draw's first colour target,
// read back now (its commands are all recorded), appended to the frame's probe.bin: a text line (the draw, the target,
// its host format and size, the rectangle in host pixels, the row bytes) and the rows.
void FrameDumpProbeLocked() {
  HostSurface *t = g_dump.probe_target;
  const int draw = g_dump.probe_draw;
  g_dump.probe_target = nullptr;
  if (!t || !g_dump.active || !t->texture || t->depth)
    return;
  static const std::string spec = REXCVAR_GET(fnxf_xiii_frame_dump_probe);
  unsigned gx = 0, gy = 0, gw = 0, gh = 0;
  if (std::sscanf(spec.c_str(), "%u,%u,%u,%u", &gx, &gy, &gw, &gh) != 4 || !gw || !gh)
    return;
  const std::string file = (std::filesystem::path(g_dump.dir) / "probe.bin").string();
  if (t->samples != plume::RenderSampleCount::COUNT_1) {
    if (FILE *f = std::fopen(file.c_str(), "ab")) {
      const std::string line = fmt::format("draw {} target {:08X} multisampled: not read\n", draw, t->guest_va);
      std::fwrite(line.data(), 1, line.size(), f);
      std::fclose(f);
    }
    return;
  }
  uint32_t block = 1;
  const uint32_t unit = DumpBlockBytes(t->format, block);
  const uint32_t tw = std::max(t->scale.Size(t->width), 1u), th = std::max(t->scale.Size(t->height), 1u);
  const uint32_t x = std::min(t->scale.Size(gx), tw - 1), y = std::min(t->scale.Size(gy), th - 1);
  const uint32_t w = std::min(std::max(t->scale.Size(gw), 1u), tw - x), h = std::min(std::max(t->scale.Size(gh), 1u), th - y);
  if (!unit || block != 1)
    return;
  const uint32_t row_bytes = ((w * unit) + 255) & ~255u;
  auto &s = State();
  std::unique_ptr<plume::RenderBuffer> readback =
      s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(row_bytes) * h));
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!readback || !cmd)
    return;
  const plume::RenderTextureLayout before = t->layout;
  TransitionSurfaceLocked(t, plume::RenderTextureLayout::COPY_SOURCE);
  const plume::RenderBox box(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
  cmd->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), t->format, w, h, 1, row_bytes / unit),
      plume::RenderTextureCopyLocation::Subresource(t->texture.get(), 0, 0), 0, 0, 0, &box);
  detail::SubmitAndWaitLocked(s);
  if (const auto *data = static_cast<const uint8_t *>(readback->map())) {
    if (FILE *f = std::fopen(file.c_str(), "ab")) {
      const std::string line = fmt::format("draw {} target {:08X} format {} size {}x{} box {} {} {} {} row {}\n", draw,
                                           t->guest_va, u32(t->format), tw, th, x, y, w, h, row_bytes);
      std::fwrite(line.data(), 1, line.size(), f);
      std::fwrite(data, 1, size_t(row_bytes) * h, f);
      std::fclose(f);
    }
    readback->unmap();
  }
  Video::RetireLocked(std::move(readback));
  TransitionSurfaceLocked(t, before == plume::RenderTextureLayout::UNKNOWN ? plume::RenderTextureLayout::COLOR_WRITE
                                                                            : before);
  g_bound = {};  // the readback submitted the list: everything is bound again
}

void FrameDumpFlushLocked() {
  FrameDumpProbeLocked();  // fnxf_xiii (2026-10-31)
  if (g_dump.pending.empty())
    return;
  auto pending = std::move(g_dump.pending);
  g_dump.pending.clear();
  for (const auto &p : pending)
    DumpTextureLocked(p.va, p.fetch, g_dump.dir, p.draw);
  // The readbacks submitted the list: everything is bound again.
  g_bound = {};
}

// fnxf_xiii: the stand-in target of exporting draws without one (see DrawStateLocked): a 64x16 8_8_8_8 surface in the
// last EDRAM tile, described by a header of its own in guest memory.
bool ExportTargetsLocked(Targets &t) {
  static u32 header_va = 0;
  if (!header_va) {
    header_va = records::AllocPhysicalBlock(64);
    auto *w = GuestPtr<be_u32>(header_va);
    if (!w)
      return false;
    for (u32 i = 0; i < 16; ++i)
      w[i] = 0;
    w[0] = 0x00000004u;  // Common: surface
    constexpr u32 kWidth = 64, kHeight = 16, kTile = 2047;
    w[6] = kWidth;                                          // SurfaceInfo: pitch, 1x
    w[7] = kTile;                                           // ColorInfo: EDRAM base, 8_8_8_8
    w[8] = 0;                                               // HiControl
    w[9] = (kHeight - 1) << 3 | (kWidth - 1) << 18;         // Size
    w[10] = 0x18280186u;                                    // D3DFMT_A8R8G8B8
    w[11] = 5120;                                           // EdramSize
  }
  HostSurface *surface = GetHostSurfaceLocked(header_va);
  if (!surface || surface->depth)
    return false;
  t = Targets{};
  t.stand_in = true;
  t.colors[0] = surface;
  t.color_count = 1;
  t.width = surface->width;
  t.height = surface->height;
  t.scale = surface->scale;
  t.guest_width = d3d::SurfaceWidth(surface->size_word);
  t.guest_height = d3d::SurfaceHeight(surface->size_word);
  return true;
}

void FrameDumpDrawLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord *vs,
                         const ShaderRecord *ps, const Targets &t, u32 texture_mask) {
  if (!FrameDumpOn())
    return;
  const d3d::Device &dev = device.dev();
  const d3d::ControlPacket &cp = dev.m_ControlPacket;
  std::string line = fmt::format(
      "draw {} prim {} count {} vs {:016X} ps {:016X} | colour", g_dump.draw++, u32(call.primitive), call.count,
      vs ? vs->hash : 0, ps ? ps->hash : 0);
  for (u32 i = 0; i < t.color_count; ++i)
    line += fmt::format(" {:08X}({}x{} f{} s{} scale {}{})", t.colors[i]->guest_va, t.colors[i]->width,
                        t.colors[i]->height, u32(t.colors[i]->format), u32(t.colors[i]->samples),
                        t.colors[i]->scale.code, t.colors[i]->scale.drs ? " drs" : "");  // fnxf_xiii (2026-11-08)
  if (t.depth)
    line += fmt::format(" depth {:08X}({}x{} host {:X} gen {} img {:X})", t.depth->guest_va, t.depth->width,
                        t.depth->height, uintptr_t(t.depth), t.depth->generation,
                        uintptr_t(t.depth->texture.get()));  // fnxf_xiii (2026-11-08, shadows)
  line += fmt::format(" | blend {:08X} mask {:04X} depthctl {:08X} colorctl {:08X} aamask {:04X} vte {:02X}",
                      u32(cp.BlendControl0), u32(dev.m_ValuesPacket.ColorMask) & 0xFFFF, u32(cp.DepthControl),
                      u32(cp.ColorControl), device.U32(0x2A00) & 0xFFFF, u32(cp.VteControl) & 0xFF);
  // fnxf_xiii (2026-10-28): predicated tiling and the viewport, to tell a draw placed for the wrong tile.
  {
    const d3d::ValuesPacket &v = dev.m_ValuesPacket;
    const float vp[4] = {float(v.VportXScale), float(v.VportXOffset), float(v.VportYScale), float(v.VportYOffset)};
    ClearRect tile{};
    const bool one_tile = PredicatedTile(device, tile);
    line += fmt::format(" | tiles {} pred {:08X} replay {:08X} tile {} vp {:g} {:g} {:g} {:g} winoff {:08X} "
                        "scissor {:08X} {:08X}",
                        g_tiles.size(), device.U32(d3d::kDeviceOffsetPredication), g_replay_predication,
                        one_tile ? fmt::format("{},{}-{},{}", tile.x1, tile.y1, tile.x2, tile.y2) : std::string("all"),
                        vp[0], vp[1], vp[2], vp[3], u32(dev.m_WindowPacket.WindowOffset),
                        u32(dev.m_WindowPacket.WindowScissorTL), u32(dev.m_WindowPacket.WindowScissorBR));
    // fnxf_xiii (2026-11-08, shadows): the depth range, stencil reference / masks and clip control.
    line += fmt::format(" | z {:g} {:g} sref {:06X} clip {:08X} modectl {:08X}", float(v.VportZScale),
                        float(v.VportZOffset), u32(v.StencilRefMask) & 0xFFFFFF, u32(cp.ClipControl),
                        u32(cp.ModeControl));
  }
  for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
    if (!(texture_mask & (1u << r)))
      continue;
    const TextureFetch fetch = DeviceTextureFetch(dev, r);
    const u32 va = device.U32(d3d::kDeviceOffsetTextures + r * 4);
    u32 w = 0, h = 0, d = 0;
    if (fetch.IsTexture())
      fetch.Size(w, h, d);
    line += fmt::format(" | t{} {:08X} fmt {} {}x{}x{} base {:05X} swz {:03X} tiled {} dim {}", r, va,
                        u32(fetch.Format()), w, h, d, fetch.BasePage(), fetch.Swizzle(), fetch.Tiled(),
                        u32(fetch.Dimension()));
    // fnxf_xiii (2026-10-31): the fetch constant's words (filters, clamps, LOD in 3-5) and the shaders' own filters
    // for it (fetch_filters.h: replaced bits and values of word 3).
    line += fmt::format(" fc {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", u32(fetch.dword[0]), u32(fetch.dword[1]),
                        u32(fetch.dword[2]), u32(fetch.dword[3]), u32(fetch.dword[4]), u32(fetch.dword[5]));
    for (const ShaderRecord *sh : {vs, ps})
      if (sh && ((sh->fetch_filters.slots >> r) & 1u))
        line += fmt::format(" {}filt {:08X}/{:08X}", sh->pixel() ? "ps" : "vs", sh->fetch_filters.clear[r],
                            sh->fetch_filters.set[r]);
    // fnxf_xiii (2026-10-22): again whenever its content changed (a resolve into it) since it was last dumped, so the
    // passes' intermediate results are all there (a scene target resolves several times a frame).
    if (va && fetch.IsTexture() && REXCVAR_GET(fnxf_xiii_frame_dump_textures)) {
      const uint64_t sig = DumpContentSignatureLocked(va, fetch);  // 0: nothing on the GPU to dump
      auto [it, added] = g_dump.dumped_signature.try_emplace(va, sig);
      if (sig && (added || it->second != sig)) {
        it->second = sig;
        g_dump.dumped.insert(va);
        g_dump.pending.push_back({va, fetch, int(g_dump.draw) - 1});
      }
    }
  }
  line += '\n';
  std::fputs(line.c_str(), g_dump.file);
  FrameDumpShaderLocked(vs);
  FrameDumpShaderLocked(ps);
  // fnxf_xiii (2026-10-31, fnxf_xiii_frame_dump_probe): read back once this draw is recorded (FrameDumpFlushLocked).
  g_dump.probe_target = t.color_count ? t.colors[0] : nullptr;
  g_dump.probe_draw = int(g_dump.draw) - 1;
}

// fnxf_xiii (2026-10-31, fnxf_xiii_frame_dump): the pixel shader's float constants it reads ("psc:"), after the draw's lines.
void FrameDumpPixelConstantsLocked(const ShaderRecord *ps, const FloatConstants &constants) {
  if (!g_dump.active || !g_dump.file || !ps)
    return;
  static constexpr u32 kAll[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  const u32 *used = ps->entry ? ps->entry->floatConstants : kAll;
  std::string line = "    psc:";
  for (u32 r = 0; r < 256; ++r) {
    if (!((used[r / 32] >> (r % 32)) & 1u))
      continue;
    float v[4];
    for (u32 i = 0; i < 4; ++i) {
      u32 w;
      std::memcpy(&w, reinterpret_cast<const u8 *>(constants.Register(r)) + i * 4, 4);
      w = std::byteswap(w);
      std::memcpy(&v[i], &w, 4);
    }
    line += fmt::format(" c{}=({:g},{:g},{:g},{:g})", r, v[0], v[1], v[2], v[3]);
  }
  line += '\n';
  std::fputs(line.c_str(), g_dump.file);
}

// fnxf_xiii (2026-10-28, fnxf_xiii_frame_dump): after a draw's line, where each vertex stream is read from (the GPU's export
// window or guest memory), a hash of its bytes in guest memory, and a hash of the vertex shader's constants: two frames
// whose same draw differs in them got different vertices, or the same vertices from a different place.
void FrameDumpStreamsLocked(const GuestDevice &device, const VertexDeclarationRecord &declaration,
                            const GpuStreams &gpu, const FloatConstants &vs_constants, const u32 *vs_used) {
  if (!g_dump.active || !g_dump.file)
    return;
  std::string line = fmt::format("    decl {} {:08X} elements {}; streams:", g_decl_source, g_decl_va,
                                 declaration.elements.size());  // fnxf_xiii (2026-11-08, shadows)
  if (g_decl_source == 4 && g_decl_vs && g_decl_vs->entry) {  // the shader's own vfetches (ShaderFetchDeclarationLocked)
    const ShaderCacheEntry &e = *g_decl_vs->entry;
    line += fmt::format(" vs inputs {}:", e.vertexInputCount);
    for (u32 i = 0; i < e.vertexInputCount && i < 12; ++i) {
      const ShaderVertexInput &in = g_shaderVertexInputs[e.vertexInputOffset + i];
      line += fmt::format(" [usage {} {} fetch {} format {} flags {} stride {} offset {}]", in.usage, in.usageIndex,
                          in.fetchIndex, in.format, in.flags, in.strideDwords, in.offsetDwords);
    }
    line += ";";
  }
  for (u32 stream = 0; stream < kMaxVertexStreams; ++stream) {
    if (!declaration.layout.streams[stream].used)
      continue;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
    const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
    // Fetch constants hold GPU (physical) addresses: hashed through the physical view, within its 512 MB.
    const u32 phys = base & 0x1FFFFFFFu;
    const u32 bytes = std::min<u32>({size, 4u << 20, 0x20000000u - phys});
    const uint64_t hash = base && bytes ? XXH3_64bits(GuestPhysicalPtr<const u8>(phys), bytes) : 0;
    line += fmt::format(" s{} {:08X} size {:X} {} guest {:016X};", stream, base, size,
                        gpu.bytes[stream] ? "from-export-window" : "from-guest", hash);
  }
  u8 constants[256 * 16];
  for (u32 r = 0; r < 256; ++r)
    std::memcpy(constants + r * 16, vs_constants.Register(r), 16);
  line += fmt::format(" vs constants {:016X}\n", XXH3_64bits(constants, sizeof(constants)));
  // fnxf_xiii (2026-10-28): the values of the registers the vertex shader reads (all when it indexes them, then only
  // the non-zero ones), as floats: what places this draw's vertices.
  if (vs_used) {
    line += "    vsc:";
    for (u32 r = 0; r < 256; ++r) {
      if (!((vs_used[r / 32] >> (r % 32)) & 1))
        continue;
      float v[4];
      for (u32 i = 0; i < 4; ++i) {
        u32 w;
        std::memcpy(&w, constants + r * 16 + i * 4, 4);
        w = std::byteswap(w);
        std::memcpy(&v[i], &w, 4);
      }
      if (vs_used[0] == ~0u && vs_used[7] == ~0u && v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0)
        continue;
      line += fmt::format(" c{}=({:g},{:g},{:g},{:g})", r, v[0], v[1], v[2], v[3]);
    }
    line += '\n';
  }
  std::fputs(line.c_str(), g_dump.file);
}

const VertexDeclarationRecord *NativeDeclarationLocked(u32 va) {
  // fnxf_xiii (2026-11-06, seen21): one record per content, shared by every address that holds it. Keyed by address
  // alone, each new declaration object built its layout again (BuildDeclarationLayout, a few dozen allocations): run
  // 9a82c4a6, an area entry built 92 in one frame for 18.2 ms of the render thread while the game's loaders were
  // allocating too. The record of a native declaration is only read (its elements, layout and layout_hash); nothing
  // keys anything by its self_va (shader.cpp's are the renderer's own records), so one per content serves them all.
  struct Cached {
    uint64_t hash = 0;
    const VertexDeclarationRecord *record = nullptr;
  };
  static std::unordered_map<u32, Cached> cache;
  static std::unordered_map<uint64_t, std::unique_ptr<VertexDeclarationRecord>> by_content;
  const auto *object = GuestPtr<const u8>(va);
  if (!object)
    return nullptr;
  const u32 common = *reinterpret_cast<const be_u32 *>(object);
  const u32 count = *reinterpret_cast<const be_u32 *>(object + 24);
  if ((common & 0xF) != 5 || count == 0 || count > 64)
    return nullptr;
  const uint64_t hash = XXH3_64bits(object + 24, 28 + size_t(count) * sizeof(GuestVertexElement));
  Cached &slot = cache[va];
  if (slot.record && slot.hash == hash)
    return slot.record;
  if (cache.size() > 8192 || by_content.size() > 8192) {
    ++rtw::g_decl_clears;  // fnxf_xiii (2026-11-06, seen20, [rtwork])
    cache.clear();
    by_content.clear();
    return NativeDeclarationLocked(va);
  }
  std::unique_ptr<VertexDeclarationRecord> &shared = by_content[hash];
  if (!shared) {
    ++rtw::g_decl_builds;
    auto record = std::make_unique<VertexDeclarationRecord>();
    record->self_va = va;  // the first address it was seen at
    record->kind = RecordKind::VertexDeclaration;
    const auto *elements = reinterpret_cast<const GuestVertexElement *>(object + 52);
    for (u32 i = 0; i < count && u32(elements[i].Type) != kDeclTypeUnused; ++i)
      record->elements.push_back(elements[i]);
    BuildDeclarationLayout(*record);
    shared = std::move(record);
  } else {
    ++rtw::g_decl_shared;
  }
  slot.hash = hash;
  slot.record = shared.get();
  return slot.record;
}

void NoteOverlayResolutionsLocked(const HostSurface *depth, bool with_colour);  // fnxf_xiii
// fnxf_xiii (2026-10-25, fnxf_xiii_renut_upscale_filter): the game's upscale pixel shader (one 1280x720 draw a frame from
// the 1024x576 picture, 9 fetches and 12 sines a pixel: 2.6 ms of GPU in the 2026-10-25 profile), drawn with Renut's
// shaders instead (DrawStateLocked).
constexpr uint64_t kGameUpscalePs = 0x94709CAEEDEE2378ull;
// fnxf_xiii (2026-11-07): the pixel shader of a 1280x720 scene's copy of its finished picture into the scene's colour
// target (the field has no upscale draw then): FSR 3.1's native path, the Upscaler's 1080p output.
constexpr uint64_t kSceneCopyPs = 0xC0D9E64179DEDD6Bull;
// fnxf_xiii (2026-11-08): the draw that copies the finished picture into the scene's colour target (kSceneCopyPs, no
// depth target, the colour target FSR 3.1's motion pass knew as the scene's when there is one).
bool PictureCopy(const GuestDevice &device, const ShaderRecord *ps) {
  if (!ps || ps->hash != kSceneCopyPs || device.U32(d3d::kDeviceOffsetDepthStencil))
    return false;
  const HostSurface *scene = g_motion_target.scene;
  return !scene || device.U32(d3d::kDeviceOffsetRenderTargets) == scene->guest_va;
}
int32_t UpscaleFilter() {
  static const int32_t filter = REXCVAR_GET(fnxf_xiii_renut_upscale_filter);
  // fnxf_xiii (2026-11-08): prerendered movies always with Lanczos-2 (FSR 1 gave them random hitches).
  if (filter == 3 && fnxf_xiii::frame_interval::MoviePlaying())
    return 1;
  return filter;
}
// Once: which shader, and the game's own constants of the draw (c0, c1: the sizes Renut's shaders read; c253-c255: the
// game shader's kernel constants), raw guest words, to check the filter Renut's fit reproduces.
void LogUpscaleOnce(int32_t filter, const FloatConstants &constants) {
  static bool logged = false;
  if (logged)
    return;
  logged = true;
  if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[upscale] the game's upscale drawn with Renut's {} (fnxf_xiii_renut_upscale_filter {})",
            filter == 1   ? "Lanczos-2"
            : filter == 3 ? "FSR 1 (EASU, then RCAS)"
            : filter == 4 ? "plain bilinear"
                          : "fit of the game's filter",
            filter);
  for (const u32 r : {0u, 1u, 253u, 254u, 255u}) {
    const u8 *p = constants.Register(r);
    u32 w[4];
    std::memcpy(w, p, sizeof(w));
    if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[upscale] c{}: {:08X} {:08X} {:08X} {:08X}", r, w[0], w[1], w[2], w[3]);
  }
}

// fnxf_xiii (2026-10-27): the game's copy kernel (vs_BE95EE803D59E654, the area loads' 0.1-1 s of GPU time a frame) as a
// buffer copy. Each point reads its vertex (8 elements of 16_16_16_16 unsigned integers, 64 bytes) and exports it as
// format 26 (16_16_16_16, integer, unsigned, 8in16) to element 8 * (index - trunc(c1.x) + trunc(c1.y)) + c0.y - 2^23 of
// c0's stream: a move of whole vertices inside the export window (the game compacting its texture pool). Its stream
// comes from a pending export (stream_convert_cs: 8in32, components 1 0 3 2), so the bytes it writes are the bytes it
// read (a word B0 B1 B2 B3: 8in32 then the pair swap gives x = B0B1, y = B2B3; exported x | y << 16 in 8in16 is
// B0 B1 B2 B3 again; a 16-bit integer goes through a float exactly). The vertex shader's work (81-89 ns a vertex) and
// the conversion's (36-39 ns) become a copy (the copy engine: 32 MB in 5.2 ms). Only for draws that match all of it;
// any other goes the shader's way. (fnxf_xiii, the copykernel lines of 2026-10-27's run.)
// fnxf_xiii (2026-10-27, fnxf_xiii_diag_watch): every draw of the copy kernel while a range is watched: the guest range
// it reads, where its vertices come from (an export window, or guest memory), and the guest range it writes, so the
// pool's moves can be followed (the minimap's 512 KB is moved into place by one of them).
void LogCopyKernelLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                         const u32 *shader_strides, const VertexDeclarationRecord &declaration, const Geometry &g,
                         const FloatConstants &vs_constants, const MemExportStream *exports, u32 export_count) {
  if (!WatchOn() || vs.hash != 0xBE95EE803D59E654ull || export_count != 1)
    return;
  static uint32_t logged = 0;
  if (logged >= 4000)
    return;
  float c[8];
  for (u32 r = 0; r < 2; ++r) {
    const u8 *reg = vs_constants.Register(r);
    for (u32 k = 0; k < 4; ++k) {
      u32 w;
      std::memcpy(&w, reg + k * 4, 4);
      w = __builtin_bswap32(w);
      std::memcpy(&c[r * 4 + k], &w, 4);
    }
  }
  u32 stream = kMaxVertexStreams;
  for (u32 i = 0; i < kMaxVertexStreams; ++i)
    if (declaration.layout.streams[i].used && stream == kMaxVertexStreams)
      stream = i;
  if (stream == kMaxVertexStreams)
    return;
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
  const u32 first = g.first_vertex, count = g.vertex_count;
  const uint64_t src_begin = uint64_t(base) + uint64_t(first) * 64, src_end = src_begin + uint64_t(count) * 64;
  const int64_t dest_index = int64_t(c[1] - 8388608.0f) +
                             8 * (int64_t(first) - int64_t(std::trunc(c[4])) + int64_t(std::trunc(c[5])));
  const uint64_t dst_begin = uint64_t(exports[0].base) + uint64_t(dest_index) * 8, dst_end = dst_begin + uint64_t(count) * 64;
  ExportedStream src;
  std::string origin;
  if (FindExportedStream(device, call, vs, shader_strides, declaration, stream, src))
    origin = fmt::format("window of slot {} +{:X}", src.slot, src.offset + uint64_t(first) * 64);
  else if (PendingExportSlots(base, size))
    origin = "guest memory, with exports pending over it (the draw waits for them)";
  else
    origin = "guest memory";
  const bool hit = WatchHit(u32(dst_begin), u32(dst_end - dst_begin)) || WatchHit(u32(src_begin), u32(src_end - src_begin));
  ++logged;
  RGPU_INFO("[copylog] frame {} slot {}: {:08X}-{:08X} -> {:08X}-{:08X} ({} vertices, buffer {:08X}+{:X}), source {}{}",
            TextureFrameForLog(), State().frame, src_begin, src_end, dst_begin, dst_end, count, base, size, origin,
            hit ? "  <== watched range" : "");
  // A move into the watched range: the source's bytes as guest memory has them now, beside the watch snapshots (what
  // the move should write when guest memory is the newest data).
  static u32 dumps = 0;
  if (WatchHit(u32(dst_begin), u32(dst_end - dst_begin)) && dumps < 8) {
    if (const u8 *p = GuestPhysicalPtr<const u8>(u32(src_begin))) {
      std::error_code ec;
      const std::filesystem::path dir = fnxf_xiii::renut_gpu::NextToExecutable("watch_dump");
      std::filesystem::create_directories(dir, ec);
      const std::string name = fmt::format("copysrc_{:02}_f{}_{:08X}.bin", dumps++, TextureFrameForLog(), u32(src_begin));
      if (FILE *f = std::fopen((dir / name).string().c_str(), "wb")) {
        const std::string head = fmt::format("RNWATCH1 begin {:08X} end {:08X} frame {} when the copy's source to "
                                             "{:08X}, guest memory at the draw\n",
                                             u32(src_begin), u32(src_end), TextureFrameForLog(), u32(dst_begin));
        std::fwrite(head.data(), 1, head.size(), f);
        std::fwrite(p, 1, size_t(src_end - src_begin), f);
        std::fclose(f);
      }
    }
  }
}

// fnxf_xiii (2026-10-29): 'offsets' are the draw's windows as DrawStateLocked's PrepareMemExportLocked placed and
// seeded them. This prepared them a second time before: the window is not pending until FinishMemExportLocked, so the
// second preparation found it fresh at the same place, cleared its flags again and seeded again every page whose record
// did not prove it (a new pool's pages before their first completion: all 32 MB, twice a frame; and every page the
// game changed, which the first preparation had just copied).
bool CopyKernelAsCopyLocked(VideoState &s, const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                            const u32 *shader_strides, const VertexDeclarationRecord &declaration, const Geometry &g,
                            const FloatConstants &vs_constants, const MemExportStream *exports, u32 export_count,
                            const u32 *offsets) {
  const rtw::Scope rtw_mx(rtw::kMxCopyKernel);  // fnxf_xiii (2026-11-05, [rtwork])
  constexpr uint64_t kCopyKernelVs = 0xBE95EE803D59E654ull;
  if (vs.hash != kCopyKernelVs || export_count != 1 || call.indexed || call.tessellation ||
      call.primitive != kPointList || g.instance_count != 1 || g.per_instance)
    return false;
  const MemExportStream &e = exports[0];
  if (e.format != 26 || e.endian != 1 || e.num_format != 2)
    return false;
  // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the copy's dwords reach the write mask by a compute pass
  // (FlushExportMaskRangesLocked); without the pipeline the kernel runs and marks them itself.
  if (ExportMaskOn() && !s.stream_convert_pipeline)
    return false;
  // c0 and c1 as the guest has them (big-endian floats).
  float c[8];
  for (u32 r = 0; r < 2; ++r) {
    const u8 *reg = vs_constants.Register(r);
    for (u32 k = 0; k < 4; ++k) {
      u32 w;
      std::memcpy(&w, reg + k * 4, 4);
      w = __builtin_bswap32(w);
      std::memcpy(&c[r * 4 + k], &w, 4);
    }
  }
  u32 c0x;
  std::memcpy(&c0x, &c[0], 4);
  if ((c0x & 0x3FFFFFFFu) * 4u != e.base)
    return false;
  const float first_index = c[1] - 8388608.0f, element_count = c[3] - 8388608.0f;
  if (!(first_index >= 0.0f) || !(element_count >= 0.0f) || first_index != std::floor(first_index))
    return false;
  // The single stream: from a pending export, 64-byte vertices of the 8 elements above.
  u32 stream = kMaxVertexStreams;
  for (u32 i = 0; i < kMaxVertexStreams; ++i) {
    if (!declaration.layout.streams[i].used)
      continue;
    if (stream != kMaxVertexStreams)
      return false;
    stream = i;
  }
  ExportedStream src;
  if (stream == kMaxVertexStreams ||
      !FindExportedStream(device, call, vs, shader_strides, declaration, stream, src, true) || src.stride != 64 ||
      src.host_stride != 128 || src.element_count != 8)
    return false;
  for (u32 i = 0; i < 8; ++i)
    if (src.elements[2 * i] != 0x00260000u + i * 0x402u || src.elements[2 * i + 1] != 0x0013061Au)
      return false;
  const u32 first = g.first_vertex, count = g.vertex_count;
  if (!count || uint64_t(first) + count > src.vertex_count)
    return false;
  // The destination: element 8 * (index - trunc(c1.x) + trunc(c1.y)) + first_index of the first vertex, 8 bytes each.
  const int64_t dest_index = int64_t(first_index) + 8 * (int64_t(first) - int64_t(std::trunc(c[4])) +
                                                         int64_t(std::trunc(c[5])));
  const uint64_t bytes = uint64_t(count) * 64;
  if (dest_index < 0 || uint64_t(dest_index) + uint64_t(count) * 8 > uint64_t(element_count) ||
      uint64_t(dest_index) * 8 + bytes > e.size)
    return false;

  plume::RenderBuffer *dest = ExportBuffer(s, s.frame);  // fnxf_xiii (2026-10-30): guest memory itself (ZeroCopyOn)
  const uint64_t dest_offset = offsets[0] + uint64_t(dest_index) * 8;
  uint64_t source_offset = src.offset + uint64_t(first) * 64;
  // fnxf_xiii (2026-10-30, ZeroCopyOn): the source read in place only when it is a kernel's output; otherwise copied
  // from guest memory now (below), as the game's fence promises (ZeroCopyRange).
  const bool snapshot = src.slot == kSnapshotSlot ||
                        (ZeroCopyOn() && !ZeroCopyReadOnGpuLocked(u32(source_offset), u32(bytes)));
  const plume::RenderBuffer *source = snapshot ? nullptr : ExportBuffer(s, src.slot);
  if (!dest || (!source && !snapshot))
    return false;
  if (dest == source && dest_offset < source_offset + bytes && source_offset < dest_offset + bytes)
    return false;  // overlapping in one buffer: the shader's way (with the window as prepared)
  UploadAllocation snap{};
  if (snapshot) {
    // The source as the game left it when it issued the draw (ZeroCopyRange): its bytes into the upload ring.
    const u8 *guest_src = GuestPhysicalPtr<const u8>(u32(source_offset));
    if (!guest_src || source_offset + bytes > 0x20000000ull)
      return false;
    snap = Video::UploadsLocked().Allocate(u32(bytes), 256);
    if (!snap)
      return false;
    ParallelMemcpy(snap.cpu, guest_src, size_t(bytes));  // fnxf_xiii (2026-11-05, seen15): over the copy helpers
    source = snap.ref.ref;
    source_offset = snap.ref.offset;
  }
  // fnxf_xiii (2026-10-27): the page flags of the pages the copy writes (what memExport sets for the shader's way).
  // fnxf_xiii (2026-10-30, ZeroCopyOn): in the slot's flags buffer, by physical page (dest_offset is the address).
  UploadAllocation ones{};
  uint64_t flags_at = 0;
  u32 flags_bytes = 0;
  plume::RenderBuffer *flags_buffer = dest;
  if (ZeroCopyOn()) {
    flags_buffer = g_zero_copy_flags[s.frame].get();
    if (!flags_buffer)
      return false;
    const uint64_t first_page = dest_offset >> 12, last_page = (dest_offset + bytes - 1) >> 12;
    flags_bytes = u32(last_page - first_page + 1) * 4u;
    flags_at = first_page * 4;
  } else if (ExportFlagBytes(e.size)) {
    const uint64_t first_page = (uint64_t(dest_index) * 8) >> 12, last_page = (uint64_t(dest_index) * 8 + bytes - 1) >> 12;
    flags_bytes = u32(last_page - first_page + 1) * 4u;
    flags_at = ExportFlagOffset(offsets[0], e.size) + first_page * 4;
  }
  if (flags_bytes) {
    ones = Video::UploadsLocked().Allocate(flags_bytes, 256);
    if (!ones)
      return false;
    for (u32 i = 0; i < flags_bytes / 4; ++i)
      static_cast<u32 *>(ones.cpu)[i] = 1u;
  }

  static const uint32_t kProfileLabel = gpu_profile::Label("memexport copy kernel as a buffer copy [bytes]");
  gpu_profile::Mark(s, kProfileLabel);
  gpu_profile::AddAmount(s, kProfileLabel, bytes);
  plume::RenderCommandList *cmd = s.command_list;
  if (dest == source || snapshot) {  // a snapshot's upload needs no barrier (the ring's lines are cleaned at the submit)
    cmd->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE));
  } else {
    plume::RenderBufferBarrier copy[2] = {
        plume::RenderBufferBarrier(const_cast<plume::RenderBuffer *>(source), plume::RenderBufferAccess::READ),
        plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE)};
    cmd->barriers(plume::RenderBarrierStage::COPY, copy, 2);
  }
  if (flags_buffer != dest)
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderBufferBarrier(flags_buffer, plume::RenderBufferAccess::WRITE));
  cmd->copyBufferRegion(plume::RenderBufferReference(dest, dest_offset), plume::RenderBufferReference(source, source_offset),
                        bytes);
  if (flags_bytes)
    cmd->copyBufferRegion(plume::RenderBufferReference(flags_buffer, flags_at), ones.ref, flags_bytes);
  if (flags_buffer != dest) {
    // fnxf_xiii (2026-10-30): a full barrier (COPY: the copy engine's work done) before the kernels write on.
    plume::RenderBufferBarrier after[2] = {plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE),
                                           plume::RenderBufferBarrier(flags_buffer, plume::RenderBufferAccess::WRITE)};
    cmd->barriers(plume::RenderBarrierStage::COPY | plume::RenderBarrierStage::GRAPHICS, after, 2);
  } else {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE));
  }
  g_bound.framebuffer = nullptr;  // the copy ends any render pass
  // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the dwords the copy wrote, into the window's write mask at
  // the end of the recording (FlushExportMaskRangesLocked: one compute pass for the frame's copies; only the CPU reads
  // the mask, after the slot's fence). fnxf_xiii (2026-10-30): no mask with the windows in guest memory (ZeroCopyOn).
  if (flags_bytes && ExportMaskOn() && !ZeroCopyOn())
    g_mask_ranges[s.frame].push_back({dest_offset, bytes});
  if (ZeroCopyOn())  // fnxf_xiii (2026-10-30): a kernel output the GPU may read in place (ZeroCopyReadOnGpuLocked)
    g_zero_copy_written[s.frame].push_back({u32(dest_offset), u32(bytes)});
  FinishMemExportLocked(s, exports, 1, offsets, false);
  static uint32_t logged = 0;
  if (logged++ < 4)
    FNXF_XIII_BURST_LOG(RGPU_INFO, "[copykernel] draw of {} vertices made a copy: {} bytes from {} +{:X} to the window at +{:X}", count,
              bytes, snapshot ? "a copy of guest memory" : fmt::format("slot {}", src.slot), source_offset,
              dest_offset - offsets[0]);
  return true;
}

// fnxf_xiii (2026-11-05, the bursts): the render thread's work by kind, for frames where it worked long ([rtwork], with
// fnxf_xiii_diag_vsync_timing). Draws, and inside them their steps; clears; resolves; the rest of the thread's busy time
// is everything else it ran (bookkeeping, presents, uploads outside draws). Render thread only.
namespace rtw {
double g_ms[kCount] = {};
uint32_t g_n[kCount] = {};
std::atomic<uint64_t> g_log_ns{0};
uint64_t g_fill_stream_bytes = 0, g_fill_copy_bytes = 0;
uint64_t g_shader_misses = 0, g_decl_builds = 0, g_decl_clears = 0, g_decl_shared = 0;
uint64_t g_fill_first_bytes = 0, g_fill_changed_bytes = 0, g_fill_waiting_bytes = 0, g_fill_full_bytes = 0,
         g_fill_untracked_bytes = 0;
bool g_in_replay = false;
bool g_in_draw = false;
std::chrono::steady_clock::time_point g_frame_item_t0{};
bool On() { return fnxf_xiii::frame_timing::On(); }
}  // namespace rtw

void RenderWorkReportLocked(uint64_t frame, double busy_ms) {
  double ms[rtw::kCount];
  uint32_t n[rtw::kCount];
  std::memcpy(ms, rtw::g_ms, sizeof(ms));
  std::memcpy(n, rtw::g_n, sizeof(n));
  std::fill(std::begin(rtw::g_ms), std::end(rtw::g_ms), 0.0);
  std::fill(std::begin(rtw::g_n), std::end(rtw::g_n), 0u);
  auto take_mb = [](uint64_t &v) {
    const double mb = double(v) / 1048576.0;
    v = 0;
    return mb;
  };
  const double first_mb = take_mb(rtw::g_fill_first_bytes), changed_mb = take_mb(rtw::g_fill_changed_bytes),
               waiting_mb = take_mb(rtw::g_fill_waiting_bytes), full_mb = take_mb(rtw::g_fill_full_bytes),
               untracked_mb = take_mb(rtw::g_fill_untracked_bytes);
  const uint64_t shader_misses = rtw::g_shader_misses, decl_builds = rtw::g_decl_builds, decl_clears = rtw::g_decl_clears,
                 decl_shared = rtw::g_decl_shared;
  rtw::g_shader_misses = rtw::g_decl_builds = rtw::g_decl_clears = rtw::g_decl_shared = 0;
  const double dynamic_mb = double(g_dynamic_fill_bytes) / 1048576.0, quiet_mb = double(g_quiet_fill_bytes) / 1048576.0;
  const uint64_t redeemed = g_redeemed_streams;
  g_dynamic_fill_bytes = g_quiet_fill_bytes = g_redeemed_streams = 0;
  const double fill_stream_mb = double(rtw::g_fill_stream_bytes) / 1048576.0;
  const double fill_copy_mb = double(rtw::g_fill_copy_bytes) / 1048576.0;
  rtw::g_fill_stream_bytes = rtw::g_fill_copy_bytes = 0;
  if (!rtw::On())
    return;
  // The render thread's own CPU time for this frame: what of the busy time it really ran (the rest: another
  // thread on its core, or a wait no WaitScope covers).
  // fnxf_xiii (2026-11-05, seen14): the baseline is taken every frame, before the early return below (seen13 took it
  // only on reported frames, so its CPU time ran from the previous slow frame and could exceed the busy time).
  double cpu_ms = 0.0;
#if defined(__SWITCH__)
  {
    static uint64_t last_ticks = 0;
    u64 ticks = 0;
    if (R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX)) ||
        R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, UINT64_MAX))) {
      if (last_ticks && ticks >= last_ticks)
        cpu_ms = double(armTicksToNs(ticks - last_ticks)) / 1e6;
      last_ticks = ticks;
    }
  }
#endif
  // The same for the deko3d.log writes: this frame's only.
  const double log_ms = double(rtw::g_log_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
  if (busy_ms < 40.0)
    return;
  static uint32_t logged = 0;
  if (logged++ >= 200)
    return;
  double steps = ms[rtw::kMxPrepare] + ms[rtw::kMxCopyKernel] + (ms[rtw::kMxComplete] - ms[rtw::kMxCompleteOutside]) +
                 ms[rtw::kHostShader] + ms[rtw::kDescribeFetch] + ms[rtw::kDrawLookup] + ms[rtw::kPipeKey] +
                 ms[rtw::kRecord] + ms[rtw::kDeclLookup];
  for (uint32_t i = rtw::kExportWait; i <= rtw::kConstants; ++i)
    steps += ms[i];
  // Draws inside command buffer replays are counted as draws, not again as replays.
  const double replay_own = std::max(0.0, ms[rtw::kReplay] - ms[rtw::kReplayDraws]);
  // The frame's own item so far (its present, the bookkeeping up to here) holds the completions it runs (in
  // SubmitAndAdvance): not twice.
  const double frame_item_ms =
      rtw::g_frame_item_t0.time_since_epoch().count()
          ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rtw::g_frame_item_t0).count()
          : 0.0;
  const double frame_item_own = std::max(0.0, frame_item_ms - ms[rtw::kMxCompleteOutside]);
  const double others = std::max(0.0, busy_ms - ms[rtw::kDraw] - ms[rtw::kClear] - ms[rtw::kResolve] - replay_own -
                                           ms[rtw::kShaderMake] - ms[rtw::kRelease] - ms[rtw::kKick] -
                                           ms[rtw::kMxCompleteOutside] - frame_item_own - ms[rtw::kLockWait] -
                                           ms[rtw::kMiscItems] - ms[rtw::kKickVertexFills] - ms[rtw::kKickTextureFills]);
  RGPU_INFO("[rtwork] frame {}: render thread busy {:.1f} ms: draws {:.1f} ms ({}; of it: export waits {:.1f}, targets "
            "{:.1f}, host shaders {:.1f}, fetch descriptions {:.1f}, geometry {:.1f} (index cache lookups {:.1f}, index reads {:.1f}, index tracking {:.1f}), exported streams {:.1f}, fetch data {:.1f}, textures {:.1f} "
            "(uploads included), pipelines {:.1f}, constants {:.1f}, memexport setup {:.1f} ({}), the game's copy kernel "
            "as copies {:.1f} ({}), memexport completions {:.1f}, shader lookups {:.1f} ({} memo misses), declarations "
            "{:.1f} ({} built, {} found by content, {} cache clears), pipeline keys {:.1f}, "
            "command recording {:.1f}, the rest of the draws {:.1f}), clears {:.1f} ms ({}), "
            "resolves {:.1f} ms ({}), command buffer replays besides their draws {:.1f} ms ({}), shaders made {:.1f} ms "
            "({}), resources released {:.1f} ms ({}), kicks {:.1f} ms ({}), memexport completions outside draws {:.1f} ms "
            "({}), the frame's own item {:.1f} ms (present {:.1f}, of it submit's memory publish {:.1f} and slot wait {:.1f}), "
            "waits for the video mutex {:.1f} ms ({}), other small items {:.1f} ms ({}), the kicks' fills: vertex {:.1f} ms ({}; {:.1f} MB "
            "of streams (guest bytes: first seen {:.1f} MB, changed since the last flush {:.1f} MB, unchanged waiting "
            "for their copy {:.1f} MB, the cache full {:.1f} MB, no cache entry or dynamic {:.1f} MB), of them dynamic "
            "{:.1f} MB, unwritten since their last conversion {:.1f} MB, {} streams cached again; {:.1f} MB of fetch "
            "copies) and texture {:.1f} ms ({}), deko3d.log writes (any thread) "
            "{:.1f} ms, everything else {:.1f} ms; the thread ran {:.1f} ms of the frame (CPU time)",
            frame, busy_ms, ms[rtw::kDraw], n[rtw::kDraw], ms[rtw::kExportWait], ms[rtw::kTargets], ms[rtw::kHostShader],
            ms[rtw::kDescribeFetch], ms[rtw::kGeometry],
            ms[rtw::kGeoLookup], ms[rtw::kGeoRead], ms[rtw::kGeoTrack],
            ms[rtw::kExportStreams], ms[rtw::kFetch], ms[rtw::kTextures], ms[rtw::kPipeline], ms[rtw::kConstants],
            ms[rtw::kMxPrepare], n[rtw::kMxPrepare], ms[rtw::kMxCopyKernel], n[rtw::kMxCopyKernel],
            ms[rtw::kMxComplete] - ms[rtw::kMxCompleteOutside], ms[rtw::kDrawLookup], shader_misses,
            ms[rtw::kDeclLookup], decl_builds, decl_shared, decl_clears, ms[rtw::kPipeKey], ms[rtw::kRecord],
            std::max(0.0, ms[rtw::kDraw] - steps), ms[rtw::kClear], n[rtw::kClear], ms[rtw::kResolve],
            n[rtw::kResolve], replay_own, n[rtw::kReplay], ms[rtw::kShaderMake], n[rtw::kShaderMake], ms[rtw::kRelease],
            n[rtw::kRelease], ms[rtw::kKick], n[rtw::kKick], ms[rtw::kMxCompleteOutside], n[rtw::kMxCompleteOutside],
            frame_item_own, ms[rtw::kPresent], ms[rtw::kSubmitPublish], ms[rtw::kWaitSlot], ms[rtw::kLockWait],
            n[rtw::kLockWait], ms[rtw::kMiscItems], n[rtw::kMiscItems], ms[rtw::kKickVertexFills],
            n[rtw::kKickVertexFills], fill_stream_mb, first_mb, changed_mb, waiting_mb, full_mb, untracked_mb,
            dynamic_mb, quiet_mb, redeemed, fill_copy_mb, ms[rtw::kKickTextureFills], n[rtw::kKickTextureFills], log_ms, others, cpu_ms);
}

// ---------------------------------------------------------------------------
// Motion vectors (fnxf_xiii, 2026-11-06, fnxf_xiii_motion_vectors; for FSR 3)
// ---------------------------------------------------------------------------
// Per object: a scene draw's vertex shader runs a second time with the float constants the same draw had in the previous
// frame (nx_shaders/xenos_glsl.cpp MotionVectorPass: UBO 4), so the previous position comes from the previous world
// matrix, camera and bones, whatever the shader does with them. Frame dumps of the Steppe (notes, 2026-11-06): the scene
// draws' vertex shaders read the world matrix c0-c3, World * View * Projection c8-c11, the camera c16-c18 and the
// projection c165-c168 (bones from c21); a draw is the same draw in the next frame by its vertex count, shaders and
// vertex stream addresses (and its rank among draws with all of those equal): 101 of 101 matched with the camera still,
// 144 of 146 turning (two objects entering the view).
//
// A draw counts as the scene's when it has a colour target and a depth target and its vertex shader reads those camera
// registers (shadow maps have no colour target; post-processing and the UI read none of them). The constants kept are
// the shader's view of all 256 registers (the guest's, byte-swapped) with its literal registers, as UploadFloatConstants
// writes them; 4 KB a draw, ~150 draws a frame. A draw with no match last frame gets its own constants (no motion).
struct MotionBlock {
  u8 bytes[kFloatConstantsUboBytes];
};
struct MotionFrame {
  std::vector<MotionBlock> blocks;
  std::unordered_map<uint64_t, uint32_t> index;  // draw key (with its rank) -> block
  std::unordered_map<uint64_t, uint32_t> ranks;  // draw key without the rank -> draws seen with it
  // fnxf_xiii (2026-11-08, the cutscenes): the same without the buffer addresses (shaders, count, where it starts in its
  // buffers, rank): a real-time cutscene's characters are drawn from vertex / index buffers that alternate from frame to
  // frame, so no draw of theirs matched the last frame's by the full key: more than half of the scene unmatched, a
  // "cut", FSR's history reset nearly every frame (ghosting and wrong colours), and no motion for them.
  std::unordered_map<uint64_t, uint32_t> loose_index;
  std::unordered_map<uint64_t, uint32_t> loose_ranks;
  void Clear() {
    blocks.clear();
    index.clear();
    ranks.clear();
    loose_index.clear();
    loose_ranks.clear();
  }
};
struct MotionState {
  MotionFrame frames[2];
  uint32_t cur = 0;
  uint64_t matched = 0, unmatched = 0;  // this frame's scene draws
  // fnxf_xiii (FSR 3): the projection of the frame's first scene draw (c165.x, c166.y, c167.z, c168.z).
  float proj[4] = {};
  bool proj_set = false;
  std::chrono::steady_clock::time_point last_report{};
  // fnxf_xiii (2026-11-07, fnxf_xiii_diag_fsr3_frames): the jitter this frame's jittered scene draws used.
  uint64_t jittered = 0;
  float drawn_jitter[2] = {};
  bool jitter_mixed = false;
  // fnxf_xiii (2026-11-07, fnxf_xiii_diag_camera_steps): one big scene draw followed from frame to frame (its key), how
  // much its registers 8-11 (the game's world-view-projection rows) changed since last frame (sum of |differences|),
  // and the frame starts' times: a camera turning at a steady speed changes them by the same amount every frame.
  uint64_t step_key = 0;
  float step = -1.0f;
  std::chrono::steady_clock::time_point step_frame_time{};
  std::string step_line;
  uint32_t step_frames = 0;
};
MotionState g_motion;

int32_t MotionMode() {
  // FSR 3 (render/fsr3.cpp) needs them: on with it.
  static const int32_t mode = std::max(REXCVAR_GET(fnxf_xiii_motion_vectors), Fsr3On() ? 1 : 0);
  return IsDeko3D() ? mode : 0;
}

void MotionFrameStartLocked() {
  if (!MotionMode())
    return;
  MotionState &m = g_motion;
  const auto now = std::chrono::steady_clock::now();
  if (now - m.last_report >= std::chrono::seconds(10)) {
    if (m.last_report != std::chrono::steady_clock::time_point{})
      RGPU_INFO("[motion] last frame: {} scene draws with last frame's constants, {} without", m.matched, m.unmatched);
    m.last_report = now;
  }
  m.cur ^= 1;
  m.frames[m.cur].Clear();
  ++g_motion_target.frame;
  m.proj_set = false;
  Fsr3FrameStartLocked();  // the next jitter
  m.matched = m.unmatched = 0;
  m.jittered = 0;
  m.jitter_mixed = false;
  static const bool steps = REXCVAR_GET(fnxf_xiii_diag_camera_steps);
  if (steps) {
    const double frame_ms = m.step_frame_time == std::chrono::steady_clock::time_point{}
                                ? 0.0
                                : std::chrono::duration<double, std::milli>(now - m.step_frame_time).count();
    m.step_frame_time = now;
    m.step_line += fmt::format(" {:.0f}/{:.3f}", frame_ms, m.step);
    if (m.step < 0.0f)
      m.step_key = 0;  // not seen this frame: another one next frame
    m.step = -1.0f;
    if (++m.step_frames >= 60) {
      RGPU_INFO("[camera steps] frame ms / change:{}", m.step_line);
      m.step_line.clear();
      m.step_frames = 0;
    }
  }
}

inline bool RegisterUsed(const u32 used[8], u32 r) { return (used[r / 32] >> (r % 32)) & 1u; }

// The draw's motion words (sc.motion) and, for a scene draw, the previous constants bound at UBO 4 on the list.
void MotionPrepareLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord *vs,
                         const ShaderRecord *ps, const VertexDeclarationRecord &declaration,
                         const FloatConstants &vs_constants, const u32 *vs_used, const Targets &targets,
                         SharedConstants &sc) {
  const int32_t mode = MotionMode();
  if (!mode || !vs || !ps || !targets.color_count || !targets.depth || targets.stand_in)
    return;
  // fnxf_xiii (2026-11-08, FSR 3.1 before the post-processing): after FSR 3.1 ran on the scene this frame, what is drawn with the
  // camera (the chapter title's 3D text at a cutscene's end) is in the upscaled picture: not jittered (nothing takes the
  // jitter out any more), no motion.
  if (g_early_fsr.ran_frame == g_motion_target.frame)
    return;
  for (const u32 r : {8u, 9u, 10u, 11u, 16u, 17u, 18u, 165u, 166u, 167u, 168u})
    if (!RegisterUsed(vs_used, r))
      return;
  MotionState &m = g_motion;
  // The key: count, shaders, vertex stream addresses; then its rank among this frame's draws with the same.
  // fnxf_xiii (2026-11-07): also where the draw starts in its buffers (index buffer, first index, base vertex; or the
  // first vertex): parts of one mesh drawn with the same shaders and streams were told apart only by their rank, so when
  // one of them was culled or came into view while the camera turned, every later one took another part's last
  // constants: motion vectors of a whole object far off for a frame (FSR 3's image hitched).
  uint64_t parts[5 + kMaxVertexStreams] = {vs->hash, ps->hash ^ (uint64_t(call.count) << 32)};
  u32 n = 2;
  // The loose key's parts: shaders, count, where it starts (no buffer addresses).
  uint64_t loose_parts[3] = {vs->hash, ps->hash ^ (uint64_t(call.count) << 32),
                             call.indexed ? ((uint64_t(1) << 63) | (uint64_t(call.start_index) << 32) |
                                             uint32_t(call.base_vertex))
                                          : uint64_t(call.start_vertex)};
  if (call.indexed) {
    parts[n++] = (uint64_t(1) << 63) | device.U32(d3d::kDeviceOffsetIndexBuffer);
    parts[n++] = (uint64_t(call.start_index) << 32) | uint32_t(call.base_vertex);
  } else {
    parts[n++] = call.start_vertex;
  }
  for (u32 stream = 0; stream < kMaxVertexStreams; ++stream) {
    if (!declaration.layout.streams[stream].used)
      continue;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    parts[n++] = (uint64_t(stream) << 32) | (device.VertexFetchWord(fetch_index, 0) & ~3u);
  }
  const uint64_t base_key = XXH3_64bits(parts, n * sizeof(uint64_t));
  MotionFrame &cur = m.frames[m.cur];
  const uint32_t rank = cur.ranks[base_key]++;
  const uint64_t key = XXH3_64bits_withSeed(&base_key, sizeof(base_key), rank + 1);
  const uint64_t loose_base = XXH3_64bits(loose_parts, sizeof(loose_parts));
  const uint32_t loose_rank = cur.loose_ranks[loose_base]++;
  const uint64_t loose_key = XXH3_64bits_withSeed(&loose_base, sizeof(loose_base), loose_rank + 1);
  // This frame's block: the registers as the shader sees them.
  const uint32_t at = uint32_t(cur.blocks.size());
  cur.blocks.emplace_back();
  u8 *now = cur.blocks.back().bytes;
  for (u32 r = 0; r < 256; ++r) {
    u32 lanes[4];
    std::memcpy(lanes, vs_constants.Register(r), 16);
    for (u32 &lane : lanes)
      lane = std::byteswap(lane);
    std::memcpy(now + r * 16, lanes, 16);
  }
  std::memset(now + 256 * 16, 0, kFloatConstantsUboBytes - 256 * 16);
  if (const ShaderCacheEntry *e = vs->entry; e && e->literalCount) {
    for (u32 i = 0; i < e->literalCount; ++i) {
      const ShaderLiteral &l = g_shaderLiterals[e->literalOffset + i];
      if (l.reg < 256)
        std::memcpy(now + l.reg * 16, l.value, 16);
    }
  }
  cur.index.emplace(key, at);
  cur.loose_index.emplace(loose_key, at);
  // Last frame's block of the same draw (by the full key, else by the loose one), or this one (no motion).
  const MotionFrame &prev = m.frames[m.cur ^ 1];
  auto it = prev.index.find(key);
  bool found = it != prev.index.end();
  if (!found) {
    it = prev.loose_index.find(loose_key);
    found = it != prev.loose_index.end();
  }
  const u8 *before = found ? prev.blocks[it->second].bytes : now;
  (found ? m.matched : m.unmatched)++;
  static const bool steps = REXCVAR_GET(fnxf_xiii_diag_camera_steps);
  if (steps && found && (m.step_key == key || (!m.step_key && call.count >= 3000))) {
    m.step_key = key;
    float sum = 0.0f;
    for (u32 i = 8 * 4; i < 12 * 4; ++i) {
      float a, b;
      std::memcpy(&a, now + i * 4, 4);
      std::memcpy(&b, before + i * 4, 4);
      sum += std::fabs(a - b);
    }
    m.step = sum;
  }
  constexpr u32 kBytes = (kFloatConstantsUboBytes + 255u) & ~255u;  // deko3d's uniform sizes are 256-byte multiples
  UploadAllocation alloc = Video::UploadsLocked().AllocateWithRoom(kBytes, kBytes, 256);
  if (!alloc)
    return;
  std::memcpy(alloc.cpu, before, kFloatConstantsUboBytes);
#if defined(PLUME_DEKO3D)
  plume::Deko3DBindUniform(State().command_list, 4, alloc.gpu_address, kBytes);
#endif
  sc.motion = 1u | (mode == 2 ? 2u : 0u);
  g_early_fsr.resolves.clear();  // fnxf_xiii (2026-11-08): resolves before a scene draw are not the scene's last
  // FSR 3: the scene jittered, and its projection for FSR's depth conversion.
  // Only single-sampled: FSR does not run on a multisampled scene (anti-aliasing on), whose picture would shake.
  if (Fsr3On() && targets.colors[0]->samples == plume::RenderSampleCount::COUNT_1) {
    sc.motion |= 4u;
    Fsr3JitterNdc(sc.jitter);
    if (g_dump.active && g_dump.file)  // fnxf_xiii (2026-11-08, diagnostic): which draws are jittered
      std::fprintf(g_dump.file, "    jittered %g %g (draw %u)\n", sc.jitter[0], sc.jitter[1], g_dump.draw);
    if (m.jittered++ == 0) {
      m.drawn_jitter[0] = sc.jitter[0];
      m.drawn_jitter[1] = sc.jitter[1];
    } else if (sc.jitter[0] != m.drawn_jitter[0] || sc.jitter[1] != m.drawn_jitter[1]) {
      m.jitter_mixed = true;
    }
  }
  if (!m.proj_set) {
    auto reg = [&](u32 r, u32 lane) {
      float v;
      std::memcpy(&v, now + r * 16 + lane * 4, 4);
      return v;
    };
    m.proj[0] = reg(165, 0);
    m.proj[1] = reg(166, 1);
    m.proj[2] = reg(167, 2);
    m.proj[3] = reg(168, 2);
    m.proj_set = true;
  }
}

// fnxf_xiii (2026-11-06, FSR 3): what this frame's scene left for FSR (render/fsr3.h).
Fsr3SceneInputs MotionSceneInputsLocked() {
  Fsr3SceneInputs in;
  const MotionTargetState &t = g_motion_target;
  const MotionState &m = g_motion;
  if (t.cleared_frame != t.frame || !t.texture)
    return in;  // no motion target written this frame
  in.motion = t.texture.get();
  in.depth = t.scene_depth;
  in.max_width = t.width;
  in.max_height = t.height;
  in.width = t.width;
  in.height = t.height;
  // fnxf_xiii (2026-11-07, dynamic resolution): the scene drew only its part of the targets.
  if (t.scene && t.scene->scale.drs) {
    in.width = std::min(DrsDrawn(t.width), t.width);
    in.height = std::min(DrsDrawn(t.height), t.height);
  }
  in.single_sample = t.samples == plume::RenderSampleCount::COUNT_1 && t.scene_depth &&
                     t.scene_depth->samples == plume::RenderSampleCount::COUNT_1;
  in.proj_x = m.proj[0];
  in.proj_y = m.proj[1];
  in.proj_a = m.proj[2];
  in.proj_b = m.proj[3];
  in.cut = m.matched + m.unmatched >= 16 && m.unmatched * 2 > m.matched + m.unmatched;
  in.frame = t.frame;
  in.matched = m.matched;
  in.unmatched = m.unmatched;
  in.jittered = m.jittered;
  in.drawn_jitter[0] = m.drawn_jitter[0];
  in.drawn_jitter[1] = m.drawn_jitter[1];
  in.jitter_mixed = m.jitter_mixed;
  return in;
}

void EarlyFsrBeforePostLocked(const GuestDevice &device);  // fnxf_xiii (2026-11-08), below

// fnxf_xiii (2026-11-14, fnxf_xiii_repeat_draw_fast; FF XIII frame
// dumps: 5-34 %, the constants that change are mostly c0-c3, c8-c15). A draw that repeats the draw before it (same
// mesh, shaders, textures and state) with other float constants needs none of DrawStateLocked's work but its float
// constants and the draw call: what the last plain draw recorded is still bound. A draw is taken as a repeat only
// when: the chain hint says it follows that draw directly with no device chunk changed (only float constants), its
// vertex group 0 was written (c0-c3: the same buffer drawn again with nothing changed could be a buffer rewritten in
// place), its DrawCall is the same, and no other item ran on the render thread in between
// (render_thread::ItemSerial: draws replayed from one command buffer are one item and so never repeats of each
// other). Armed by a plain draw only: pushed constants, no memexport, tessellation, UP data, point sprites, profile
// spans, draw log, frame dump, skin-once or promotion, and no motion vectors (MotionPrepareLocked gives each draw its
// own previous constants).
#if defined(PLUME_DEKO3D)
struct RepeatDraw {
  bool armed = false;
  uint64_t seq = 0, serial = 0, served = 0;
  DrawCall call;
  plume::RenderCommandList *cmd = nullptr;
  const u32 *vs_used = nullptr, *ps_used = nullptr;
  const ShaderCacheEntry *vs_entry = nullptr, *ps_entry = nullptr;
  bool indexed = false, depth_counts = false;
  u32 count = 0, instances = 0, color_count = 0;
  HostSurface *colors[d3d::kMaxRenderTargets] = {};
  HostSurface *depth = nullptr;
};
RepeatDraw g_repeat;
#endif

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): the game issues the crystal's glare sprite every frame, yet Renut draws
// it only on some: where a glare draw (6 vertices, triangle list, a 256x256 k_8_8_8_8 texture in slot 0) leaves
// DrawStateLocked before it is drawn: the last checkpoint passed ([glaredrop]).
int g_glare_cp = -1;
struct GlareDropScope {
  bool glare = false;
  ~GlareDropScope() {
    if (!glare || g_glare_cp == 99)
      return;
    static uint32_t counts[16] = {}, logged = 0;
    ++counts[g_glare_cp & 15];
    if (logged++ < 60)
      RGPU_INFO("[glaredrop] frame {}: a glare draw left DrawStateLocked after checkpoint {} (drops so far at 1-6: {} "
                "{} {} {} {} {})", TextureFrameForLog(), g_glare_cp, counts[1], counts[2], counts[3], counts[4],
                counts[5], counts[6]);
  }
};

bool DrawStateLocked(const u8 *device_state, const DrawCall &call, const ReplayConstants *replay) {
#if defined(PLUME_DEKO3D)
  if (g_repeat.armed) {  // fnxf_xiii (2026-11-14, fnxf_xiii_repeat_draw_fast)
    RepeatDraw &r = g_repeat;
    const ChainHint &hint = g_chain_hint;
    const uint64_t serial = render_thread::ItemSerial();
    if (replay && replay->vs_dirty && hint.seq == r.seq + 1 && !(hint.changed[0] | hint.changed[1]) &&
        serial == r.serial + 1 && (*replay->vs_dirty >> 63) != 0 && State().command_list == r.cmd &&
        call.primitive == r.call.primitive && call.indexed == r.call.indexed &&
        call.start_vertex == r.call.start_vertex && call.count == r.call.count &&
        call.base_vertex == r.call.base_vertex && call.start_index == r.call.start_index && !call.tessellation &&
        !call.up_vertices && !call.up_indices) {
      const FloatConstants vs_constants = DrawFloatConstants(device_state, replay, false);
      const FloatConstants ps_constants = DrawFloatConstants(device_state, replay, true);
      PushFloatConstants(r.cmd, vs_constants, r.vs_used, g_bound.pushed_vs, r.vs_entry, g_pushed_buffers.address);
      PushFloatConstants(r.cmd, ps_constants, r.ps_used, g_bound.pushed_ps, r.ps_entry,
                         g_pushed_buffers.address + kPushedFloatStride);
      g_bound.vs_constants.replay = 0;
      g_bound.ps_constants.replay = 0;
      fnxf_xiii::renut_gpu::CountPushedConstants(1, 2);
      for (u32 i = 0; i < r.color_count; ++i)
        ++r.colors[i]->writes;
      if (r.depth_counts)
        ++r.depth->writes;
      ++g_trace_draws;
      g_fnxf_xiii_draws.fetch_add(1, std::memory_order_relaxed);
      Video::NoteDrawLocked();
      if (r.indexed)
        r.cmd->drawIndexedInstanced(r.count, r.instances, 0, 0, 0);
      else
        r.cmd->drawInstanced(r.count, r.instances, 0, 0);
      r.seq = hint.seq;
      r.serial = serial;
      if ((++r.served & 0x3FFFF) == 0 && REXCVAR_GET(log_diagnostics))
        RGPU_INFO("[repeat] {} draws served as repeats of the draw before them so far (fnxf_xiii_repeat_draw_fast)",
                  r.served);
      return true;
    }
    r.armed = false;
  }
#endif
  const rtw::Scope rtw_draw(rtw::kDraw);  // fnxf_xiii (2026-11-05, [rtwork])
  const rtw::Scope rtw_replay_draw(rtw::g_in_replay ? rtw::kReplayDraws : rtw::kUnused);
  struct InDraw {
    InDraw() { rtw::g_in_draw = true; }
    ~InDraw() { rtw::g_in_draw = false; }
  } rtw_in_draw;
  FrameDumpFlushLocked();  // fnxf_xiii: the previous draw's textures, read back before this draw records anything
  ++g_trace_draws;
  guest_gpu::NewDrawLocked();  // fnxf_xiii (2026-10-30): the GPU's L2 invalidated once a draw at most, before its guest reads
  g_fnxf_xiii_draws.fetch_add(1, std::memory_order_relaxed);
  auto &s = State();
  const GuestDevice device(device_state);
  if (!device.valid())
    return false;
  GlareDropScope glare_drop;  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare)
  {
    static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
    if (on && call.count == 6 && u32(call.primitive) == 4) {
      const TextureFetch f = DeviceTextureFetch(device.dev(), 0);
      u32 w = 0, h = 0, d = 0;
      if (f.IsTexture()) {
        f.Size(w, h, d);
        glare_drop.glare = u32(f.Format()) == 20 && w == 256 && h == 256;
      }
    }
    g_glare_cp = 1;
  }

  const u32 vs_va = device.U32(d3d::kDeviceOffsetVertexShader);
  auto *vs = rtw::Timed(rtw::kDrawLookup, [&] { return FindShaderLocked(vs_va, RecordKind::VertexShader); });
  const u32 declaration_va = device.U32(d3d::kDeviceOffsetVertexDeclaration);
  auto *ps = rtw::Timed(rtw::kDrawLookup,  // fnxf_xiii (2026-11-05, seen18, [rtwork])
                        [&] { return FindShaderLocked(device.U32(d3d::kDeviceOffsetPixelShader), RecordKind::PixelShader); });
  // Without a declaration set, the shader fetches as D3DVertexShader_Bind
  // bound it, or else as its own vfetches describe (bundle meshes).
  const VertexDeclarationRecord *declaration = nullptr;
  const u32 *shader_strides = nullptr;  // see GetStreamSource
  g_decl_va = declaration_va;  // fnxf_xiii (2026-11-08, shadows)
  g_decl_source = declaration_va ? 1 : vs && vs->bound_declaration ? 2 : 4;
  g_decl_vs = vs;
  if (declaration_va) {
    if (declaration_va != g_last_declaration_va) {
      g_last_declaration =
          FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
      g_last_declaration_va = g_last_declaration ? declaration_va : 0;
    }
    declaration = g_last_declaration_va == declaration_va ? g_last_declaration : nullptr;
    // fnxf_xiii: the game's D3D builds its declarations itself (CreateVertexDeclaration, the in-place initializer the
    // game calls, SetFVF's declaration inside the device): read them from the guest object.
    if (!declaration)
      declaration = rtw::Timed(rtw::kDeclLookup, [&] { return NativeDeclarationLocked(declaration_va); });
    // fnxf_xiii (2026-11-08, real-time shadows): this vertex buffer's declaration and strides (g_vb_declarations).
    if (declaration && !call.up_vertices) {
      if (const uint64_t key = VertexBufferKey(device)) {
        if (g_vb_declarations.size() > 16384)
          g_vb_declarations.clear();
        VertexBufferDeclaration &vb = g_vb_declarations[key];
        vb.declaration_va = declaration_va;
        for (u32 stream = 0; stream < d3d::kMaxStreams; ++stream)
          vb.strides[stream] = u32(device.U8(d3d::kDeviceOffsetStreamStrides + stream)) * 4;
      }
    }
  } else if (vs && vs->bound_declaration) {
    declaration = vs->bound_declaration.get();
    shader_strides = vs->bound_strides;
  } else if (const VertexDeclarationRecord *drawn = [&]() -> const VertexDeclarationRecord * {
               // fnxf_xiii (2026-11-08, real-time shadows): as this vertex buffer was drawn with a declaration.
               if (!vs || call.up_vertices || !vs->entry || !vs->entry->vertexInputCount)
                 return nullptr;
               const uint64_t key = VertexBufferKey(device);
               const auto it = key ? g_vb_declarations.find(key) : g_vb_declarations.end();
               if (it == g_vb_declarations.end())
                 return nullptr;
               const VertexDeclarationRecord *record = NativeDeclarationLocked(it->second.declaration_va);
               if (record)
                 shader_strides = it->second.strides;
               return record;
             }()) {
    declaration = drawn;
    g_decl_va = 0;
    g_decl_source = 5;
  } else if (const VertexDeclarationRecord *fetch = vs ? ShaderFetchDeclarationLocked(*vs) : nullptr) {
    declaration = fetch;
    shader_strides = vs->fetch_strides;
    g_decl_source = 3;
  } else {
    declaration = &NoDeclaration();
  }
  // fnxf_xiii (2026-10-19, render-thread proposal 16): the records this draw reads next, fetched while the code between
  // runs (the shader entries' first lines, the declaration): first touches were most of the draw's sampled loads.
  if (vs && vs->entry) {
    __builtin_prefetch(vs->entry);
    __builtin_prefetch(reinterpret_cast<const char *>(vs->entry) + 64);
  }
  if (ps && ps->entry) {
    __builtin_prefetch(ps->entry);
    __builtin_prefetch(reinterpret_cast<const char *>(ps->entry) + 64);
  }
  if (declaration)
    __builtin_prefetch(declaration);
  if (!vs || !declaration) {
    g_fnxf_xiii_draws_dropped.fetch_add(1, std::memory_order_relaxed);
    static uint32_t logged = 0;
    if (LogLimited(logged, 16)) {
      const u32 va = vs ? declaration_va : vs_va;
      const auto *words = GuestPtr<const be_u32>(va);
      RGPU_WARN("Draw dropped: {} {:08X} is not one the renderer knows (words {:08X} {:08X})",
                vs ? "vertex declaration" : "vertex shader", va, words ? u32(words[0]) : 0,
                words ? u32(words[1]) : 0);
    }
    return false;
  }
  // Earlier memexport results reach guest memory before this draw reads them
  // (WaitForExportsLocked may submit the list: before anything is recorded).
  const FloatConstants vs_constants = DrawFloatConstants(device_state, replay, false);
  const FloatConstants ps_constants = DrawFloatConstants(device_state, replay, true);
  // fnxf_xiii (2026-10-19, render-thread proposal 4): the draw's memexport streams, gathered once here (they were
  // gathered for each pass of the loop below and again for the draw: the same constants each time).
  MemExportStream exports[kMaxMemExportStreams];
  u32 export_count = 0;
  GatherMemExportStreams(vs->entry, vs_constants, exports, export_count);
  if (ps)
    GatherMemExportStreams(ps->entry, ps_constants, exports, export_count);
  // fnxf_xiii (diagnostic): every memexport destination once, and the draw's shape.
  for (u32 i = 0; REXCVAR_GET(fnxf_xiii_diag_log) && i < export_count; ++i) {
    static std::unordered_set<uint64_t> seen;
    if (seen.insert(uint64_t(exports[i].base) << 32 | exports[i].size).second && seen.size() < 2000)
      RGPU_INFO("[export] stream base {:08X} size {:X} format {} endian {} | vs {:016X} ps {:016X} prim {} count {} "
                "| colour {:08X} depth {:08X}",
                exports[i].base, exports[i].size, exports[i].format, exports[i].endian, vs->hash, ps ? ps->hash : 0,
                u32(call.primitive), call.count, device.U32(d3d::kDeviceOffsetRenderTargets),
                device.U32(d3d::kDeviceOffsetDepthStencil));
  }
  // fnxf_xiii (2026-10-27, fnxf_xiii_renut_diag_stream_dump_vs): the draw's export streams and its first two float
  // constants as the guest has them (the game's copy kernel: c0 the export address, c1 the index offsets).
  {
    static const uint64_t want = [] {
      const std::string v = REXCVAR_GET(fnxf_xiii_renut_diag_stream_dump_vs);
      return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
    }();
    static uint32_t logged = 0;
    if (want && vs->hash == want && export_count && logged++ < 24) {
      std::string words;
      for (u32 r = 0; r < 2; ++r) {
        const u8 *reg = vs_constants.Register(r);
        for (u32 k = 0; k < 4; ++k) {
          u32 w;
          std::memcpy(&w, reg + k * 4, 4);
          words += fmt::format(" {:08X}", w);
        }
        words += r == 0 ? " |" : "";
      }
      std::string ex;
      for (u32 i = 0; i < export_count; ++i)
        ex += fmt::format(" [{:08X} size {:X} format {} endian {} num {}]", exports[i].base, exports[i].size,
                          exports[i].format, exports[i].endian, exports[i].num_format);
      FNXF_XIII_BURST_LOG(RGPU_INFO, "[copykernel] draw prim {} count {} indexed {}: exports{}; c0 c1 (guest words):{}", u32(call.primitive),
                call.count, call.indexed ? 1 : 0, ex, words);
    }
  }
  // Waiting for an older slot's results leaves the current slot's, which the
  // draw may need as well: another pass checks (none are left after it).
  while (AnyPendingExports()) {
    u32 slots = 0;
    const ExportRead read = DrawNeedsExportFlush(device, call, *vs, ps, *declaration,
                                                 shader_strides, exports, export_count, slots);
    if (read == ExportRead::None)
      break;
    // fnxf_xiii (2026-10-21): with the time it took (the game: a few per area load, its texture kernels).
    const auto wait_start = std::chrono::steady_clock::now();
    rtw::Timed(rtw::kExportWait, [&] { WaitForExportsLocked(slots); });
    static uint32_t wait_logged = 0;
    if (LogLimited(wait_logged, 32)) {
      static constexpr const char *kReadNames[] = {"none",    "vertex stream", "vertex fetch", "indices",
                                                   "texture", "capacity"};
      FNXF_XIII_BURST_LOG(RGPU_WARN, "Draw waited {:.1f} ms for memexport results ({}; VS {:016X}, PS {:016X}{}): GPU {:.1f} ms, results to "
                "guest memory {:.1f} ms ({} windows, {} pages written, {} merged)",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count(),
                kReadNames[u32(read)], vs->hash, ps ? ps->hash : 0,
                (slots & (1u << s.frame)) ? ", this frame's: GPU round trip" : "", g_export_wait_gpu_ms,
                g_export_wait_complete_ms, g_export_wait_windows, g_export_wait_pages, g_export_wait_merged);
    }
  }
  if (!Video::EnsureCommandListLocked())
    return false;
  g_glare_cp = 2;
  SyncBound(s);

  plume::RenderShader *host_vs = rtw::Timed(rtw::kHostShader, [&] { return GetHostShader(vs); });
  plume::RenderShader *host_ps = ps ? rtw::Timed(rtw::kHostShader, [&] { return GetHostShader(ps); }) : nullptr;
  if (!host_vs || (ps && !host_ps))
    return false;  // missing from the shader cache (captured for the next build)
  g_glare_cp = 3;

  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): the previous chain draw's targets when the device's target words (bytes
  // 12440-12459 in N&B, 12816-12835 in SA2) did not change, no target was built, retired or rebound since (the surface epoch), and the targets'
  // descriptor words (which GetHostSurfaceLocked reads in guest memory) are the same.
  static const bool targets_reuse_on = REXCVAR_GET(fnxf_xiii_renut_chain_reuse);
  static const bool targets_verify = targets_reuse_on && REXCVAR_GET(fnxf_xiii_renut_chain_reuse_verify);
  const ChainHint &chain = g_chain_hint;
  // fnxf_xiii (2026-11-07): from the scene's copy on, its promoted 1280x720 targets are the 1.5x ones (graphics_options.h).
  // fnxf_xiii (2026-11-07, dynamic resolution): at a 1280x720 output too (the promoted ones are its, not dynamic).
  // fnxf_xiii (2026-11-08): the copy of the finished picture into the scene's colour target (PictureCopy): the same
  // shader also copies into the scene's target with its depth bound (draws 129-132 of a Steppe frame) and into the
  // post-processing's 1280x720 target (171, whose resolve is the picture), and either set the phase there: that
  // target (promoted) was drawn at full size, the picture without the dynamic resolution's code, and FSR 3.1 took the
  // drawn part of it (a zoomed picture, handheld 1280x720 with FSR 3.1 and dynamic resolution).
  // fnxf_xiii (2026-11-08, diagnostic): Fsr3DiagFlags 2 / 4, the per-tile restores of the scene's colour / depth skipped.
  if (const uint32_t df = Fsr3DiagFlags(); (df & 6u) && ps) {
    ClearRect tr{};
    if (((df & 2u) && ps->hash == kSceneCopyPs) || ((df & 4u) && ps->hash == 0xEC4C50672C27D4E7ull))
      if (PredicatedTile(device, tr))
        return true;
  }
#if defined(PLUME_DEKO3D)
  // fnxf_xiii (2026-11-08, diagnostic): Fsr3DiagFlags 16, a full GPU barrier before each per-tile colour restore.
  if (ps && ps->hash == kSceneCopyPs && (Fsr3DiagFlags() & 16u) && Video::EnsureCommandListLocked()) {
    ClearRect tr{};
    if (PredicatedTile(device, tr))
      plume::Deko3DComputeBarrier(s.command_list, false);
  }
#endif
  // fnxf_xiii (2026-11-08, FSR 3.1 before the post-processing): the post-processing's first draw: FSR 3.1 on the scene first.
  // fnxf_xiii (2026-11-08): the first draw into another target that samples one of the scene's resolves kept (the
  // post-processing's first that reads the scene). Before, the first draw into another target at all: at 1600x900 two
  // draws into the composite target come between the scene's opaque and transparent passes (draws 128-129 of a field
  // frame, sampling neither); FSR ran there, the transparent pass and the scene's last resolves came after it at the
  // dynamic resolution, and the post-processing read those: FSR's picture unused, the field soft.
  // fnxf_xiii (2026-11-09, diagnostic, fnxf_xiii_diag_early_fsr_stats): once a second, how many frames drew the scene and
  // how many of them ran no FSR before the post-processing (with the last outcome, g_early_reason), and the resolves
  // noted then; cheap enough not to move the game's timing (the per-draw log hid a glitch at 1280x720 dynamic).
  {
    static const bool stats = REXCVAR_GET(fnxf_xiii_diag_early_fsr_stats);
    if (stats) {
      static uint64_t frame = ~uint64_t(0);
      static bool scene = false, ran = false;
      static uint32_t frames = 0, scenes = 0, missed = 0, reasons[16] = {}, noted_missed = 0;
      static auto last = std::chrono::steady_clock::now();
      const uint64_t now_frame = TextureFrameLocked();
      if (now_frame != frame) {
        if (frame != ~uint64_t(0)) {
          ++frames;
          if (scene) {
            ++scenes;
            if (!ran) {
              ++missed;
              ++reasons[std::min(g_early_reason, 15)];
              noted_missed += g_early_noted;
            }
          }
        }
        g_early_reason = 0;
        g_early_noted = 0;
        frame = now_frame;
        scene = ran = false;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) {
          last = now;
          std::string r;
          for (int i = 0; i < 16; ++i)
            if (reasons[i])
              r += fmt::format(" {}:{}", i, reasons[i]);
          if (scenes)
            RGPU_INFO("[early] last second: {} frames, {} with the scene, {} without FSR before the post-processing "
                      "(reasons{}; {} resolves noted in them), output phase {}",
                      frames, scenes, missed, r.empty() ? " -" : r, noted_missed, g_output_phase);
          frames = scenes = missed = noted_missed = 0;
          std::fill(std::begin(reasons), std::end(reasons), 0u);
        }
      }
      scene |= g_motion_target.cleared_frame == g_motion_target.frame;
      ran |= g_early_fsr.ran_frame == g_motion_target.frame;
    }
  }
  if (!g_early_fsr.resolves.empty()) {
    const u32 mask = ps && ps->entry ? ps->entry->textureFetchMask : 0u;
    bool samples_scene = false;
    for (u32 r = 0; r < d3d::kTextureFetchSlots && !samples_scene; ++r) {
      if (!(mask & (1u << r)))
        continue;
      const u32 va = device.U32(d3d::kDeviceOffsetTextures + r * 4);
      for (const SceneResolve &sr : g_early_fsr.resolves)
        if (va && sr.dest_va == va) {
          samples_scene = true;
          break;
        }
    }
    if (samples_scene)
      EarlyFsrBeforePostLocked(device);
  }
  if (PictureCopy(device, ps) && (Output1080p() || DrsOn()) && fnxf_xiii::RenderHeight() == 720 &&
      SetOutputPhaseLocked(true))
    BumpSurfaceEpochLocked();
  // fnxf_xiii (2026-11-08, the flash when a game is loaded): in the output phase a 1280x720 target is promoted BEFORE
  // its first draw, not after it. Promoted after, the frame that first drew into it (the first one of the field after
  // a load: the HUD's targets, the fade's) drew into its 1x variant while the picture shown was the promoted one's:
  // the scene shown without the game's fade from black (a lit frame, or a half-drawn one) for a frame.
  if (OutputPromotionOn()) {
    if (fnxf_xiii::RenderHeight() == 720 && PictureCopy(device, ps))
      g_output_copied_frame = TextureFrameLocked();
    if (g_output_copied_frame == TextureFrameLocked()) {
      bool promoted = false;
      auto promote_ahead = [&](u32 va) {
        const d3d::SurfaceDesc *desc = va ? SurfaceDescLocked(va) : nullptr;
        if (!desc || ((u32(desc->SurfaceInfo) >> 16) & 3) ||
            !SceneSized(d3d::SurfaceWidth(desc->Size), d3d::SurfaceHeight(desc->Size)))
          return;
        const HostSurface *t = GetHostSurfaceLocked(va);
        const bool scene_variant = Output1080p() ? t && t->scale.code == 0 : t && t->scale.drs;
        if (scene_variant)
          promoted |= PromoteOutputTargetLocked(OutputTargetKey(*desc));
      };
      promote_ahead(device.U32(d3d::kDeviceOffsetRenderTargets));
      promote_ahead(device.U32(d3d::kDeviceOffsetDepthStencil));
      if (promoted)
        BumpSurfaceEpochLocked();
    }
  }
  Targets targets;
  bool targets_reused = false;
  if (targets_reuse_on && chain.seq && chain.seq == g_targets_reuse.seq + 1 && !ChainChanged(chain, d3d::kDeviceOffsetRenderTargets, d3d::kDeviceOffsetDepthStencil + 3) &&
      g_targets_reuse.epoch == SurfaceEpochLocked()) {
    targets_reused = true;
    for (u32 i = 0; targets_reused && i <= d3d::kMaxRenderTargets; ++i) {
      u32 words[4];
      SurfaceDescWords(g_targets_reuse.va[i], words);
      targets_reused = std::memcmp(words, g_targets_reuse.desc[i], sizeof(words)) == 0;
    }
  }
  if (targets_reused) {
    targets = g_targets_reuse.targets;
    if (targets_verify) {
      Targets made;
      if (!rtw::Timed(rtw::kTargets, [&] { return GatherTargets(device, made); }) || !SameTargets(made, targets)) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("fnxf_xiii_renut_chain_reuse_verify: the reused targets differ from those gathered");
        targets = made;
      }
    }
  } else {
    if (!rtw::Timed(rtw::kTargets, [&] { return GatherTargets(device, targets); })) {
      g_targets_reuse.seq = 0;
      // fnxf_xiii: a draw without targets that exports (the game's memexport kernels, vs_BE95EE803D59E654 between
      // D3DDevice_BeginExport and EndExport, a point list with no target bound) runs against a small target of its
      // own: what it writes to memory is the point, its pixels go nowhere the game reads. Before, it was dropped,
      // and what it writes (textures among it) stayed whatever the memory held.
      if (!export_count || !ExportTargetsLocked(targets)) {
        static uint32_t logged = 0;
        if (export_count && logged++ < 8)
          RGPU_WARN("[export] draw with {} export stream(s) dropped: no render target and no stand-in (base {:08X})",
                    export_count, exports[0].base);
        return false;
      }
      static uint32_t logged = 0;
      if (logged++ < 4)
        FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] draw without a render target: drawn against the stand-in target (base {:08X})",
                  exports[0].base);
    }
  }
  g_glare_cp = 4;
  if (targets_reuse_on) {
    g_targets_reuse.seq = chain.seq;
    g_targets_reuse.epoch = SurfaceEpochLocked();
    if (!targets_reused) {
      for (u32 i = 0; i <= d3d::kMaxRenderTargets; ++i) {
        g_targets_reuse.va[i] = device.U32(i < d3d::kMaxRenderTargets ? d3d::kDeviceOffsetRenderTargets + i * 4
                                                                       : d3d::kDeviceOffsetDepthStencil);
        SurfaceDescWords(g_targets_reuse.va[i], g_targets_reuse.desc[i]);
      }
      g_targets_reuse.targets = targets;
    }
  }
  // fnxf_xiii (2026-10-20, NOT exact, fnxf_xiii_renut_fur_shells = N > 1, the user's call): the fur family's meshes are drawn
  // once a shell, shell by shell (each shell draws every part of the body: RenderDoc of Ryujinx shows the same four
  // parts repeating); of each part's draws in a frame (the same shaders and index range) every Nth is drawn, the first
  // always (tools/nullps_shaders.txt: the game's fur and the dialogue portrait's).
  // fnxf_xiii_renut_portrait_fur_shells: the same for draws into a target of 128 x 128 or less (the dialogue portrait,
  // 96 x 96: 21 shells there cost ~2.5 ms of the heaviest frames, mostly vertex work).
  const bool portrait = targets.width <= 128 && targets.height <= 128;
  if (const uint32_t every = portrait ? fnxf_xiii::renut_gpu::PortraitFurShellEvery() : fnxf_xiii::renut_gpu::FurShellEvery();
      every > 1 && ps) {
    const uint64_t h = ps->hash;
    if (h == 0x2F36770769087F99ull || h == 0xC92BA80672FB9774ull || h == 0xB3C10F3D80EF61E2ull ||
        h == 0xD40EBF0336F89333ull) {
      struct ShellPart {
        uint64_t frame = 0;
        const void *vs = nullptr, *ps = nullptr;
        u32 start_index = 0, start_vertex = 0, count = 0;
        i32 base_vertex = 0;
        bool indexed = false;
        bool portrait = false;
        u32 n = 0;
      };
      static ShellPart parts[64];
      const uint64_t frame = TextureFrameLocked();
      ShellPart *part = nullptr, *free_part = nullptr;
      for (ShellPart &p : parts) {
        if (p.frame != frame) {
          if (!free_part)
            free_part = &p;
          continue;
        }
        if (p.vs == vs && p.ps == ps && p.start_index == call.start_index && p.start_vertex == call.start_vertex &&
            p.count == call.count && p.base_vertex == call.base_vertex && p.indexed == call.indexed &&
            p.portrait == portrait) {
          part = &p;
          break;
        }
      }
      bool draw = true;
      if (part) {
        draw = ++part->n % every == 0;
      } else if (free_part) {
        *free_part = {frame, vs, ps, call.start_index, call.start_vertex, call.count, call.base_vertex, call.indexed,
                      portrait, 0};
      }
      fnxf_xiii::renut_gpu::CountFurShell(draw);
      if (!draw)
        return true;
    }
  }

  // fnxf_xiii (fnxf_xiii_gpu_depth_only): before anything of the pixel shader is set up.
  if (ps && DrawsWithoutPixelShader(device, targets, *ps)) {
    ps = nullptr;
    host_ps = nullptr;
    fnxf_xiii::renut_gpu::CountDepthOnly();
  }

  // Everything that records copies or barriers comes before the framebuffer.
  SharedConstants sc;
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_shared_partial_clear): FillTextures writes all 128 texture and 32 sampler words (its
  // first loop, or the chain's copy of a previous draw's), so only what follows them is zeroed (848 of 1,360 bytes).
  static_assert(offsetof(SharedConstants, booleans) == sizeof(sc.texture_indices) + sizeof(sc.sampler_indices));
  static const bool partial_clear = REXCVAR_GET(fnxf_xiii_renut_shared_partial_clear);
  if (partial_clear)
    std::memset(&sc.booleans, 0, sizeof(sc) - offsetof(SharedConstants, booleans));
  else
    std::memset(&sc, 0, sizeof(sc));
  sc.flt_max = std::numeric_limits<float>::max();  // fnxf_xiii: 0x7F7FFFFF, the clamps' bound (shader_common.h)
  const u32 texture_mask =
      (vs->entry ? vs->entry->textureFetchMask : 0) | (ps && ps->entry ? ps->entry->textureFetchMask : 0);
  // fnxf_xiii (2026-10-31): the shaders' own fetch filters (fetch_filters.h), the pixel shader's over the vertex
  // shader's; 'filters_key' tells the chain reuse below whether they are the previous draw's.
  static const bool fetch_filters_on = REXCVAR_GET(fnxf_xiii_renut_fetch_filters);
  FetchFilterOverrides draw_filters;
  const FetchFilterOverrides *filters = nullptr;
  uint64_t filters_key = 0;
  if (fetch_filters_on && (!vs->fetch_filters.Empty() || (ps && !ps->fetch_filters.Empty()))) {
    draw_filters = vs->fetch_filters;
    if (ps)
      MergeFetchFilters(draw_filters, ps->fetch_filters);
    filters = &draw_filters;
    filters_key = (uint64_t(vs->self_va) << 32 | (ps ? ps->self_va : 0)) ^ vs->hash ^ (ps ? ps->hash * 3 : 0);
  }
  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): the draw's colour targets whose content a resolve destination holds,
  // before its textures are bound (a destination it samples may be one of them).
  g_colour_take_pending_count = 0;
  if (!g_colour_tracked.empty())
    ColourHandOverDrawStartLocked(device, call, vs, ps, targets, declaration, shader_strides, vs_constants);
  g_handover_targets = &targets;  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover)
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): the previous chain draw's texture and sampler words when FillTextures'
  // memo would answer every bound slot as it did then: the device's fetch constants and texture words unchanged
  // (bytes 1152-1775, and the texture words: 12544-12647 in N&B, 12920-13023 in SA2), the same slots, frame, texture and depth epochs and depth target, and each bound
  // texture's header words the same. The memo hit's work is done again for each slot (its transitions and counters).
  struct TexturesReuse {
    uint64_t seq = 0, frame = 0, epoch = 0, depth_epoch = 0;
    uint64_t filters_key = 0;  // fnxf_xiii (2026-10-31): the shaders' fetch filters it was filled with
    u32 mask = 0;
    const HostSurface *depth = nullptr;
    u32 header[32][6] = {};
    BindEffects fx[32];
    u32 indices[3][32] = {};
    u32 samplers[32] = {};
  };
  static TexturesReuse treuse;
  // fnxf_xiii (2026-11-07): g_mag_linear for a colour target drawn larger than the guest's (the key with it).
  g_mag_linear = targets.color_count && targets.colors[0] && targets.colors[0]->scale.code != 0;
  if (g_mag_linear)
    filters_key ^= 0x5CA1ED0000000001ull;
  bool textures_reused = targets_reuse_on && chain.seq && chain.seq == treuse.seq + 1 && treuse.mask == texture_mask &&
                         !ChainChanged(chain, 1152, 1775) && !ChainChanged(chain, d3d::kDeviceOffsetTextures, d3d::kDeviceOffsetTextures + 4 * d3d::kTextureFetchSlots - 1) &&
                         treuse.frame == TextureFrameLocked() && treuse.epoch == g_texture_epoch &&
                         treuse.depth_epoch == g_depth_epoch && treuse.depth == targets.depth &&
                         treuse.filters_key == filters_key;
  for (u32 r = 0; textures_reused && r < d3d::kTextureFetchSlots; ++r) {
    if (!(texture_mask & (1u << r)))
      continue;
    const u32 va = device.U32(d3d::kDeviceOffsetTextures + r * 4);
    const auto *header = va ? GuestPtr<const d3d::BaseTexture>(va) : nullptr;
    textures_reused = header && std::memcmp(treuse.header[r], &header->Format, sizeof(treuse.header[r])) == 0;
  }
  if (textures_reused) {
    for (u32 r = 0; r < 32; ++r) {
      sc.texture_indices[0][r] = treuse.indices[0][r];
      sc.texture_indices[1][r] = treuse.indices[1][r];
      sc.texture_indices[2][r] = treuse.indices[2][r];
      sc.sampler_indices[r] = treuse.samplers[r];
    }
    for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
      if (!(texture_mask & (1u << r)))
        continue;
      const BindEffects &fx = treuse.fx[r];
      for (HostTexture *tex : fx.transition)
        if (tex)
          TransitionTextureLocked(tex, plume::RenderTextureLayout::SHADER_READ);
      if (fx.handover_surface)
        TransitionSurfaceLocked(fx.handover_surface, plume::RenderTextureLayout::SHADER_READ);
      if (fx.handover_image)
        TransitionDepthImageLocked(*fx.handover_image, plume::RenderTextureLayout::SHADER_READ);
      if (fx.float_sampled)
        fnxf_xiii::renut_gpu::CountDepthFloatSampled();
      if (fx.words_viewed)
        fnxf_xiii::renut_gpu::CountDepthWords(true);
    }
    if (targets_verify) {
      SharedConstants made{};
      rtw::Timed(rtw::kTextures, [&] { FillTextures(device, texture_mask, made, nullptr, nullptr, filters); });
      if (std::memcmp(made.texture_indices, sc.texture_indices, sizeof(sc.texture_indices)) != 0 ||
          std::memcmp(made.sampler_indices, sc.sampler_indices, sizeof(sc.sampler_indices)) != 0) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("fnxf_xiii_renut_chain_reuse_verify: the reused texture words differ from those filled");
        std::memcpy(sc.texture_indices, made.texture_indices, sizeof(sc.texture_indices));
        std::memcpy(sc.sampler_indices, made.sampler_indices, sizeof(sc.sampler_indices));
      }
    }
  } else {
    bool all_memo = false;
    rtw::Timed(rtw::kTextures, [&] {
      FillTextures(device, texture_mask, sc, targets_reuse_on ? treuse.fx : nullptr,
                   targets_reuse_on ? &all_memo : nullptr, filters);
    });
    if (targets_reuse_on) {
      treuse.filters_key = filters_key;
      // Kept for the next draw only when every slot is one the memo keys (a header), as it then answers them all.
      treuse.seq = all_memo ? chain.seq : 0;
      treuse.mask = texture_mask;
      treuse.frame = TextureFrameLocked();
      treuse.epoch = g_texture_epoch;
      treuse.depth_epoch = g_depth_epoch;
      treuse.depth = targets.depth;
      for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
        if (!(texture_mask & (1u << r)))
          continue;
        const u32 va = device.U32(d3d::kDeviceOffsetTextures + r * 4);
        const auto *header = va ? GuestPtr<const d3d::BaseTexture>(va) : nullptr;
        if (header)
          std::memcpy(treuse.header[r], &header->Format, sizeof(treuse.header[r]));
      }
      for (u32 r = 0; r < 32; ++r) {
        treuse.indices[0][r] = sc.texture_indices[0][r];
        treuse.indices[1][r] = sc.texture_indices[1][r];
        treuse.indices[2][r] = sc.texture_indices[2][r];
        treuse.samplers[r] = sc.sampler_indices[r];
      }
    }
  }
  if (textures_reused && targets_reuse_on)
    treuse.seq = chain.seq;
  g_handover_targets = nullptr;
  // fnxf_xiii (2026-11-16, fnxf_xiii_deko3d_vertex_texture_barrier): a vertex shader that samples textures (the crystal
  // glare's flare) after GPU writes: all earlier work finished first (textures.cpp).
  if (vs->entry && vs->entry->textureFetchMask)
    VertexTextureBarrierLocked();
  FrameDumpDrawLocked(device, call, vs, ps, targets, texture_mask);  // fnxf_xiii (fnxf_xiii_frame_dump)

  const d3d::Device &dev = device.dev();
  const u8 *bools = reinterpret_cast<const u8 *>(&dev.m_Constants.VertexShaderB[0]);
  // fnxf_xiii (2026-10-19, render-thread proposal 22): SwapEndian(2, ...) (8in32) inline: eight byte swaps, vectorised.
  for (u32 i = 0; i < 8; ++i) {
    u32 word;
    std::memcpy(&word, bools + 4 * i, 4);
    sc.booleans[i] = __builtin_bswap32(word);
  }
  const u32 color_control = dev.m_ControlPacket.ColorControl;
  sc.alpha_test_func = (color_control & 8) ? (color_control & 7) : 7;
  sc.alpha_threshold = dev.m_ValuesPacket.AlphaRef;
  if (sc.alpha_test_func == 7 && ((color_control >> 4) & 1) && fnxf_xiii::renut_gpu::AlphaToMaskTestOn()) {
    // fnxf_xiii (fnxf_xiii_renut_alpha_to_mask_test, 2026-10-18): alpha to coverage on a single-sample target is an alpha
    // test at 0.5 (MakeKey leaves the pipeline's alpha to coverage off for it). NVIDIA GPUs dither alpha to coverage
    // without samples: the checker-patterned grass, fur and foliage edges of the console and Ryujinx runs.
    const HostSurface *first = targets.color_count ? targets.colors[0] : targets.depth;
    if (ps && first && first->samples == plume::RenderSampleCount::COUNT_1) {
      sc.alpha_test_func = 6;  // >=
      sc.alpha_threshold = 0.5f;
    }
  }
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): the previous chain draw's viewport when nothing ComputeViewport reads
  // changed: the window, values and control packets (bytes 10432-10599), the word at 0x29C0, the predication, the
  // targets' sizes and scale, the tiles and the replay's predication.
  ViewportState viewport;
  const bool viewport_reused =
      targets_reuse_on && chain.seq && chain.seq == g_viewport_reuse.seq + 1 && !ChainChanged(chain, 10432, 10599) &&
      !ChainChanged(chain, 0x29C0, 0x29C3) && !ChainChanged(chain, d3d::kDeviceOffsetPredication,
                                                          d3d::kDeviceOffsetPredication + 3) &&
      g_viewport_reuse.width == targets.width && g_viewport_reuse.height == targets.height &&
      g_viewport_reuse.guest_width == targets.guest_width && g_viewport_reuse.guest_height == targets.guest_height &&
      g_viewport_reuse.scale == targets.scale.Key() && g_viewport_reuse.drs == g_drs_factor &&
      g_viewport_reuse.tiles == g_tiles.size() &&
      g_viewport_reuse.predication == g_replay_predication;
  if (viewport_reused) {
    viewport = g_viewport_reuse.viewport;
    if (targets_verify) {
      const ViewportState made = ComputeViewport(device, targets);
      if (std::memcmp(&made, &viewport, sizeof(made)) != 0) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("fnxf_xiii_renut_chain_reuse_verify: the reused viewport differs from the one computed");
        viewport = made;
      }
    }
  } else {
    viewport = ComputeViewport(device, targets);
  }
  if (targets_reuse_on) {
    g_viewport_reuse.seq = chain.seq;
    g_viewport_reuse.width = targets.width;
    g_viewport_reuse.height = targets.height;
    g_viewport_reuse.guest_width = targets.guest_width;
    g_viewport_reuse.guest_height = targets.guest_height;
    g_viewport_reuse.scale = targets.scale.Key();
    g_viewport_reuse.drs = g_drs_factor;
    g_viewport_reuse.tiles = g_tiles.size();
    g_viewport_reuse.predication = g_replay_predication;
    g_viewport_reuse.viewport = viewport;
  }
  sc.screen_space = viewport.screen_space ? 1 : 0;
  std::memcpy(sc.screen_scale, viewport.screen_scale, sizeof(sc.screen_scale));
  std::memcpy(sc.screen_offset, viewport.screen_offset, sizeof(sc.screen_offset));
  sc.viewport_size[0] = viewport.guest_size[0];
  sc.viewport_size[1] = viewport.guest_size[1];
  NoteOverlayResolutionsLocked(targets.depth, targets.color_count != 0);  // fnxf_xiii (2026-10-24): SaltyNX's Depth
  // fnxf_xiii (2026-11-08): a draw of the 3D scene: dynamic resolution only in frames of 3D gameplay.
  // fnxf_xiii (2026-11-08, the pause screen): only geometry: the viewport transform on and depth tested and written. The
  // pause screen's (and the HUD's) 2D draws into the scene's targets with the depth bound counted too: the paused frames
  // were 3D gameplay, drawn at the level the battle had left (lower), then raised at once (load ~10%).
  if (targets.depth && targets.color_count && targets.colors[0] && targets.colors[0]->scale.drs)
    DrsNoteSceneDraw(!viewport.screen_space && (u32(device.dev().m_ControlPacket.DepthControl) & 6u) == 6u);
  sc.resolution_scale = targets.scale.DrawFactor();  // fnxf_xiii (2026-11-07): as drawn (dynamic resolution)
  sc.drs_step = DrsStep();  // fnxf_xiii (2026-11-07): the dynamic-resolution textures' levels (their codes)
  std::memcpy(sc.pixel_origin, viewport.pixel_origin, sizeof(sc.pixel_origin));  // Renut upstream 574b488
  if (call.primitive == kPointList) {
    // PA_SU_POINT_SIZE: height 0-15, width 16-31, half the size in 12.4 fixed
    // point (SetRenderState_PointSize 0x8264C6C0 stores the size * 8);
    // PA_SU_POINT_MINMAX: minimum 0-15, maximum 16-31, the size in 12.4
    // (SetRenderState_PointSizeMin 0x8264C720 stores the minimum * 16).
    const auto *tessellator = reinterpret_cast<const be_u32 *>(dev.m_TessellatorPacket);
    const u32 point_size = tessellator[0], point_min_max = tessellator[1];
    sc.point_state[0] = float(point_size >> 16) / 8.0f;
    sc.point_state[1] = float(point_size & 0xFFFF) / 8.0f;
    // fnxf_xiii (2026-10-18, the depth of field's bokeh sprites): the minimum and maximum are radii in 12.4 as well, as
    // the hardware (and Xenia, the emulated renderer: point_vertex_diameter_min/max = value * 2 / 16) reads them; / 16
    // halved the largest sprites.
    const float minmax_scale = fnxf_xiii::renut_gpu::PointMinMaxRadiusOn() ? 8.0f : 16.0f;
    sc.point_state[2] = float(point_min_max & 0xFFFF) / minmax_scale;
    sc.point_state[3] = float(point_min_max >> 16) / minmax_scale;
  }

  g_glare_cp = 5;
  Geometry geometry;
  if (call.tessellation ? !PrepareTessellation(device, call, geometry, sc.tessellation)
                        : !rtw::Timed(rtw::kGeometry, [&] { return PrepareGeometry(device, call, geometry); })) {
    return false;
  }
  g_glare_cp = 6;
  // The host draws the vertex range in use from vertex 0 (indices rebased to
  // it in PrepareGeometry); the guest's r0.x is the vertex index itself.
  if (!call.tessellation)
    sc.tessellation[3] = geometry.first_vertex;
  // fnxf_xiii (2026-10-27): the draw's export windows before its vertex streams are found: a stream inside them is read
  // from this slot's fresh window (guest memory's bytes, and the pages older pending windows' kernels wrote), not from
  // an older slot's window, whose other pages are a frame old (the game may have written them since: the minimap).
  u32 export_offsets[kMaxMemExportStreams] = {};
  if (export_count &&!PrepareMemExportLocked(s, exports, export_count, sc, export_offsets)) {
    export_count = 0;
    std::memset(sc.memexport_streams, 0, sizeof(sc.memexport_streams));
  }
  // fnxf_xiii (2026-10-27): the game's copy kernel as a buffer copy (CopyKernelAsCopyLocked); the draw is done then.
  if (export_count)  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_watch)
    LogCopyKernelLocked(device, call, *vs, shader_strides, *declaration, geometry, vs_constants, exports, export_count);
  if (export_count && CopyKernelAsCopyLocked(s, device, call, *vs, shader_strides, *declaration, geometry, vs_constants,
                                             exports, export_count, export_offsets))
    return true;
  GpuStreams gpu_streams;
  if (!rtw::Timed(rtw::kExportStreams, [&] {
        return ConvertExportedStreamsLocked(s, device, call, *vs, shader_strides, *declaration, geometry, gpu_streams);
      })) {
    return false;
  }
  {
    static constexpr u32 kAll[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};  // a shader without an entry reads them all
    FrameDumpStreamsLocked(device, *declaration, gpu_streams, vs_constants, vs->entry ? vs->entry->floatConstants : kAll);
    FrameDumpPixelConstantsLocked(ps, ps_constants);  // fnxf_xiii (2026-10-31)
    GlareNoteLocked(device, vs, vs_constants);  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare)
    g_glare_cp = 99;
  }  // fnxf_xiii (2026-10-28, fnxf_xiii_frame_dump)

  const u32 fetch_mask = (vs->entry ? vs->entry->vertexFetchMemoryMask : 0) |
                         rtw::Timed(rtw::kDescribeFetch, [&] {
                           return DescribeBoundFetches(device, call, *vs, *declaration, shader_strides, sc);
                         });
  {
    static const uint64_t want = [] {
      const std::string v = REXCVAR_GET(fnxf_xiii_renut_diag_stream_dump_vs);
      return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
    }();
    g_dump_fetch = want && vs->hash == want;
  }
  UploadAllocation fetch_alloc =
      rtw::Timed(rtw::kFetch, [&] { return UploadVertexFetchData(device, call, fetch_mask, sc); });
  if (g_dump_fetch) {
    static uint32_t logged = 0;
    if (logged++ < 24)
      RGPU_INFO("[fetchdump] draw: mask {:08X} fetch data {:X} bytes {} prim {} count {} tess {}", fetch_mask,
                sc.vertex_fetch_data, sc.vertex_fetch_bytes, call.primitive, call.count, call.tessellation);
  }
  g_dump_fetch = false;
  uint64_t fetch_window = 0;  // fnxf_xiii (2026-10-20, fnxf_xiii_deko3d_skin_once): the binding's GPU address
#if defined(PLUME_DEKO3D)
  // fnxf_xiii (deko3d renderer): the vertex shaders read the fetch data through a storage binding of its range alone,
  // indexed by the address's low word (shader_common.h, g_DkAddressWindow): the binding starts at the address rounded
  // down to 256 bytes, and the word becomes the offset from there. One binding over every buffer (runs 14-15) cost
  // Ryujinx ~12 s a frame: it brings a storage binding's whole range up to date with guest memory at each draw.
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_fetch_data_unread): a draw whose shaders fetch nothing by address (fetch_mask 0:
  // the vertex shader's memory fetches and its bound fetches) never reads the fetch data's address or size: they stay
  // 0 instead of this draw's new allocation, so the shared constants' line with them does not change at every draw,
  // and no binding is made.
  static const bool fetch_data_unread = REXCVAR_GET(fnxf_xiii_renut_fetch_data_unread);
  if (IsDeko3D() && fetch_mask == 0 && fetch_data_unread) {
    sc.vertex_fetch_data = 0;
    sc.vertex_fetch_bytes = 0;
  } else if (IsDeko3D() && sc.vertex_fetch_data) {
    const uint64_t address = sc.vertex_fetch_data;
    const uint64_t base = address & ~uint64_t(0xFF);
    plume::Deko3DBindFetchData(s.command_list, base, u32(address - base) + sc.vertex_fetch_bytes);
    sc.vertex_fetch_data = address - base;
    fetch_window = base;
    // fnxf_xiii (2026-10-20): the 16-byte multiples tools/deko3d_glsl_opt.py's aw4 pass relies on.
    if (DiagCountsOn()) {  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_lean_diagnostics)
      u32 bits = u32(sc.vertex_fetch_data) | sc.vertex_fetch_bytes;
      for (u32 m = fetch_mask; m; m &= m - 1)
        bits |= sc.vertex_fetch_slots[64 + std::countr_zero(m)] & ~3u;
      fnxf_xiii::renut_gpu::CountFetchAlignment((bits & 15u) == 0);
    }
  }
#endif
  static constexpr u32 kAllRegisters[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  static constexpr u32 kNoRegisters[8] = {};
  const u32 *vs_used = vs->entry ? vs->entry->floatConstants : kAllRegisters;
  const u32 *ps_used = !ps ? kNoRegisters : ps->entry ? ps->entry->floatConstants : kAllRegisters;
  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_push_constants): pushed, except for draws point_sprite_gs expands (it reads
  // SharedConstants through its address).
#if defined(PLUME_DEKO3D)
  const bool pushed = IsDeko3D() && fnxf_xiii::renut_gpu::PushConstantsOn() &&
                      !(call.primitive == kPointList && s.point_sprite_gs) && EnsurePushedBuffers();
#else
  constexpr bool pushed = false;
#endif
  UploadAllocation vs_alloc, ps_alloc;
  if (pushed) {
#if defined(PLUME_DEKO3D)
    plume::RenderCommandList *cmd = s.command_list;
    PushFloatConstants(cmd, vs_constants, vs_used, g_bound.pushed_vs, vs->entry, g_pushed_buffers.address);
    PushFloatConstants(cmd, ps_constants, ps_used, g_bound.pushed_ps, ps ? ps->entry : nullptr,
                       g_pushed_buffers.address + kPushedFloatStride);
    // The replay's dirty masks were cleared for the pushed models: the ring uploads' tracking of them no longer holds.
    g_bound.vs_constants.replay = 0;
    g_bound.ps_constants.replay = 0;
    fnxf_xiii::renut_gpu::CountPushedConstants(1, 2);
#endif
  } else {
    vs_alloc = UploadFloatConstants(vs_constants, vs_used, g_bound.vs_constants, vs->entry);
    ps_alloc = UploadFloatConstants(ps_constants, ps_used, g_bound.ps_constants, ps ? ps->entry : nullptr);
    // fnxf_xiii (2026-10-19): and the other way round: the uploads cleared the dirty masks the pushed models track.
    g_bound.pushed_vs.replay = 0;
    g_bound.pushed_ps.replay = 0;
  }
  plume::RenderBuffer *memexport = MemExportBufferLocked(s);
  if (memexport && UsesVulkanBindings())  // fnxf_xiii: and deko3d
    sc.memexport_address = memexport->getDeviceAddress();
#if defined(PLUME_DEKO3D)
  // fnxf_xiii (deko3d renderer): the deko3d shaders write memexport through a binding of the buffer itself.
  if (memexport && IsDeko3D()) {
    plume::Deko3DBindMemExport(s.command_list, memexport);
    // fnxf_xiii (2026-10-30, ZeroCopyOn): and the slot's page flags at storage binding 12 (shader_common_glsl.h
    // g_DkMemExportFlags; made by PrepareZeroCopyExportLocked before the draw's exports). The export buffer itself
    // stands in while there are none (nothing writes the binding without a window's flag bit 1).
    const plume::RenderBuffer *flags = g_zero_copy_flags[s.frame].get();
    plume::Deko3DBindStorage(s.command_list, 12, flags ? flags->getDeviceAddress() : memexport->getDeviceAddress(),
                             flags ? u32(kZeroCopyFlagBytes) : 256u);
  }
#endif
  MotionPrepareLocked(device, call, vs, ps, *declaration, vs_constants, vs_used, targets, sc);  // fnxf_xiii (2026-11-06)
  UploadAllocation sc_alloc;
  if (pushed) {
#if defined(PLUME_DEKO3D)
    // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): a chain draw whose texture words are the previous draw's, when that
    // draw pushed last (no push since, the same command list's buffer): its first 512 bytes are held already.
    static uint64_t texture_push_seq = 0, texture_push_count = 0;
    const bool texture_lines_held = textures_reused && !targets_verify && chain.seq &&
                                    texture_push_seq + 1 == chain.seq && texture_push_count == g_shared_pushes &&
                                    g_bound.pushed_shared_valid;
    // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_fetch_slots_skip): no fetches in this draw and none in the last push: both have
    // the fetch slots zero (sc is zeroed there; only UploadVertexFetchData writes them, for the fetch mask's bits).
    static const bool fetch_slots_skip = REXCVAR_GET(fnxf_xiii_renut_fetch_slots_skip);
    const bool fetch_slots_equal = fetch_slots_skip && fetch_mask == 0 && g_held_fetch_slots_zero &&
                                   g_bound.pushed_shared_valid;
    rtw::Timed(rtw::kConstants, [&] {
      PushSharedConstants(s.command_list, sc,
                          (texture_lines_held ? kTextureLines : 0u) | (fetch_slots_equal ? kFetchSlotLines : 0u));
    });
    g_held_fetch_slots_zero = fetch_mask == 0;  // the buffer holds this draw's now
    texture_push_seq = chain.seq;
    texture_push_count = g_shared_pushes;
#endif
    // The 10 s line's draw count is the uploads plus the reuses: a pushed draw counts as an upload (package 35 logged
    // about 2,800 draws where there were 250,000).
    fnxf_xiii::renut_gpu::CountSharedConstants(false);
  } else if (g_bound.shared_valid && fnxf_xiii::renut_gpu::SharedDedupeOn() &&
      std::memcmp(&g_bound.shared_bytes, &sc, sizeof(sc)) == 0) {
    sc_alloc = g_bound.shared_alloc;  // fnxf_xiii: the same bytes are in this list's last upload
    fnxf_xiii::renut_gpu::CountSharedConstants(true);
  } else {
    sc_alloc = Video::UploadsLocked().Upload(&sc, sizeof(sc), 256);
    if (sc_alloc) {
      g_bound.shared_alloc = sc_alloc;
      std::memcpy(&g_bound.shared_bytes, &sc, sizeof(sc));
      g_bound.shared_valid = true;
    }
    fnxf_xiii::renut_gpu::CountSharedConstants(false);
  }
  if (!fetch_alloc || (!pushed && (!vs_alloc || !ps_alloc || !sc_alloc)) || !memexport)
    return false;

  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): a chain draw takes the previous draw's key when nothing MakeKey reads
  // changed: the device's bool constants and render state packets (bytes 10112-10599: VertexShaderB / PixelShaderB,
  // the clip planes, the destination, window, values and control packets), the shaders' cache entries, the
  // declaration's layout, the primitive, the instancing, the alpha test, the targets (their surfaces and the surface
  // epoch, which every target built, retired or rebound bumps) and the in-run A/B half.
  struct KeyReuse {
    uint64_t seq = 0;
    const ShaderCacheEntry *vs_entry = nullptr, *ps_entry = nullptr;
    uint64_t layout_hash = 0, surface_epoch = 0;
    u32 primitive = 0, alpha_test_func = 0, color_count = 0;
    bool per_instance = false;
    int ab_off = 0;
    HostSurface *colors[d3d::kMaxRenderTargets] = {};
    HostSurface *depth = nullptr;
    PipelineKey key{};
  };
  static KeyReuse reuse;
  static const bool reuse_on = REXCVAR_GET(fnxf_xiii_renut_chain_reuse);
  static const bool reuse_verify = reuse_on && REXCVAR_GET(fnxf_xiii_renut_chain_reuse_verify);
  const ChainHint &hint = chain;
  const ShaderCacheEntry *const ps_entry = ps ? ps->entry : nullptr;
  const int ab_off = fnxf_xiii::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
  rtw::Span rtw_key(rtw::kPipeKey);  // fnxf_xiii (2026-11-05, seen18, [rtwork]): the pipeline key, to the lookup
  const uint64_t surface_epoch = SurfaceEpochLocked();
  bool reused = reuse_on && hint.seq && hint.seq == reuse.seq + 1 && !ChainChanged(hint, 10112, 10599) &&
                reuse.vs_entry == vs->entry && reuse.ps_entry == ps_entry &&
                reuse.layout_hash == declaration->layout_hash && reuse.primitive == call.primitive &&
                reuse.per_instance == geometry.per_instance && reuse.alpha_test_func == sc.alpha_test_func &&
                reuse.color_count == targets.color_count && reuse.depth == targets.depth &&
                reuse.surface_epoch == surface_epoch && reuse.ab_off == ab_off;
  for (u32 i = 0; reused && i < targets.color_count; ++i)
    reused = reuse.colors[i] == targets.colors[i];
  PipelineKey key;
  if (reused) {
    key = reuse.key;
    if (reuse_verify) {
      const PipelineKey made = MakeKey(device, vs, ps, declaration, targets, call.primitive, geometry.per_instance,
                                       sc.booleans, sc.alpha_test_func);
      if (std::memcmp(&made, &key, sizeof(key)) != 0) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("fnxf_xiii_renut_chain_reuse_verify: the reused pipeline key differs from the one made (VS {:016X})",
                    vs->hash);
        key = made;
      }
    }
  } else {
    key = MakeKey(device, vs, ps, declaration, targets, call.primitive, geometry.per_instance, sc.booleans,
                  sc.alpha_test_func);
  }
  if (reuse_on) {
    reuse.seq = hint.seq;
    reuse.vs_entry = vs->entry;
    reuse.ps_entry = ps_entry;
    reuse.layout_hash = declaration->layout_hash;
    reuse.surface_epoch = surface_epoch;
    reuse.primitive = call.primitive;
    reuse.alpha_test_func = sc.alpha_test_func;
    reuse.color_count = targets.color_count;
    reuse.per_instance = geometry.per_instance;
    reuse.ab_off = ab_off;
    for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i)
      reuse.colors[i] = i < targets.color_count ? targets.colors[i] : nullptr;
    reuse.depth = targets.depth;
    if (!reused)
      reuse.key = key;
  }
  // fnxf_xiii (2026-11-16): the motion vectors are a compile-time switch of the shaders: specialization constant
  // 914's bit 2 (reserved bit 10; render/pipeline.cpp gives the shaders the constant), set for a draw whose motion
  // word is (MotionPrepareLocked: a scene draw with FSR 3.1 on). After the reuse block: its keys are kept without the
  // bit.
  key.reserved = (key.reserved & ~0x700u) | (sc.motion ? 0x400u : 0u);
  if (DiagCountsOn()) {  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_lean_diagnostics)
    // fnxf_xiii (package 28, a census): draws that write no colour, nothing at all, or part of a target's channels.
    const u32 used_mask = key.color_count ? key.color_mask & ((1u << (4 * key.color_count)) - 1u) : 0u;
    const bool no_colour = used_mask == 0;
    const u32 dc = key.depth_control;
    const bool depth = key.depth_format != plume::RenderFormat::UNKNOWN;
    const bool depth_writes = depth && (dc & 2) && (dc & 4);
    const u32 stencil_write_mask = (key.stencil_ref_mask >> 16) & 0xFF;
    const bool stencil_ops = ((dc >> 11) & 0x1FF) != 0 || ((dc >> 20) & 0x1FF) != 0;
    const bool stencil_writes = depth && (dc & 1) && stencil_write_mask && stencil_ops;
    bool partial = false;
    for (u32 i = 0; i < key.color_count; ++i) {
      const u32 m = (key.color_mask >> (4 * i)) & 0xF;
      partial |= m != 0 && m != 0xF;
    }
    const bool exports = (vs->entry && vs->entry->memExportConstants[0]) || (ps && ps->entry && ps->entry->memExportConstants[0]);
    fnxf_xiii::renut_gpu::CountDrawKind(no_colour && depth, no_colour && depth && sc.alpha_test_func == 7 &&
                                    !key.alpha_to_coverage && ps != nullptr,
                                    no_colour && !depth_writes && !stencil_writes && !exports, partial);
  }
  // fnxf_xiii (2026-10-25, fnxf_xiii_renut_upscale_filter): the game's upscale draw (its 1024x576 picture to 1280x720) with
  // Renut's pixel shader (1: shaders/deko3d/lanczos2_ps.glsl, 2: upscale_l15_ps.glsl) instead of the game's; the vertex
  // shader, targets, viewport, blend and textures stay the draw's. The key gets reserved bit 3 or 4 (pipelines of their
  // own, never prebuilt: the prebuild lists clear it) and none of the game shader's specialisations (bools, alpha
  // test, blend skip, output trim: the shaders read none of them).
  // 2026-10-25: 1 (Lanczos-2), 2 (the game's filter) and 3 (FSR 1's EASU) are fixed to a 1024x576 source (the draw's c0, big-endian floats
  // 1024 and 576); the game's own shader draws any other.
  const bool upscale_576 = [&] {
    if (!ps || ps->hash != kGameUpscalePs)
      return false;
    u32 c0[2];
    std::memcpy(c0, ps_constants.Register(0), sizeof(c0));
    return __builtin_bswap32(c0[0]) == 0x44800000u && __builtin_bswap32(c0[1]) == 0x44100000u;
  }();
  // fnxf_xiii (2026-11-08): when FSR 3.1 ran before the post-processing this frame, the picture is FSR's output and the
  // draw a 1:1 copy of it (FSR on the finished picture here is gone: EarlyFsrOn).
  const bool early_fsr_ran = g_early_fsr.ran_frame == g_motion_target.frame;
  // fnxf_xiii (2026-11-07): Lanczos-2 and FSR 1's EASU take any source size (c0) now: used when the draw upscales (its
  // source smaller than the target as drawn: 1024x576 to 1280x720 or 1920x1080, 1600x900 to 1920x1080 docked on a
  // 1080p TV); a draw that keeps or lowers the size (1280x720 1:1, 1600x900 or 1920x1080 to 1280x720 in hand) gets the
  // bilinear one.
  const bool upscales = [&] {
    if (!ps || ps->hash != kGameUpscalePs || targets.color_count < 1 || !targets.colors[0] || early_fsr_ran)
      return false;
    // The picture is the scene's (fnxf_xiii_render_height, 16:9): the draw's c0 is not used for it, as a 1280x720
    // scene's still says 1024x576 (only the start-up parameters change: the game's constant stays; 2026-11-07 log).
    // The shaders take the size of the texture itself.
    const float src_w = float(DrsOn() ? DrsDrawn(fnxf_xiii::RenderHeight() * 16 / 9)
                                      : fnxf_xiii::RenderHeight() * 16 / 9);  // fnxf_xiii (2026-11-07): as drawn
    const HostSurface *t = targets.colors[0];
    const bool up = float(t->scale.Size(t->width)) > src_w * 1.05f;
    // fnxf_xiii (2026-11-08): logged when the choice changes (dynamic resolution moves it), 20 times at most.
    static int logged = 0, last = -1;
    if (logged < 20 && UpscaleFilter() != 4 && int(up) != last) {
      ++logged;
      last = int(up);
      RGPU_INFO("[upscale] the game's upscale: picture {} wide, target {} wide ({}x{} guest): {}", src_w,
                t->scale.Size(t->width), t->width, t->height, up ? "upscales" : "1:1 or smaller (bilinear)");
    }
    return up;
  }();
  // fnxf_xiii (2026-11-08): after FSR 3.1 before the post-processing the draw is 1:1: Renut's bilinear one (the game's
  // filter would take its taps at the 1600x900 / 1024x576 spacing of its c0).
  const int32_t up_filter = early_fsr_ran && ps && ps->hash == kGameUpscalePs                ? 4
                         : (UpscaleFilter() == 1 || UpscaleFilter() == 3) && !upscales ? 4
                         : UpscaleFilter() == 2 && (!upscale_576 || DrsOn())          ? 0
                                                                                      : UpscaleFilter();
  // fnxf_xiii (2026-11-07): a 1280x720 scene has no upscale draw in the field: its finished picture is copied into the
  // scene's colour target (pixel shader kSceneCopyPs; FSR 3.1's native path), which with a 1920x1080 output is the 1.5x
  // one: the copy upscales (the game's shader: bilinear). Lanczos-2 and FSR 1's EASU draw it then (texture 0, its
  // texture coordinate at location 0 as the upscale draw's); the other filters leave the game's copy.
  const bool scene_copy_up = ps && ps->hash == kSceneCopyPs && (UpscaleFilter() == 1 || UpscaleFilter() == 3) &&
                             g_early_fsr.ran_frame != g_motion_target.frame &&  // fnxf_xiii (2026-11-08): already upscaled
                             fnxf_xiii::RenderHeight() == 720 && targets.color_count == 1 && targets.colors[0] &&
                             (targets.colors[0]->scale.code != 0 || DrsOn()) && !targets.depth &&
                             // fnxf_xiii (2026-11-08): only where the copy enlarges (the 1.5x target, or the scene drawn
                             // lower by dynamic resolution); 1:1 the game's copy (the Upscaler: bilinear unless enlarged)
                             float(targets.colors[0]->scale.DrawnSize(targets.colors[0]->width)) >
                                 float(DrsOn() ? DrsDrawn(1280) : 1280u) * 1.05f;
  plume::RenderShader *const upscale_ps = scene_copy_up ? (UpscaleFilter() == 1 ? s.lanczos2_ps.get() : s.fsr1_easu_ps.get())
                                          : !ps || ps->hash != kGameUpscalePs ? nullptr
                                          : up_filter == 1                       ? s.lanczos2_ps.get()
                                          : up_filter == 2                       ? s.upscale_l15_ps.get()
                                          : up_filter == 3                       ? s.fsr1_easu_ps.get()
                                          : up_filter == 4                       ? s.upscale_bilinear_ps.get()
                                                                              : nullptr;
  if (upscale_ps && key.color_count == 1 && key.samples <= 1) {
    host_ps = upscale_ps;
    const int32_t used = scene_copy_up ? UpscaleFilter() : up_filter;
    key.reserved |= used == 1 ? 8u : used == 3 ? 32u : used == 4 ? 64u : 16u;
    if (scene_copy_up) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        RGPU_INFO("[upscale] the 1280x720 scene's copy into its 1920x1080 target drawn with {}",
                  used == 1 ? "Lanczos-2" : "FSR 1 (EASU, then RCAS)");
      }
    }
    std::memset(key.bool_mask, 0, sizeof(key.bool_mask));
    std::memset(key.bool_values, 0, sizeof(key.bool_values));
    key.alpha_func = 0;
    key.blend_skip = 0;
    key.output_trim = 0;
    LogUpscaleOnce(UpscaleFilter(), ps_constants);
  }
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_chain_reuse): the previous draw's pipeline for its key again when no lookup was made
  // since and no prebuilt pipeline came in (PipelineLookupsLocked): GetPipelineLocked would answer the same.
  struct PipelineReuse {
    uint64_t seq = 0, lookups = 0, epoch = 0;
    plume::RenderPipeline *pipeline = nullptr;
    // fnxf_xiii (2026-11-16): the key's reserved bits the pipeline was looked up with (constant 914's motion bit is the
    // draw's own, not one of the things 'reused' compares): another value, another pipeline.
    u32 reserved = 0;
  };
  static PipelineReuse preuse;
  plume::RenderPipeline *pipeline = nullptr;
  // fnxf_xiii (2026-10-20): the prebuild epoch read before the lookup, not after it: a worker that finished a specialised
  // pipeline between this draw's lookup (the generic one, its job not done yet) and the epoch read left the next draw
  // reusing the generic pipeline while a lookup would give the specialised one (emulator run 120: one
  // fnxf_xiii_renut_chain_reuse_verify difference in 7 minutes; the same pixels, the generic pipeline reads the bools).
  rtw_key.End();
  const uint64_t prebuild_epoch = PrebuildEpochLocked();
  if (reused && preuse.pipeline && preuse.seq + 1 == hint.seq && preuse.lookups == PipelineLookupsLocked() &&
      preuse.epoch == prebuild_epoch && preuse.reserved == key.reserved) {
    pipeline = preuse.pipeline;
    if (reuse_verify) {
      plume::RenderPipeline *made =
          rtw::Timed(rtw::kPipeline, [&] { return GetPipelineLocked(key, {host_vs, host_ps, &declaration->layout}); });
      if (made != pipeline) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("fnxf_xiii_renut_chain_reuse_verify: the reused pipeline differs from the one looked up");
        pipeline = made;
      }
    }
  } else {
    pipeline =
        rtw::Timed(rtw::kPipeline, [&] { return GetPipelineLocked(key, {host_vs, host_ps, &declaration->layout}); });
  }
  if (reuse_on)
    preuse = {hint.seq, PipelineLookupsLocked(), prebuild_epoch, pipeline, key.reserved};
  // fnxf_xiii (2026-11-06, fnxf_xiii_motion_vectors): a scene draw with its motion (MotionPrepareLocked), not blended
  // and writing depth, writes the motion target (BindTargets); blended ones (effects) leave it.
  const bool motion_write = (sc.motion & 1u) && key.color_count &&
                            (key.blend_control[0] & 0x1FFF1FFF) == 0x00010001 && (key.depth_control & 2) &&
                            (key.depth_control & 4);
  // fnxf_xiii (2026-11-08, fnxf_xiii_lazy_resolve_mips): host levels made now for textures this draw samples into a
  // smaller target (before its targets are bound: the levels are drawn with their own).
  if (PendingMipsLocked()) {
    const HostSurface *first = targets.color_count && targets.colors[0] ? targets.colors[0] : targets.depth;
    const uint32_t target_width = first ? first->scale.Size(first->width) : 0u;
    bool made = false;
    for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r)
      if (texture_mask & (1u << r))
        made |= MakePendingMipsLocked(device.U32(d3d::kDeviceOffsetTextures + r * 4), target_width);
    if (made) {
      g_bound.framebuffer = nullptr;
      g_bound.pipeline = nullptr;
      g_bound.layout = false;
      g_bound.viewport_set = false;
    }
  }
  if (!pipeline || !rtw::Timed(rtw::kTargets, [&] {
        g_motion_target.wanted = motion_write;
        // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): the images the destinations take now that the draw is made.
        if (g_colour_take_pending_count)
          ColourHandOverDrawBindLocked();
        g_colour_ho_draw_checked = true;
        const bool bound = BindTargets(targets);
        g_colour_ho_draw_checked = false;
        g_motion_target.wanted = false;  // other binds (clears, resolves) bind as usual
        return bound;
      }))
    return false;
  EdramCarryLocked(targets);  // fnxf_xiii (2026-10-31)
  plume::RenderCommandList *cmd = s.command_list;
  rtw::Span rtw_record(rtw::kRecord);  // fnxf_xiii (2026-11-05, seen18, [rtwork]): the draw's commands, to the draw call
  SetViewport(cmd, viewport.viewport, viewport.scissor, targets.color_count);

  if (!g_bound.layout) {
    cmd->setGraphicsPipelineLayout(s.draw_layout.get());
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 1);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 2);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 3);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 4);
    g_bound.layout = true;
    g_bound.pipeline = nullptr;
    std::fill(std::begin(g_bound.root_set), std::end(g_bound.root_set), false);
    std::fill(std::begin(g_bound.constant_sets), std::end(g_bound.constant_sets), nullptr);
  }
  if (g_bound.pipeline != pipeline) {
    // fnxf_xiii: NVK's prefetch hint (the Switch); the record worker gives it itself, on the core that binds.
    if (!fnxf_xiii::renut_gpu::record::IsProxy(cmd))
      fnxf_xiii::renut_gpu::BeforePipelineBind(pipeline);
    cmd->setPipeline(pipeline);
    g_bound.pipeline = pipeline;
    // Vulkan: a pipeline without the constant as dynamic state overwrites it.
    if (IsVulkan())
      g_bound.blend_constant_set = false;
  }
  float blend_constant[4];
  if (DrawBlendConstant(dev, key, blend_constant) &&
      !(g_bound.blend_constant_set &&
        std::memcmp(blend_constant, g_bound.blend_constant, sizeof(blend_constant)) == 0)) {
    if (SetBlendConstant(cmd, blend_constant)) {
      std::memcpy(g_bound.blend_constant, blend_constant, sizeof(blend_constant));
      g_bound.blend_constant_set = true;
    } else {
      static bool logged = false;
      if (!logged) {
        logged = true;
        RGPU_WARN("Draws blend with the blend constant, which this backend cannot set");
      }
    }
  }
  if (UsesVulkanBindings()) {  // fnxf_xiii: and deko3d
    // fnxf_xiii (fnxf_xiii_renut_push_point_only): with the constants in uniform buffers only point_sprite_gs reads the
    // push constant, so the other draws do not push it (NVK: a root-table upload a draw less).
    if (key.point_sprite || !fnxf_xiii::renut_gpu::PushPointOnly()) {
      const DrawPushConstants push{vs_alloc.gpu_address, ps_alloc.gpu_address, sc_alloc.gpu_address};
      cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    }
    // fnxf_xiii: what the guest shaders read.
    if (pushed) {
#if defined(PLUME_DEKO3D)
      // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_push_constants): the three fixed buffers (bindings 0, 1, 2), and bound
      // textures read from the renderer's copy of the shared constants.
      plume::Deko3DBindUniform(cmd, 0, g_pushed_buffers.address, kFloatConstantsUboBytes);
      plume::Deko3DBindUniform(cmd, 1, g_pushed_buffers.address + kPushedFloatStride, kFloatConstantsUboBytes);
      plume::Deko3DBindUniform(cmd, 2, g_pushed_buffers.address + kPushedSharedOffset, u32(sizeof(SharedConstants)));
      std::fill(std::begin(g_bound.constant_sets), std::end(g_bound.constant_sets), nullptr);  // rebind after
      if (!g_bound.shared_words_set) {
        plume::Deko3DSetSharedWords(cmd, &g_bound.pushed_shared, u32(sizeof(SharedConstants)));
        g_bound.shared_words_set = true;
      }
#endif
    } else {
#if defined(PLUME_DEKO3D)
      if (g_bound.shared_words_set) {
        plume::Deko3DSetSharedWords(cmd, nullptr, 0);
        g_bound.shared_words_set = false;
      }
#endif
      BindConstantSet(cmd, kVertexConstantsSet, vs_alloc.float_constants_set, vs_alloc.ref.offset);
      BindConstantSet(cmd, kPixelConstantsSet, ps_alloc.float_constants_set, ps_alloc.ref.offset);
      BindConstantSet(cmd, kSharedConstantsSet, sc_alloc.shared_constants_set, sc_alloc.ref.offset);
    }
  } else {
    SetRootDescriptor(cmd, vs_alloc.ref, kDrawRootVertexConstants);
    SetRootDescriptor(cmd, ps_alloc.ref, kDrawRootPixelConstants);
    SetRootDescriptor(cmd, sc_alloc.ref, kDrawRootSharedConstants);
    SetRootDescriptor(cmd, fetch_alloc.ref, kDrawRootVertexFetchData);
    SetRootDescriptor(cmd, plume::RenderBufferReference(memexport), kDrawRootMemExport);
  }

  if (!BindStreams(device, call, *vs, shader_strides, declaration->layout,
                   declaration->layout_hash, geometry, gpu_streams)) {
    return false;
  }
#if defined(PLUME_DEKO3D)
  // fnxf_xiii (2026-10-20, fnxf_xiii_deko3d_skin_once, FurSkinLocked): the fur's vertex shaders skin once a frame. The
  // pipeline has constant 913 under the same condition (pipeline.cpp); the mode word goes to the GPU with the pushed
  // shared constants (one 64-byte line again).
  if (pushed && !export_count && fetch_window && IsSkinOnceVs(vs->hash) && fnxf_xiii::renut_gpu::SkinOnceOn() &&
      !(key.reserved & 4u) && !call.tessellation && geometry.instance_count == 1 && !geometry.per_instance) {
    if (const u32 mode = FurSkinLocked(s, cmd, vs, geometry, sc, fetch_window)) {
      sc.pad_1352[0] = std::bit_cast<float>(mode);
      PushSharedConstants(cmd, sc);
    }
  }
#endif
  if (gpu_profile::Timed()) {
    // fnxf_xiii (package 28b): the GPU profile's draws a frame, and with fnxf_xiii_renut_diag_gpu_shaders its label.
    // fnxf_xiii (2026-10-19, render-thread proposal 7): only in the frames the profile times.
    uint32_t label = fnxf_xiii::renut_gpu::DiagGpuShadersOn() ? ShaderProfileLabel(targets, vs, ps, key)
                                                          : TargetsProfileLabel(targets);
    // fnxf_xiii (2026-10-27): memexport draws under their own label, by vertex shader (the GPU slow frame report); the
    // draw after one marks its target's label again (Mark does nothing when the label is the current one).
    if (export_count) {
      // fnxf_xiii (2026-10-27): with the primitive and the geometry shader it ran with (point lists: point_sprite_gs).
      static std::unordered_map<uint64_t, uint32_t> export_labels;
      const u32 shape = (u32(call.primitive) << 2) | (key.point_sprite ? 1u : 0u) | (key.rectangle_list ? 2u : 0u);
      uint32_t &l = export_labels[vs->hash ^ (uint64_t(shape) << 56)];
      if (!l) {
        l = gpu_profile::Label(fmt::format("memexport draws, VS {:016X}, primitive {}{} [vertices]", vs->hash,
                                           u32(call.primitive), key.point_sprite      ? ", point sprite GS"
                                                                : key.rectangle_list ? ", rectangle GS"
                                                                                     : ""));
        gpu_profile::SetLabelShaders(l, 0, vs->hash, ps ? ps->hash : 0);  // the slow frame report writes their GLSL
      }
      label = l;
      gpu_profile::MarkAlways(s, label);  // fnxf_xiii (2026-10-27): each export draw its own span
      // fnxf_xiii (2026-10-27): the vertices the draw runs, as its amount
      gpu_profile::AddAmount(s, label, uint64_t(geometry.indexed ? geometry.index_count : geometry.vertex_count) *
                                           std::max<uint64_t>(geometry.instance_count, 1));
    }
    gpu_profile::Mark(s, label);
    gpu_profile::CountDraw(s, label);
  }
  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_lean_diagnostics): the draw log's check once, not a call per draw.
  static const bool draw_log = !REXCVAR_GET(fnxf_xiii_renut_lean_diagnostics) ||
                               REXCVAR_GET(fnxf_xiii_renut_diag_draw_log_s) > 0 ||
                               REXCVAR_GET(fnxf_xiii_diag_load_draw_log) > 0;
  if (draw_log)
    DrawLog(call, geometry, vs, ps, targets, key, sc, export_count, device);
  // fnxf_xiii (2026-10-19, fnxf_xiii_renut_resolve_skip): the targets this draw can write.
  for (u32 i = 0; i < targets.color_count && i < key.color_count; ++i)
    if ((key.color_mask >> (4 * i)) & 0xF)
      ++targets.colors[i]->writes;
  if (targets.depth) {
    const u32 dc = key.depth_control;
    const bool depth_writes = (dc & 2) && (dc & 4);
    const bool stencil_writes = (dc & 1) && ((key.stencil_ref_mask >> 16) & 0xFF) != 0;
    if (depth_writes || stencil_writes)
      ++targets.depth->writes;
    if (dc & 1)
      targets.depth->stencil_used = true;  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover)
  }
  rtw_record.End();  // (a kick NoteDrawLocked may make has its own scope)
  Video::NoteDrawLocked();  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_kick_submit): the open list's draws
#if defined(PLUME_DEKO3D)
  {  // fnxf_xiii (2026-11-14, fnxf_xiii_repeat_draw_fast): this draw as the one a repeat can follow
    static const bool repeat_on = REXCVAR_GET(fnxf_xiii_repeat_draw_fast);
    RepeatDraw &r = g_repeat;
    r.armed = false;
    if (repeat_on && pushed && replay && replay->vs_dirty && chain.seq && !export_count && !call.tessellation &&
        !call.up_vertices && !call.up_indices && call.primitive != kPointList && !gpu_profile::Timed() && !draw_log &&
        !FrameDumpOn() && !IsSkinOnceVs(vs->hash) && !OutputPromotionOn() && MotionMode() == 0 && targets.depth &&
        targets.color_count) {
      r.armed = true;
      r.seq = chain.seq;
      r.serial = render_thread::ItemSerial();
      r.call = call;
      r.cmd = cmd;
      r.vs_used = vs_used;
      r.ps_used = ps_used;
      r.vs_entry = vs->entry;
      r.ps_entry = ps ? ps->entry : nullptr;
      r.indexed = geometry.indexed;
      r.count = geometry.indexed ? geometry.index_count : geometry.vertex_count;
      r.instances = geometry.instance_count;
      r.color_count = 0;
      for (u32 i = 0; i < targets.color_count && i < key.color_count; ++i)
        if ((key.color_mask >> (4 * i)) & 0xF)
          r.colors[r.color_count++] = targets.colors[i];
      const u32 repeat_dc = key.depth_control;
      r.depth = targets.depth;
      r.depth_counts =
          ((repeat_dc & 2) && (repeat_dc & 4)) || ((repeat_dc & 1) && ((key.stencil_ref_mask >> 16) & 0xFF) != 0);
    }
  }
#endif
  if (geometry.indexed) {
    SetIndexBuffer(cmd, geometry.index_view);
    // fnxf_xiii: instanced for a tessellation grid (fnxf_xiii_gpu_tess_grid); 1 for every other indexed draw.
    cmd->drawIndexedInstanced(geometry.index_count, geometry.instance_count, 0, 0, 0);
  } else {
    cmd->drawInstanced(geometry.vertex_count, geometry.instance_count, 0, 0);
  }
  if (export_count) {
    g_watch.vs = vs->hash;  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_watch)
    g_watch.ps = ps ? ps->hash : 0;
    FinishMemExportLocked(s, exports, export_count, export_offsets, true);  // fnxf_xiii (2026-10-30): run as a shader
    // fnxf_xiii (2026-10-27): not on deko3d, whose targets stay bound across copies (no render pass): the next export
    // draw bound its target again (FinishMemExportLocked records no copies since the readbacks moved to the end of the
    // recording; PrepareMemExportLocked's and GrowSlotExportBuffersLocked's copies clear it themselves).
    if (!IsDeko3D())
      g_bound.framebuffer = nullptr;  // the copies end any render pass
  }
  // fnxf_xiii (2026-11-06, fnxf_xiii_fsr3): at the game's own resolution (1280x720) there is no upscale draw: the scene,
  // the post-processing into other targets, then the finished picture copied into the scene's colour target by a
  // full-screen draw without depth (Steppe dumps, 2026-11-06: draw 189, then the UI from 190). FSR 3.1 runs there, in
  // place (input and output that target), once a frame, before the UI.
#if defined(PLUME_DEKO3D)
  // fnxf_xiii (2026-11-07): a 1280x720 scene with a 1920x1080 output: the single-sampled 1280x720 targets drawn after
  // its picture's copy into the scene's colour target (kSceneCopyPs) in a frame (the copy's own, the HUD's colour and
  // depth) are promoted to 1.5x (graphics_options.h); its post-processing's before it stay 1x (all at 1.5x cost about
  // 10 ms more a frame: its post-processing at 1080p and mip levels made from it).
  // fnxf_xiii (2026-11-07, dynamic resolution): and with dynamic resolution at a 1280x720 output (the scene's variant of
  // such a target is the dynamic one there, the output's not).
  if (OutputPromotionOn()) {  // fnxf_xiii (2026-11-08): and a 1600x900 / 1024x576 scene's (EarlyFsrOn)
    uint64_t &copied_frame = g_output_copied_frame;
    const uint64_t frame = TextureFrameLocked();
    if (fnxf_xiii::RenderHeight() == 720 && PictureCopy(device, ps))  // fnxf_xiii (2026-11-08): the picture's copy
      copied_frame = frame;
    if (copied_frame == frame) {
      bool promoted = false;
      auto promote = [&](const HostSurface *t) {
        const bool scene_variant = Output1080p() ? t && t->scale.code == 0 : t && t->scale.drs;
        if (scene_variant && !((t->info_word >> 16) & 3) &&
            SceneSized(d3d::SurfaceWidth(t->size_word), d3d::SurfaceHeight(t->size_word)))
          promoted |= PromoteOutputTargetLocked(t->out_key);
      };
      for (u32 i = 0; i < targets.color_count; ++i)
        promote(targets.colors[i]);
      promote(targets.depth);
      if (promoted)
        BumpSurfaceEpochLocked();
    }
  }
  // fnxf_xiii (2026-11-08): FSR 3.1 in place at a 1280x720 scene's picture copy is gone (EarlyFsrOn).
#endif
  // fnxf_xiii (2026-10-25, fnxf_xiii_renut_upscale_filter 3): FSR 1's RCAS on the upscale's target, before the game draws
  // its UI over it (render/fsr1.cpp); its pass leaves its own targets, pipeline, layout and viewport bound.
  if (host_ps && host_ps == s.fsr1_easu_ps.get() && targets.color_count == 1 && !export_count &&
      Fsr1RcasRunLocked(targets.colors[0])) {
    g_bound.framebuffer = nullptr;
    g_bound.pipeline = nullptr;
    g_bound.layout = false;
    g_bound.viewport_set = false;
  }
  return true;
}

// fnxf_xiii (2026-10-24): the depth target sizes the game draws its scene with (draws that write colour too), for the
// SaltyNX overlay's "Depth" (sdk/src/ui/switch_saltynx.cpp shows the one set most often), published once a second.
#if defined(__SWITCH__)
extern "C" void RexSwitchSaltyNxResolutions(const uint64_t *depth, const uint64_t *viewport);
#endif
namespace {
struct ResolutionCount {
  uint32_t width = 0, height = 0, calls = 0, draws = 0;
};
struct ResolutionCounts {
  ResolutionCount slots[8];
  uint32_t last_width = 0, last_height = 0;
  // fnxf_xiii (2026-10-24): counted when the size changes from the previous colour draw's, as NX-FPS counts the game's
  // own calls that set it (a count of every draw ran into the thousands a second).
  // fnxf_xiii (2026-10-24): the draws of each size are counted too, and only the size with the most draws is sent
  // (with its count of sets): the game sets its 1024x576 scene target and a 1280x720 one once a frame each (the log's
  // "Depth target" lines), so their sets tied and the overlay switched between them every second.
  void Note(uint32_t width, uint32_t height) {
    if (!width || !height)
      return;
    const bool set = width != last_width || height != last_height;
    last_width = width;
    last_height = height;
    for (ResolutionCount &c : slots) {
      if (c.draws && c.width == width && c.height == height) {
        ++c.draws;
        c.calls += set;
        return;
      }
      if (!c.draws) {
        c = {width, height, 1, 1};
        return;
      }
    }
  }
  void Take(uint64_t out[8]) {
    int best = -1;
    for (int i = 0; i < 8; ++i)
      if (slots[i].draws && (best < 0 || slots[i].draws > slots[best].draws))
        best = i;
    // The per-size draws, for the log now and then (what the choice was made from).
    static uint32_t takes = 0, logged = 0;
    if (best >= 0 && ++takes % 30 == 0 && logged < 20) {
      ++logged;
      std::string line;
      for (const ResolutionCount &c : slots)
        if (c.draws)
          line += fmt::format(" {}x{}: {} draws, {} sets;", c.width, c.height, c.draws, c.calls);
      RGPU_INFO("[saltynx] depth sizes this second:{}", line);
    }
    for (int i = 0; i < 8; ++i)
      out[i] = 0;
    if (best >= 0) {
      const ResolutionCount &c = slots[best];
      out[0] = uint64_t(c.width & 0xFFFF) << 32 | uint64_t(c.height & 0xFFFF) << 16 |
               std::min<uint32_t>(std::max<uint32_t>(c.calls, 1), 0xFFFEu);
    }
    for (ResolutionCount &c : slots)
      c = {};
    last_width = last_height = 0;  // the next second counts the first of each again
  }
};
ResolutionCounts g_depth_sizes;
}  // namespace

// In host pixels, as the GPU renders them (the guest sizes times the render scale: renut_resolution). fnxf_xiii
// (2026-10-24): the depth targets of draws that also write colour only (the scene's; shadow maps are depth-only), for
// SaltyNX's "Depth", which shows the one set most often (sdk/src/ui/switch_saltynx.cpp). "Viewport" is the presented
// size, which the SDK has: the renderer's viewport sizes (every pass's) are no longer sent.
void NoteOverlayResolutionsLocked(const HostSurface *depth, bool with_colour) {
#if defined(__SWITCH__)
  if (depth && with_colour)
    // fnxf_xiii (2026-11-08): the surface's size (its size word is one tile's with the game's predicated tiling: 400
    // wide at 1600x900), as drawn (2026-11-07, dynamic resolution).
    g_depth_sizes.Note(depth->scale.DrawnSize(depth->width), depth->scale.DrawnSize(depth->height));
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(1))
    return;
  last = now;
  uint64_t d[8], v[8] = {};
  g_depth_sizes.Take(d);
  RexSwitchSaltyNxResolutions(d, v);
#else
  (void)depth;
  (void)with_colour;
#endif
}

// fnxf_xiii (2026-10-27): a page of memexport results for guest memory (CompleteMemExportsLocked): 'seed' is the
// window's seed of the page (null: taken whole), 'range' the page's offset in the export buffer (its cache lines are
// invalidated first; UINT64_MAX: the whole window was).
struct CopyJob {
  u8 *guest;
  const u8 *result;
  const u8 *seed;
  uint64_t range;
  u32 len;
  u32 page = UINT32_MAX;  // fnxf_xiii (2026-10-27): its physical page, and its seed's interval (HarvestPageLocked)
  u32 interval = 0;
  bool seed_gpu = false;       // fnxf_xiii (2026-10-28): the seed is the GPU's copy (ExportSeedPiece::gpu)
  mutable bool whole = false;  // fnxf_xiii (2026-10-28): set by CopyPage when guest memory took the result whole
  // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the page's 32 words of the write mask (a bit a dword; its
  // lines invalidated by BuildCompletionJobsLocked): only the dwords it has are copied, no seed and no compare.
  const u32 *mask = nullptr;
};
// fnxf_xiii (2026-10-27): pages whose cards say the game has not written them since their seed, and of those, the ones
// guest memory has other bytes in all the same (a write the cards missed). Counted only: every page is compared and
// merged as before (copy_cards' shortcut took them whole, and lost those writes).
std::atomic<uint64_t> g_copy_clean_pages{0}, g_copy_missed_writes{0};
// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): bytes copied by the mask since the last [burst] line.
std::atomic<uint64_t> g_mask_copied_bytes{0};
// fnxf_xiii (2026-10-27): of the missed ones, those this renderer's own results' copy wrote since the seed (another
// slot's completion: g_copy_written), and a few of the others, for the log.
uint32_t g_copy_written[0x20000];
std::atomic<uint64_t> g_copy_missed_ours{0};
std::atomic<uint32_t> g_missed_samples_count{0};
struct MissedSample {
  u32 page, interval, game_serial, copy_serial, first_diff, diff_bytes;
} g_missed_samples[8];

// Whether the cards say the page was not written by the game or a host writer since 'interval' (its own card, and the
// page before's: an unaligned store marks only its first byte's page).
bool CardsUnwrittenSince(u32 page, u32 interval) {
  return HarvestPageLocked(page) < interval && (page == 0 || HarvestPageLocked(page - 1) < interval);
}
std::vector<CopyJob> g_copy_jobs;

// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the page's dwords the mask has, result to guest memory, in
// runs (a whole word: 128 bytes at once). The other dwords keep guest memory's bytes, whether the seed's or the game's
// newer ones: what the merge of the seeded path kept, without a seed or a compare. Bytes copied.
u32 CopyMaskedPage(const CopyJob &j) {
  const u32 words = (j.len + 127u) / 128u;
  u32 copied = 0;
  for (u32 w = 0; w < words; ++w) {
    u32 m = j.mask[w];
    if (!m)
      continue;
    const u32 base = w * 128u;
    if (m == 0xFFFFFFFFu) {
      const u32 n = std::min(128u, j.len - base);
      std::memcpy(j.guest + base, j.result + base, n);
      copied += n;
      continue;
    }
    while (m) {
      const u32 lo = u32(std::countr_zero(m));
      const u32 run = u32(std::countr_one(m >> lo));  // set bits from lo on (lo < 32: the shift is defined)
      const u32 at = base + lo * 4u;
      if (at >= j.len)
        break;
      const u32 n = std::min(run * 4u, j.len - at);
      std::memcpy(j.guest + at, j.result + at, n);
      copied += n;
      m = lo + run >= 32u ? 0u : (m & ~((1u << (lo + run)) - 1u));  // the bits taken cleared
    }
  }
  return copied;
}

// One page: as CompleteMemExportsLocked did it inline. True when merged byte by byte (the game wrote it too).
bool CopyPage(plume::RenderBuffer *readback, const CopyJob &j) {
  if (j.range != UINT64_MAX) {
    const plume::RenderRange page_range(j.range, j.range + j.len);
    readback->map(0, &page_range);  // dc civac of the page's lines: thread-safe, a cache maintenance loop only
  }
  if (j.mask) {
    // fnxf_xiii (2026-10-29): by the write mask. The page equals guest memory afterwards when the cards show no game
    // write since the window's seed (for the kept window's record; our own copies of the other slot's results are in
    // the window too, SeedFromPendingExportsLocked).
    g_mask_copied_bytes.fetch_add(CopyMaskedPage(j), std::memory_order_relaxed);
    j.whole = j.page != UINT32_MAX && CardsUnwrittenSince(j.page, j.interval);
    return false;
  }
  if (!j.seed) {
    std::memcpy(j.guest, j.result, j.len);
    j.whole = true;
    return false;
  }
  // fnxf_xiii (2026-10-28): a seed the GPU wrote (a kept window's): the CPU's lines of it dropped before it is read.
  bool seed_ready = !j.seed_gpu;
  auto ready = [&] {
    if (!seed_ready) {
#if defined(__SWITCH__)
      armDCacheFlush(const_cast<u8 *>(j.seed), j.len);
#endif
      seed_ready = true;
    }
    return true;
  };
  // fnxf_xiii (2026-10-27): what the cards say of the page (counted; see g_copy_clean_pages).
  const bool cards_clean = j.page != UINT32_MAX && CardsUnwrittenSince(j.page, j.interval);
  const u32 copied_before = j.page != UINT32_MAX ? g_copy_written[j.page] : 0;  // an earlier completion's copy
  // fnxf_xiii (2026-10-27): no game write since the seed (the cards; 2026-10-27's card_writers run: every game write
  // into the windows marked its card, and every page that changed under clean cards was this renderer's copy of the
  // other slot's results, which this window already has on the GPU, SeedFromPendingExportsLocked): the result is what
  // the page is to hold, taken whole, no comparing. One in 64 is compared all the same; a change no copy of ours
  // explains is merged and logged.
  if (cards_clean) {
    const uint64_t n = g_copy_clean_pages.fetch_add(1, std::memory_order_relaxed);
    if ((n & 63u) != 0 || copied_before >= j.interval || (ready() && std::memcmp(j.guest, j.seed, j.len) == 0)) {
      if ((n & 63u) == 0 && copied_before >= j.interval)
        g_copy_missed_ours.fetch_add(1, std::memory_order_relaxed);
      std::memcpy(j.guest, j.result, j.len);
      g_copy_written[j.page] = u32(g_flush_serial);
      j.whole = true;
      return false;
    }
  }
  ready();
  // fnxf_xiii (2026-10-27): a page the GPU left as seeded is left as guest memory has it (the same bytes, or the
  // game's newer ones the merge below would keep).
  if (std::memcmp(j.result, j.seed, j.len) == 0)
    return false;
  if (std::memcmp(j.guest, j.seed, j.len) == 0) {
    std::memcpy(j.guest, j.result, j.len);
    if (j.page != UINT32_MAX)
      g_copy_written[j.page] = u32(g_flush_serial);
    j.whole = true;
    return false;
  }
  if (j.page != UINT32_MAX)
    g_copy_written[j.page] = u32(g_flush_serial);  // merged below
  if (cards_clean) {
    g_copy_missed_writes.fetch_add(1, std::memory_order_relaxed);  // changed, by nothing the cards or our copies say
    {
      const u32 k = g_missed_samples_count.fetch_add(1, std::memory_order_relaxed);
      if (k < 8) {
        u32 first = j.len, count = 0;
        for (u32 b = 0; b < j.len; ++b)
          if (j.guest[b] != j.seed[b]) {
            first = std::min(first, b);
            ++count;
          }
        g_missed_samples[k] = {j.page, j.interval, g_game_serial[j.page], copied_before, first, count};
      }
    }
  }
  // fnxf_xiii (2026-10-27): 8 bytes at a time: every byte the GPU changed from the seed takes the result's, the others
  // keep guest memory's (a byte loop was 5754 pages in ~50 ms at an area load's compaction).
  u32 b = 0;
  for (; b + 8 <= j.len; b += 8) {
    uint64_t r, sd, g;
    std::memcpy(&r, j.result + b, 8);
    std::memcpy(&sd, j.seed + b, 8);
    const uint64_t x = r ^ sd;
    if (!x)
      continue;
    constexpr uint64_t kLow7 = 0x7F7F7F7F7F7F7F7Full, kHigh = 0x8080808080808080ull;
    const uint64_t changed = (((x & kLow7) + kLow7) | x) & kHigh;  // a byte's high bit: that byte differs
    const uint64_t mask = (changed >> 7) * 0xFFu;
    std::memcpy(&g, j.guest + b, 8);
    g = (g & ~mask) | (r & mask);
    std::memcpy(j.guest + b, &g, 8);
  }
  for (; b < j.len; ++b)
    if (j.result[b] != j.seed[b])
      j.guest[b] = j.result[b];
  return true;
}

// fnxf_xiii (2026-10-27): the render thread and two helpers (cores 0 and 2; OnCopyHelperStart has their priorities) take
// the pages of g_copy_jobs in turns of 16; the render thread returns once all are done. The helpers sleep on a condition
// variable between completions. Pages are independent (one window at a time, each page once).
class CopyHelpers {
public:
  static constexpr u32 kHelpers = 2;
  static constexpr u32 kBatch = 16;

  // fnxf_xiii (2026-10-28): the jobs [begin, end) (a deferred completion takes them a part at a time).
  uint64_t Run(plume::RenderBuffer *readback, size_t begin, size_t end) {
    if (end - begin < 64) {  // not worth waking anyone
      uint64_t merged = 0;
      for (size_t i = begin; i < end; ++i)
        merged += CopyPage(readback, g_copy_jobs[i]);
      return merged;
    }
    return RunAll(readback, begin, end, false);
  }

  // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_parallel_seeds): the first 'count' of g_seed_jobs (2 MB each), one a
  // thread at a time: the render thread and the helpers memcpy a window's 16 pieces together.
  void RunSeeds(size_t count) {
    if (count == 0)
      return;
    if (count < 2) {
      SeedPiece(g_seed_jobs[0]);
      return;
    }
    RunAll(nullptr, 0, count, true);
  }

  static void SeedPiece(const SeedJob &j) {
    std::memcpy(j.dst, j.src, j.size);
    if (!j.buffer)
      return;  // an upload ring's allocation: the ring's lines are cleaned at the submit
    const plume::RenderRange written(j.offset, j.offset + j.size);
    j.buffer->unmap(0, &written);  // dc cvac of the piece: a cache maintenance loop, thread-safe
  }

private:
  uint64_t RunAll(plume::RenderBuffer *readback, size_t begin, size_t end, bool seeds) {
    Start();
    {
      std::lock_guard lock(mutex_);
      readback_ = readback;
      seeds_ = seeds;
      end_ = end;
      next_.store(begin, std::memory_order_relaxed);
      merged_.store(0, std::memory_order_relaxed);
      busy_ = 0;
      active_ = true;
      ++generation_;
    }
    wake_.notify_all();
    Work();
    // fnxf_xiii (2026-11-05, seen16): only the helpers that joined are waited for (the work is taken from next_, so the
    // render thread's Work above did whatever no helper took): a helper that is not running yet delays nothing.
    std::unique_lock lock(mutex_);
    active_ = false;
    done_.wait(lock, [this] { return busy_ == 0; });
    return merged_.load(std::memory_order_relaxed);
  }

private:
  void Start();

public:
  // A helper's loop (a long-lived thread: std::thread::detach throws on the Switch, StartLongLivedThread).
  void HelperLoop(u32 index) {
    fnxf_xiii::renut_gpu::OnCopyHelperStart(index);
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
        if (!active_)
          continue;  // that run is over (fnxf_xiii 2026-11-05: RunAll waits only for the helpers that joined)
        ++busy_;
      }
      Work();
      {
        std::lock_guard lock(mutex_);
        --busy_;
      }
      done_.notify_one();
    }
  }

private:
  void Work() {
    const size_t count = end_;
    uint64_t merged = 0;
    if (seeds_) {  // fnxf_xiii (2026-10-29): seed pieces, one at a time (each is 2 MB)
      for (;;) {
        const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count)
          break;
        SeedPiece(g_seed_jobs[i]);
      }
      return;
    }
    for (;;) {
      const size_t begin = next_.fetch_add(kBatch, std::memory_order_relaxed);
      if (begin >= count)
        break;
      const size_t end = std::min(count, begin + kBatch);
      for (size_t i = begin; i < end; ++i)
        merged += CopyPage(readback_, g_copy_jobs[i]);
    }
    merged_.fetch_add(merged, std::memory_order_relaxed);
  }

  bool started_ = false;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  uint64_t generation_ = 0;
  u32 busy_ = 0;       // helpers inside the current run's Work
  bool active_ = false;  // a run is taking helpers (RunAll)
  plume::RenderBuffer *readback_ = nullptr;
  bool seeds_ = false;  // fnxf_xiii (2026-10-29): this run takes g_seed_jobs, not g_copy_jobs
  size_t end_ = 0;
  std::atomic<size_t> next_{0};
  std::atomic<uint64_t> merged_{0};
};
CopyHelpers g_copy_helpers;

void CopyHelpers::Start() {
  if (started_)
    return;
  started_ = true;
  fnxf_xiii::renut_gpu::StartLongLivedThread([] { g_copy_helpers.HelperLoop(0); }, 64 << 10);
  fnxf_xiii::renut_gpu::StartLongLivedThread([] { g_copy_helpers.HelperLoop(1); }, 64 << 10);
}

uint64_t RunCopyJobs(plume::RenderBuffer *readback, size_t begin, size_t end) {
  return g_copy_helpers.Run(readback, begin, end);
}

// fnxf_xiii (2026-10-29, fnxf_xiii_memexport_parallel_seeds): PrepareMemExportLocked's pieces of a window's seed.
void RunSeedJobs(size_t count) {
  g_copy_helpers.RunSeeds(std::min(count, g_seed_jobs.size()));
}

// fnxf_xiii (2026-10-30): backend/guest_gpu_memory.h (CleanForGpu's cards).
uint32_t GuestPageWriteCount(uint32_t page) {
  if (page >= 0x20000u)
    return 0;
  HarvestPageLocked(page);  // the cards read and reset: a zero card counts as a write found now
  return g_game_writes[page];
}

// fnxf_xiii (2026-10-30): backend/guest_gpu_memory.h.
const uint32_t *GuestPageWriteCounts() {
  return g_game_writes;
}

void HarvestGuestPagesLocked(uint32_t first, uint32_t last) {
  HarvestPagesLocked(first, last);
}

// fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy; draw.h): the CPU's lines of every window pending in the current
// slot cleaned to memory, by the cards (guest_gpu::CleanForGpu), right before the slot's lists are submitted (video.cpp
// KickLocked, FinishRecordingLocked): the kernels write guest memory itself once the GPU runs them, and a dirty line of
// the game's written back after that would put the game's bytes over the kernel's. The windows are 32 MB each, so this
// is one scan of them per submit, not one per kernel draw.
void CleanPendingExportsForGpuLocked() {
  if (!ZeroCopyOn())
    return;
  const SlotExports &slot = g_exports[State().frame];
  for (const PendingExport &p : slot.pending)
    guest_gpu::CleanForGpu(p.base, p.size);
}
uint32_t GuestWriteInterval() {
  return u32(g_flush_serial);
}

// fnxf_xiii (2026-10-28): a slot's results on their way to guest memory, a window at a time and, deferred, a part of a
// window at a time (DeferMemExportsLocked): the window, its pages (g_copy_jobs) and how far the copy got.
struct Completion {
  int slot = -1;  // none
  bool deferred = false;
  const u8 *data = nullptr;
  plume::RenderBuffer *readback = nullptr;
  size_t window = 0;  // of g_exports[slot].pending
  bool jobs_built = false;
  size_t next_job = 0;
  u8 *guest = nullptr;
  const u32 *flags = nullptr;
  uint64_t window_merged = 0;
  uint64_t merged_before = 0;
  double ms = 0.0, idle_ms = 0.0;  // the copy's time, and of it in the render thread's idle time
  std::chrono::steady_clock::time_point deferred_at;
} g_completion;
// The copies' bytes and time, logged every 10 s while there are any (2026-10-24).
uint64_t g_completion_bytes = 0, g_completions = 0;
double g_completion_ms_sum = 0.0, g_completion_longest_ms = 0.0;
auto g_completion_last_log = std::chrono::steady_clock::now();
uint64_t g_completion_merged_pages = 0, g_completion_merged_windows = 0;

// The current window's pages (g_copy_jobs), as CompleteMemExportsLocked collected them before.
void BuildCompletionJobsLocked(Completion &c, const PendingExport &p) {
  const std::vector<ExportSeedPiece> &seeds = g_export_seeds[c.slot];
  // fnxf_xiii (2026-10-27): the window's page flags (memExport sets a page's when a kernel writes it): only the
  // flagged pages are invalidated, compared and taken back. Without them (a readback buffer), all of it.
  c.flags = nullptr;
  if (DirectExports() && ExportFlagBytes(p.size)) {
    const uint64_t at = ExportFlagOffset(p.offset, p.size);
    const plume::RenderRange flag_range(at, at + ExportFlagBytes(p.size));
    c.readback->map(0, &flag_range);
    c.flags = reinterpret_cast<const u32 *>(c.data + at);
  }
  const u32 *flags = c.flags;
  auto page_written = [&](u32 page_offset) { return !flags || flags[page_offset >> 12] != 0; };
  ++g_export_wait_windows;
  if (!flags) {
    const plume::RenderRange range(p.offset, p.offset + p.size);
    c.readback->map(0, &range);  // a cached readback's lines of this window invalidated
  }
  u8 *guest = c.guest;
  const u8 *result = c.data + p.offset;
  // Every seed piece of this window: a page the guest memory still holds as seeded takes the result whole; one
  // written since takes the result's bytes only where the GPU changed the seed, and keeps the newer ones.
  // fnxf_xiii (2026-10-27): the pages are copied by the render thread and two helpers (CopyHelpers): one thread took
  // 75 ms for the 61 MB the game's texture kernels write at an area's event.
  g_copy_jobs.clear();
  u32 covered = 0;
  for (const ExportSeedPiece &piece : seeds) {
    if (piece.window != p.offset || piece.at >= p.size)
      continue;
    const u32 n = std::min(piece.size, p.size - piece.at);
    for (u32 page = 0; page < n; page += 4096u) {
      const u32 len = std::min(4096u, n - page);
      if (!page_written(piece.at + page))
        continue;  // fnxf_xiii (2026-10-27): no kernel wrote it
      ++g_export_wait_pages;
      g_copy_jobs.push_back({guest + piece.at + page, result + piece.at + page, piece.cpu + page,
                             flags ? p.offset + piece.at + page : UINT64_MAX, len,
                             len == 4096u ? (p.base + piece.at + page) >> 12 : UINT32_MAX, piece.interval, piece.gpu});
    }
    covered += n;
  }
  if (covered < p.size) {  // no seed kept for (all of) it: the whole window, as before (its written pages)
    if (!flags) {
      std::memcpy(guest, result, p.size);
    } else {
      // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): no window has a seed piece then; each written page's
      // dwords by the window's write mask (its lines invalidated here, once a window).
      const u32 *mask = nullptr;
      if (ExportMaskOn()) {
        const uint64_t mask_at = ExportMaskWordOffset(p.offset);
        const plume::RenderRange mask_range(mask_at, mask_at + ExportMaskBytes(ExportWindowSpan(p.size)));
        c.readback->map(0, &mask_range);
        mask = reinterpret_cast<const u32 *>(c.data + mask_at);
      }
      const bool aligned = (p.base & 4095u) == 0;
      for (u32 page = 0; page < p.size; page += 4096u) {
        if (!page_written(page))
          continue;
        const u32 len = std::min(4096u, p.size - page);
        ++g_export_wait_pages;  // fnxf_xiii (2026-10-30): the "Draw waited" line's pages (0 before, in mask mode)
        CopyJob job{guest + page, result + page, nullptr, p.offset + page, len};
        if (mask) {
          job.mask = mask + (page >> 12) * 32u;
          job.page = aligned && len == 4096u ? (p.base + page) >> 12 : UINT32_MAX;
          job.interval = p.seed_interval;
        }
        g_copy_jobs.push_back(job);
      }
    }
  }
  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_watch): what this window's results do to the watched range's pages.
  if (WatchHit(p.base, p.size)) {
    u32 pages = 0, unchanged = 0, taken = 0, merged_here = 0, whole = 0;
    std::string map;
    for (const CopyJob &j : g_copy_jobs) {
      const u32 at = p.base + u32(j.guest - guest);
      if (!WatchHit(at, j.len))
        continue;
      ++pages;
      if (j.seed_gpu) {
#if defined(__SWITCH__)
        armDCacheFlush(const_cast<u8 *>(j.seed), j.len);
#endif
      }
      char ch;
      if (!j.seed) {
        ++whole;
        ch = 'W';  // taken whole (no seed)
      } else if (std::memcmp(j.result, j.seed, j.len) == 0) {
        ++unchanged;
        ch = '.';  // the kernel left it as seeded
      } else if (std::memcmp(j.guest, j.seed, j.len) == 0) {
        ++taken;
        ch = 'R';  // the result taken (guest memory as seeded)
      } else {
        ++merged_here;
        ch = 'M';  // the game wrote it since the seed: merged
      }
      map += ch;
    }
    static uint32_t logged = 0;
    if (logged++ < 64)
      RGPU_INFO("[watch] frame {}: results of slot {} window {:08X}-{:08X}: {} watched pages written by kernels "
                "({} as seeded, {} taken, {} merged with the game's writes, {} whole) [{}]",
                TextureFrameForLog(), c.slot, p.base, uint64_t(p.base) + p.size, pages, unchanged, taken, merged_here,
                whole, map);
  }
  // fnxf_xiii (2026-10-28): every page these results are taken back to has a new version (KeptWindow).
  for (const CopyJob &j : g_copy_jobs) {
    const u32 page = (p.base + u32(j.guest - guest)) >> 12;
    if (page < 0x20000u)
      ++g_page_version[page];
  }
}

// The current window's pages all copied: the slot's record of it, the other slots' seeds, the cached copies' serials.
void FinishCompletionWindowLocked(Completion &c, const PendingExport &p) {
  auto &s = State();
  u8 *guest = c.guest;
  const u32 *flags = c.flags;
  auto page_written = [&](u32 page_offset) { return !flags || flags[page_offset >> 12] != 0; };
  // fnxf_xiii (2026-10-28): the slot's record of the window: a page taken whole equals guest memory now; one
  // merged or left (the game's newer bytes) may not. The pages no kernel wrote keep what their seed recorded.
  if (KeptWindow *kw = FindKeptWindowLocked(u32(c.slot), s.memexport_buffer[c.slot].get(), p.base, p.size, p.offset)) {
    const bool by_mask = ExportMaskOn();
    for (const CopyJob &j : g_copy_jobs) {
      const u32 rel = u32(j.guest - guest) >> 12;
      if (rel >= kw->version.size())
        continue;
      const u32 page = (p.base >> 12) + rel;
      // fnxf_xiii (2026-10-29): by the write mask only the kernel's dwords went to guest memory, so a page whose other
      // bytes were not guest memory's at the seed (kUnknownVersion: an older window's copied whole) stays unknown.
      if (by_mask && kw->version[rel] == kUnknownVersion)
        continue;
      kw->version[rel] = j.whole && j.len == 4096u && page < 0x20000u ? g_page_version[page] : 0u;
      kw->interval[rel] = u32(g_flush_serial);
    }
  }
  CpuReadSampleLocked(p, flags);  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_cpu_reads)
  g_completion_merged_pages += c.window_merged;
  g_export_wait_merged += c.window_merged;
  g_completion_merged_windows += c.window_merged != 0;
  // fnxf_xiii (2026-10-24): the other slots' pending seeds over this window take its bytes as they now are in
  // guest memory: their windows got this one's results on the GPU (SeedFromPendingExportsLocked), so a page
  // differing from their seed only by them is not the game's write (a byte their kernel set back to its old value
  // was kept from this one: black and blue texels after the texture-kernel bursts).
  for (u32 other = 0; other < kFramesInFlight; ++other) {
    if (other == u32(c.slot))
      continue;
    for (ExportSeedPiece &piece : g_export_seeds[other]) {
      if (piece.gpu)
        continue;  // fnxf_xiii (2026-10-28): copied by the GPU after this window's results reached it
      const uint64_t begin = std::max<uint64_t>(piece.guest, p.base);
      const uint64_t end = std::min<uint64_t>(uint64_t(piece.guest) + piece.size, uint64_t(p.base) + p.size);
      if (begin >= end)
        continue;
      if (!flags) {
        std::memcpy(piece.cpu + (begin - piece.guest), guest + (begin - p.base), size_t(end - begin));
      } else {
        // fnxf_xiii (2026-10-27): only this window's written pages are this window's results.
        for (uint64_t at = begin; at < end;) {
          const uint64_t page_end = std::min<uint64_t>(end, ((at - p.base) & ~uint64_t(4095)) + 4096 + p.base);
          if (page_written(u32(at - p.base)))
            std::memcpy(piece.cpu + (at - piece.guest), guest + (at - p.base), size_t(page_end - at));
          at = page_end;
        }
      }
#if defined(__SWITCH__)
      // fnxf_xiii (2026-10-28): cleaned, so no dirty line is left in a block the GPU may write next (SeedBlock).
      armDCacheClean(piece.cpu + (begin - piece.guest), size_t(end - begin));
#endif
    }
  }
  g_completion_bytes += p.size;
  // fnxf_xiii (2026-10-27): written in this interval for the cached copies, not as the game's write
  // (HarvestPageLocked): the next window's results are taken whole where only these copies changed the page.
  WrotePagesLocked(p.base, p.size);
}

// Every window done: the logs, and the slot's exports no longer pending.
void FinishCompletionLocked(Completion &c, const char *why) {
  const u32 slot = u32(c.slot);
  SlotExports &exports = g_exports[slot];
  if (g_completion_merged_pages) {
    static uint64_t logged_pages = 0;
    static uint32_t logged = 0;
    if (g_completion_merged_pages != logged_pages && logged++ < 16)
      FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] {} pages of {} windows written by the game while their kernels ran: kept, merged with the "
                "results (so far)", g_completion_merged_pages, g_completion_merged_windows);
    logged_pages = g_completion_merged_pages;
  }
  if (c.data) {
    const plume::RenderRange no_range(0, 0);
    c.readback->unmap(0, &no_range);  // fnxf_xiii: nothing written (a whole-buffer clean of a cached export buffer)
  }
  const double ms = c.ms;
  g_completion_ms_sum += ms;
  g_completion_longest_ms = std::max(g_completion_longest_ms, ms);
  ++g_completions;
  // fnxf_xiii (2026-10-27): every long one (with or without a draw waiting), with the copy helpers (CopyHelpers).
  static uint32_t slow_logged = 0;
  if (ms >= 20.0 && slow_logged++ < 48) {
    const uint64_t clean = g_copy_clean_pages.exchange(0), missed = g_copy_missed_writes.exchange(0),
                   ours = g_copy_missed_ours.exchange(0);
    std::string samples;
    const u32 n = std::min<u32>(8, g_missed_samples_count.exchange(0));
    for (u32 k = 0; k < n; ++k) {
      const MissedSample &m = g_missed_samples[k];
      samples += fmt::format("\n  page {:08X}: seed interval {}, game write interval {}, our copy interval {}; "
                             "{} bytes differ from the seed, the first at +{:X}",
                             m.page << 12, m.interval, m.game_serial, m.copy_serial, m.diff_bytes, m.first_diff);
    }
    FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] results to guest memory of slot {}: {:.1f} ms, {} windows (render thread and {} helpers); "
              "{} pages taken whole (no game write since their seed, by the cards); of the one in 64 of them "
              "checked, {} changed by our copy of the other slot's results, {} by something else (merged){}",
              slot, ms, exports.pending.size(), CopyHelpers::kHelpers, clean, ours, missed, samples);
  }
  // fnxf_xiii (2026-10-28): a deferred one: how much was done in the render thread's idle time, and what ended it.
  if (c.deferred) {
    static uint32_t logged = 0;
    const double after = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c.deferred_at)
                             .count();
    if (ms >= 5.0 && logged++ < 96)
      FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] deferred results of slot {}: {:.1f} ms of copying, {:.1f} ms of it in the render thread's "
                "idle time; finished {:.1f} ms after the slot's fence, {}",
                slot, ms, c.idle_ms, after, why);
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_completion_last_log >= std::chrono::seconds(10)) {
    if (REXCVAR_GET(log_diagnostics))
      RGPU_INFO("[export] results to guest memory in 10 s: {} completions, {:.1f} MB in {:.1f} ms ({:.0f} MB/s), "
                "longest {:.1f} ms; readback {} (fnxf_xiii_deko3d_cached_readback), {} (fnxf_xiii_memexport_write_mask)",
                g_completions, double(g_completion_bytes) / (1 << 20), g_completion_ms_sum,
                g_completion_ms_sum > 0.0 ? double(g_completion_bytes) / (1 << 20) / (g_completion_ms_sum / 1000.0)
                                          : 0.0,
                g_completion_longest_ms, REXCVAR_GET(fnxf_xiii_deko3d_cached_readback) ? "cached" : "uncached",
                ExportMaskOn() ? "by the write mask" : "by seed compares");
    g_completion_bytes = g_completions = 0;
    g_completion_ms_sum = g_completion_longest_ms = 0.0;
    g_completion_last_log = now;
  }
  exports.pending.clear();
  exports.used = 0;
  g_export_seeds[slot].clear();  // fnxf_xiii (2026-10-24)
  ReleaseSeedBlocksLocked(slot);  // fnxf_xiii (2026-10-28)
  c = Completion{};
  WatchCheckLocked(slot == 0 ? "after the memexport results of slot 0" : slot == 1 ? "after the memexport results of slot 1"
                                                                                   : "after the memexport results");
  RebuildExportGranules();
  // fnxf_xiii (2026-10-17): guest memory changed under the draws: cached copies are proven again
  // (fnxf_xiii_renut_exact_streams), as after a kick.
  ++g_flush_serial;
}

// Up to 'budget' pages of the completion; true once it is finished (FinishCompletionLocked).
bool StepCompletionLocked(size_t budget, bool idle, const char *why) {
  Completion &c = g_completion;
  if (c.slot < 0)
    return true;
  SlotExports &exports = g_exports[c.slot];
  const auto t0 = std::chrono::steady_clock::now();
  while (c.data && c.window < exports.pending.size() && budget) {
    const PendingExport &p = exports.pending[c.window];
    if (!c.jobs_built) {
      c.guest = GuestPhysicalPtr<u8>(p.base);
      if (!c.guest) {
        ++c.window;
        continue;
      }
      BuildCompletionJobsLocked(c, p);
      c.jobs_built = true;
      c.next_job = 0;
      c.window_merged = 0;
    }
    if (c.next_job < g_copy_jobs.size()) {
      const size_t left = g_copy_jobs.size() - c.next_job;
      const size_t end = c.next_job + std::min(left, budget);
      c.window_merged += RunCopyJobs(c.readback, c.next_job, end);
      budget -= end - c.next_job;
      c.next_job = end;
      if (c.next_job < g_copy_jobs.size())
        break;
    }
    FinishCompletionWindowLocked(c, p);
    ++c.window;
    c.jobs_built = false;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  c.ms += ms;
  if (idle) {
    c.idle_ms += ms;
    g_burst.idle_copy_ms += ms;
  } else {
    g_burst.copy_ms += ms;  // fnxf_xiii (2026-10-27)
  }
  g_burst.mask_bytes += g_mask_copied_bytes.exchange(0, std::memory_order_relaxed);  // fnxf_xiii (2026-10-29)
  if (c.data && c.window < exports.pending.size())
    return false;
  FinishCompletionLocked(c, why);
  return true;
}

void StartCompletionLocked(u32 slot, bool deferred) {
  auto &s = State();
  Completion &c = g_completion;
  c = Completion{};
  c.slot = int(slot);
  c.deferred = deferred;
  c.deferred_at = std::chrono::steady_clock::now();
  // fnxf_xiii (2026-10-24, fnxf_xiii_deko3d_direct_exports): from the export buffer itself, the slot's fence waited (its
  // signal flushes the GPU's caches); the buffer is the slot's newest (a grown one got the older windows by a copy in
  // the same list).
  c.readback = DirectExports() ? s.memexport_buffer[slot].get() : s.memexport_readback[slot].get();
  const plume::RenderRange no_range(0, 0);  // the mapping only: each window's lines are invalidated as it is taken
  c.data = c.readback ? static_cast<const u8 *>(c.readback->map(0, &no_range)) : nullptr;
  c.merged_before = g_completion_merged_pages;
  // fnxf_xiii (2026-10-29, fnxf_xiii_memexport_write_mask): the slot's seed blocks were the seeds' staging only (the
  // GPU's copies of them ran before the slot's fence): back to the pool now, not when the completion ends.
  if (ExportMaskOn())
    ReleaseSeedBlocksLocked(slot);
  // A deferred one's page flags read now (their lines invalidated), for FinishDeferredOverLocked.
  if (deferred && c.data && DirectExports()) {
    for (const PendingExport &p : g_exports[slot].pending) {
      if (const uint64_t bytes = ExportFlagBytes(p.size)) {
        const uint64_t at = ExportFlagOffset(p.offset, p.size);
        const plume::RenderRange flag_range(at, at + bytes);
        c.readback->map(0, &flag_range);
      }
    }
  }
}

int DeferredSlotLocked() {
  return g_completion.slot >= 0 && g_completion.deferred ? g_completion.slot : -1;
}

uint64_t g_zero_copy_co_written = 0;  // fnxf_xiii (2026-10-30): pages the game and the kernels wrote in one frame
// fnxf_xiii (2026-10-30, ZeroCopyOn): the slot's results are in guest memory already (its fence waited): the CPU's lines
// of the pages its kernels wrote (the slot's page flags) are dropped, so the CPU reads them, and a completion's
// bookkeeping follows (the cached copies' intervals, the pending windows, the granules). Nothing is copied.
void CompleteZeroCopyLocked(u32 slot) {
  const rtw::Scope rtw_mx(rtw::kMxComplete);  // fnxf_xiii (2026-11-05, [rtwork])
  const rtw::Scope rtw_mx_outside(rtw::g_in_draw ? rtw::kUnused : rtw::kMxCompleteOutside);
  SlotExports &exports = g_exports[slot];
  if (exports.pending.empty())
    return;
  const auto t0 = std::chrono::steady_clock::now();
  plume::RenderBuffer *flags = g_zero_copy_flags[slot].get();
  const u32 *words = nullptr;
  if (flags) {
    const plume::RenderRange none(0, 0);
    words = static_cast<const u32 *>(flags->map(0, &none));
  }
  uint64_t pages = 0, bytes = 0, windows = 0, co_written = 0;
  const bool cards = fnxf_xiii::renut_gpu::StoreCardsOn();
  for (const PendingExport &p : exports.pending) {
    ++windows;
    ++g_export_wait_windows;
    const u32 first = p.base >> 12, last = u32((uint64_t(p.base) + p.size - 1) >> 12);
    if (words) {
      const plume::RenderRange range(uint64_t(first) * 4, uint64_t(last + 1) * 4);
      flags->map(0, &range);  // dc civac of the flags' lines: the GPU wrote them
    }
    for (u32 pg = first; pg <= last;) {
      if (words && words[pg] == 0) {
        ++pg;
        continue;
      }
      u32 q = pg;
      while (q <= last && (!words || words[q] != 0)) {
        // A page the game wrote too while the kernels ran (the cards since the window was prepared): a cache line both
        // wrote keeps the CPU's bytes (the drop below writes a dirty line back first), as the 360's CPU cache would.
        // Counted and logged: the game keeps them apart on the console, and this says whether it does here.
        if (cards && p.seed_interval && HarvestPageZcLocked(q) >= p.seed_interval)
          ++co_written;
        ++q;
      }
      const uint64_t lo = std::max<uint64_t>(p.base, uint64_t(pg) << 12),
                     hi = std::min<uint64_t>(uint64_t(p.base) + p.size, uint64_t(q) << 12);
      guest_gpu::InvalidateAfterGpu(u32(lo), u32(hi - lo));
      pages += q - pg;
      bytes += hi - lo;
      pg = q;
    }
    g_completion_bytes += p.size;
    // fnxf_xiii (2026-10-27): written in this interval for the cached copies, not as the game's write.
    WrotePagesLocked(p.base, p.size);
  }
  g_export_wait_pages += pages;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  g_burst.copy_ms += ms;
  g_completion_ms_sum += ms;
  g_completion_longest_ms = std::max(g_completion_longest_ms, ms);
  ++g_completions;
  static uint32_t slow_logged = 0;
  if (ms >= 5.0 && slow_logged++ < 48)
    FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] results of slot {} in guest memory: {} pages of {} windows ({:.1f} MB) dropped from the "
                        "CPU's cache in {:.1f} ms; {} of the pages were written by the game too while the kernels ran",
                        slot, pages, windows, double(bytes) / (1 << 20), ms, co_written);
  g_zero_copy_co_written += co_written;
  static uint32_t co_logged = 0;
  if (co_written && co_logged++ < 16)
    RGPU_INFO("[export] slot {}: {} pages written by both the game and its kernels in the same frame ({} so far): a "
              "cache line both wrote keeps the game's bytes (fnxf_xiii_memexport_zero_copy)",
              slot, co_written, g_zero_copy_co_written);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_completion_last_log >= std::chrono::seconds(10)) {
    if (REXCVAR_GET(log_diagnostics))
      RGPU_INFO("[export] results in guest memory in 10 s: {} completions, {:.1f} MB of windows, {:.1f} ms of cache "
                "maintenance, longest {:.1f} ms (fnxf_xiii_memexport_zero_copy)",
                g_completions, double(g_completion_bytes) / (1 << 20), g_completion_ms_sum, g_completion_longest_ms);
    g_completion_bytes = g_completions = 0;
    g_completion_ms_sum = g_completion_longest_ms = 0.0;
    g_completion_last_log = now;
  }
  exports.pending.clear();
  exports.used = 0;
  g_zero_copy_written[slot].clear();  // in guest memory for every reader now
  WatchCheckLocked(slot == 0 ? "after the memexport results of slot 0" : slot == 1 ? "after the memexport results of slot 1"
                                                                                   : "after the memexport results");
  RebuildExportGranules();
  ++g_flush_serial;  // guest memory changed under the draws: cached copies are proven again
}

void FinishDeferredLocked(const char *why) {
  if (g_completion.slot >= 0)
    StepCompletionLocked(SIZE_MAX, false, why);
}

// fnxf_xiii (2026-10-28): a deferred completion whose windows overlap [base, base + size) (physical) is finished first:
// the range's reader then finds the results in guest memory, as it would have at the slot's fence.
// Only a page a kernel wrote counts (its page flag): the others are in guest memory as they are to be.
void FinishDeferredOverLocked(u32 base, u32 size) {
  const Completion &c = g_completion;
  if (c.slot < 0 || !c.deferred)
    return;
  for (const PendingExport &p : g_exports[c.slot].pending) {
    if (!(base < p.base + p.size && p.base < base + size))
      continue;
    if (!c.data || !DirectExports() || !ExportFlagBytes(p.size)) {
      FinishDeferredLocked("a draw read their range");
      return;
    }
    const u32 *flags = reinterpret_cast<const u32 *>(c.data + ExportFlagOffset(p.offset, p.size));
    const u32 first = (std::max(base, p.base) - p.base) >> 12;
    const u32 last = (std::min<uint64_t>(uint64_t(base) + size, uint64_t(p.base) + p.size) - 1 - p.base) >> 12;
    for (u32 page = first; page <= last; ++page) {
      if (flags[page]) {
        FinishDeferredLocked("a draw read a page they wrote");
        return;
      }
    }
  }
}

namespace detail {

void ReleaseCacheCopiesLocked(uint32_t slot) {
  g_copy_pool.ReleaseSlot(slot);
}

void CompleteMemExportsLocked(uint32_t slot) {
  if (ZeroCopyOn()) {  // fnxf_xiii (2026-10-30): in guest memory already; never deferred
    CompleteZeroCopyLocked(slot);
    return;
  }
  // fnxf_xiii (2026-10-28): a deferred one first (the results land in the slots' order); if it was this slot's, done.
  if (g_completion.slot >= 0) {
    const bool same = g_completion.slot == int(slot);
    FinishDeferredLocked(same ? "the slot's results were needed (a wait for them)" : "the next slot's fence");
    if (same)
      return;
  }
  // fnxf_xiii (2026-10-28): the slot's fence was waited: every seed it recorded has run (KeptWindow::unexecuted).
  for (KeptWindow &k : g_kept_windows[slot])
    k.unexecuted = false;
  if (g_exports[slot].pending.empty()) {
    ReleaseSeedBlocksLocked(slot);
    return;
  }
  StartCompletionLocked(slot, false);
  StepCompletionLocked(SIZE_MAX, false, "at once");
}

// fnxf_xiii (2026-10-28, fnxf_xiii_renut_deferred_results): the slot's fence was waited and its next frame is about to be
// recorded: its results go to guest memory in the render thread's idle time (StepDeferredMemExportsIdle) instead of
// now, and at the latest when something needs them: an export draw of the slot (its buffers and seeds are reused), a
// draw reading their range (PendingExportSlots, FindExportWindow), a wait for them (WaitForExportsLocked) or the other
// slot's fence (CompleteMemExportsLocked). Until then they are pending as before (their windows are on the GPU).
void DeferMemExportsLocked(uint32_t slot) {
  if (!REXCVAR_GET(fnxf_xiii_renut_deferred_results) || ZeroCopyOn()) {  // fnxf_xiii (2026-10-30): a cache drop only
    CompleteMemExportsLocked(slot);
    return;
  }
  if (g_completion.slot >= 0)
    FinishDeferredLocked("the next slot's fence");
  for (KeptWindow &k : g_kept_windows[slot])
    k.unexecuted = false;
  if (g_exports[slot].pending.empty()) {
    ReleaseSeedBlocksLocked(slot);
    return;
  }
  StartCompletionLocked(slot, true);
  render_thread::RequestIdleWork();
}

} // namespace detail

// fnxf_xiii (2026-10-28): the render thread's idle step (render_thread.cpp, Run): a part of a deferred completion.
bool StepDeferredMemExportsIdle() {
  std::lock_guard lock(State().mutex);
  if (g_completion.slot < 0)
    return false;
  constexpr size_t kIdlePages = 512;  // 2 MB: about a millisecond with the helpers
  return !StepCompletionLocked(kIdlePages, true, "in the render thread's idle time");
}


void ForgetVertexDeclarationLocked(u32 declaration_va) {
  if (declaration_va == g_last_declaration_va) {
    g_last_declaration_va = 0;
    g_last_declaration = nullptr;
  }
}

// fnxf_xiii: the export buffers (export windows, readbacks, scratch) of a burst of exports (the game exports only while
// it loads: its texture kernels) are released once nothing has exported for kExportIdleFrames frames: they grow to the
// burst's largest frame and were kept for the rest of the run.
void ReleaseIdleExportBuffersLocked() {
  // fnxf_xiii (2026-10-24): 300 frames. It was raised to 1800 earlier that day (the next burst of a load made the blocks
  // again, 3 % of the render thread in those frames), and taken back: kept for a minute, the 200-360 MB of a burst's buffers stayed through the next
  // area's load, whose allocations went above them: the heap's top climbed to its end and the next burst's blocks no
  // longer fitted anywhere (NO MEMORY, from Start 2 of the 2026-10-24 tests on; the 2026-10-22 runs, at 300, did not).
  constexpr uint64_t kExportIdleFrames = 300;
  auto &s = State();
  const uint64_t frame = TextureFrameForLog();
  if (frame - g_last_export_frame < kExportIdleFrames)
    return;
  uint64_t released = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (!g_exports[slot].pending.empty())
      return;
  }
  // fnxf_xiii (2026-10-28): kept while the heap has room, checked again every kExportIdleFrames: the next area load's
  // first burst frame made them again (the 128 MB export buffers 12-15 ms each, about 75 ms of buffers in all) and seeded
  // its windows whole from guest memory (96 MB, about 50 ms), where kept ones seed only the pages changed since
  // (KeptWindow). They are in the buffer arena when it had room (fnxf_xiii_deko3d_buffer_arena_mb), not where the heap's
  // other allocations grow; the 2026-10-24 failure above was buffers kept with the heap nearly full.
  {
    constexpr unsigned long long kKeepRoom = 384ull << 20;
    const unsigned long long room = HeapRoomBytes();
    uint64_t held = 0;
    for (u32 slot = 0; slot < kFramesInFlight; ++slot)
      held += (s.memexport_capacity[slot] > (4u << 20) ? s.memexport_capacity[slot] : 0) +
              s.memexport_readback_capacity[slot] + s.memexport_scratch_capacity[slot];
    held += uint64_t(g_seed_chunks.size()) * kSeedChunkBlocks * kSeedBlockBytes;
    uint64_t arena_used = 0, arena_total = 0;
    uint32_t arena_fallbacks = 0;
#if defined(PLUME_DEKO3D)
    plume::Deko3DBufferArenaUse(arena_used, arena_total, arena_fallbacks);
#endif
    const bool keep = room >= kKeepRoom;
    static uint32_t logged = 0;
    if (held && logged++ < 32)
      FNXF_XIII_BURST_LOG(RGPU_INFO, "[export] idle for {} frames: {} MB of export buffers {} (heap room {} MB, {} needed to keep them; "
                "buffer arena {} of {} MB in use, {} blocks it could not hold so far)",
                kExportIdleFrames, held >> 20, keep ? "kept" : "released", room >> 20, kKeepRoom >> 20, arena_used,
                arena_total, arena_fallbacks);
    if (keep) {
      g_last_export_frame = frame;  // checked again in kExportIdleFrames frames
      return;
    }
  }
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    // The export buffer every pipeline binds (DrawLocked) is made again at once at its smallest (4 MB): that one
    // stays (it was released and made again every 300 frames).
    const bool keep_buffer = s.memexport_capacity[slot] <= (4u << 20);
    released += (keep_buffer ? 0 : s.memexport_capacity[slot]) + s.memexport_readback_capacity[slot] +
                s.memexport_scratch_capacity[slot];
    if (s.memexport_buffer[slot] && !keep_buffer) {
      Video::RetireLocked(std::move(s.memexport_buffer[slot]));
      ForgetKeptWindowsLocked(slot);  // fnxf_xiii (2026-10-28)
    }
    if (s.memexport_readback[slot])
      Video::RetireLocked(std::move(s.memexport_readback[slot]));
    if (s.memexport_scratch[slot])
      Video::RetireLocked(std::move(s.memexport_scratch[slot]));
    if (!keep_buffer)
      s.memexport_capacity[slot] = 0;
    s.memexport_readback_capacity[slot] = s.memexport_scratch_capacity[slot] = 0;
    g_exports[slot].used = 0;
    g_mask_ranges[slot].clear();  // fnxf_xiii (2026-10-29): none pending (every list of the slot has run)
  }
  // fnxf_xiii (2026-10-28): the seed blocks too (none in use: no slot has exports pending).
  bool seeds_in_use = false;
  for (const auto &slot_used : g_seed_used)
    for (const auto &used : slot_used)
      seeds_in_use |= !used.empty();
  if (!seeds_in_use) {
    released += uint64_t(g_seed_free[0].size()) * kSeedBlockBytes;
    g_seed_free[0].clear();
    for (auto &chunk : g_seed_chunks)
      Video::RetireLocked(std::move(chunk));
    g_seed_chunks.clear();
  }
  if (released && false)
    RGPU_INFO("[export] idle for {} frames: {:.0f} MB of export buffers released", kExportIdleFrames,
              double(released) / (1 << 20));
  g_last_export_frame = frame;  // checked again in kExportIdleFrames frames
}

void AdvanceStreamCacheFrameLocked() {
  namespace ft = fnxf_xiii::frame_timing;
  std::optional<ft::PartScope> part;
  part.emplace(ft::kPartStreamStart);
  fnxf_xiii::renut_gpu::ResolveTraceFrameStart();
  MotionFrameStartLocked();  // fnxf_xiii (2026-11-06, fnxf_xiii_motion_vectors)
  GuestGpuProbeFrameLocked();  // fnxf_xiii (2026-10-30, fnxf_xiii_diag_gpu_guest_probe): nothing unless asked
  // fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy): the mapping of guest memory, made at the first frame with no
  // exports pending (a pending window's offset is in the slot's buffer until it completes; afterwards it is an address).
  if (!AnyPendingExports() && DeferredSlotLocked() < 0)
    guest_gpu::FrameLocked(State());
  fnxf_xiii::renut_gpu::HookTimeReport(TextureFrameForLog());  // fnxf_xiii (2026-10-28, fnxf_xiii_diag_hook_time)
  CpuReadReportLocked();  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_cpu_reads)
  ColourHandOverReportLocked();  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): with log_diagnostics
  fnxf_xiii::LoadWatchTick(TextureFrameForLog());  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_load_watch)
  // fnxf_xiii (2026-10-27): a long render-thread frame with memexport work, by part.
  part.reset();
  part.emplace(ft::kPartStreamBurst);
  {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    static uint32_t logged = 0;
    // fnxf_xiii (2026-10-28): the frame's time also by what the render thread did: working, blocked (on the GPU's fences,
    // the display), or idle (waiting for the game's next commands); and the kept windows' share of the seeds.
    const double busy_ms = render_thread::BusyMsThisFrame(), waited_ms = render_thread::WaitedMsThisFrame();
    // fnxf_xiii (2026-10-28): and how long the game thread waited for the renderer (its frame queue, the fences it
    // polls), and the results copied in the render thread's idle time (fnxf_xiii_renut_deferred_results).
    const double game_wait_ms = renut::frame_stats::TakeGameThreadWaitMs();
    // fnxf_xiii (2026-10-28): the parts of "the rest" that could be large in a burst's first frames: making buffers
    // and images (their memory), pipelines and shaders, and uploading textures.
    double make_buffers_ms = 0.0, make_images_ms = 0.0, make_pipelines_ms = 0.0, shader_ms = 0.0;
#if defined(PLUME_DEKO3D)
    {
      static uint64_t last[4] = {};
      plume::Deko3DStats &st = plume::GetDeko3DStats();
      const uint64_t now_ns[4] = {st.bufferCreateNs.load(), st.textureCreateNs.load(), st.pipelineCreateNs.load(),
                                  st.shaderCompileNs.load()};
      double *out[4] = {&make_buffers_ms, &make_images_ms, &make_pipelines_ms, &shader_ms};
      for (int i = 0; i < 4; ++i) {
        *out[i] = double(now_ns[i] - last[i]) / 1e6;
        last[i] = now_ns[i];
      }
    }
#endif
    const double upload_ms = double(TakeTextureUploadNs()) / 1e6;
    // fnxf_xiii (2026-10-28): which memory blocks were slow to make, and which textures to upload (every frame's, so a
    // [burst] line has only its own frame's).
    const std::string upload_report = TakeTextureUploadReport();
#if defined(PLUME_DEKO3D)
    const std::string slow_blocks = plume::Deko3DTakeSlowBlocks();
#else
    const std::string slow_blocks;
#endif
    // fnxf_xiii (2026-10-30, fnxf_xiii_memexport_zero_copy): the windows in guest memory: the frame's cache cleans before
    // the kernels and drops after them, in place of the seeds and the copies.
    const guest_gpu::Stats zc = guest_gpu::TakeStats();
    const std::string zero_copy =
        ZeroCopyOn() ? fmt::format("; windows in guest memory: {} fresh, {:.1f} MB cleaned for the GPU in {:.1f} ms ({} pages "
                                   "skipped by the cards), {:.1f} MB dropped from the CPU's cache after it in {:.1f} ms; "
                                   "pending ranges read in place {} (copies' outputs) + {} (shader windows the game left "
                                   "alone), copied as recorded {}",
                                   g_burst.zero_copy_windows, double(zc.clean_bytes) / (1 << 20), zc.clean_ms,
                                   zc.clean_skipped_pages, double(zc.invalidate_bytes) / (1 << 20), zc.invalidate_ms,
                                   g_zero_copy_reads_gpu, g_zero_copy_reads_shader_gpu, g_zero_copy_reads_snapshot)
                     : std::string();
    g_zero_copy_reads_gpu = g_zero_copy_reads_shader_gpu = g_zero_copy_reads_snapshot = 0;
    if (frame_ms >= 50.0 && (g_burst.seeds || g_burst.copy_ms > 0.0 || g_burst.idle_copy_ms > 0.0 ||
                             g_kept_stats.windows || g_burst.zero_copy_windows || zc.clean_bytes) &&
        logged++ < 96)
      FNXF_XIII_BURST_LOG(RGPU_INFO, "[burst] frame {}: {:.1f} ms (render thread working {:.1f} ms, blocked {:.1f} ms, idle {:.1f} ms; the "
                "game waited for the renderer {:.1f} ms): seeds from guest memory {:.1f} ms ({:.0f} MB, {} of {} pieces "
                "with the helpers; {} windows kept "
                "in the export buffer, {:.0f} MB of their pages unchanged, {} compared, {} differed), results to guest "
                "memory {:.1f} ms when needed and {:.1f} ms in idle time ({:.1f} MB by the write mask), waiting for the "
                "GPU {:.1f} ms, the rest of "
                "the work {:.1f} ms (made on any thread: buffers {:.1f} ms, images {:.1f} ms, pipelines {:.1f} ms, shaders "
                "{:.1f} ms; texture uploads {:.1f} ms){}",
                TextureFrameForLog(), frame_ms, busy_ms, waited_ms, std::max(0.0, frame_ms - busy_ms - waited_ms),
                game_wait_ms, g_burst.seed_ms, double(g_burst.seed_bytes) / (1 << 20), g_burst.parallel_seeds,
                g_burst.seeds, g_kept_stats.windows,
                double(g_kept_stats.kept_bytes) / (1 << 20), g_kept_stats.checked, g_kept_stats.mismatched,
                g_burst.copy_ms, g_burst.idle_copy_ms, double(g_burst.mask_bytes) / (1 << 20), g_burst.gpu_wait_ms,
                std::max(0.0, busy_ms - g_burst.seed_ms - g_burst.copy_ms - zc.clean_ms), make_buffers_ms, make_images_ms,
                make_pipelines_ms, shader_ms, upload_ms, zero_copy);
    if (frame_ms >= 50.0 && logged <= 96 && (!slow_blocks.empty() || upload_ms >= 5.0)) {
      // fnxf_xiii (2026-10-28): the frame's slow memory blocks and texture uploads (any long frame, not only bursts).
      static uint32_t detail_logged = 0;
      if (detail_logged++ < 160) {
        if (!slow_blocks.empty())
          FNXF_XIII_BURST_LOG(RGPU_INFO, "[burst]   frame {}: memory blocks slow to make: {}", TextureFrameForLog(), slow_blocks);
        if (upload_ms >= 5.0)
          FNXF_XIII_BURST_LOG(RGPU_INFO, "[burst]   frame {}: texture uploads: {}", TextureFrameForLog(), upload_report);
      }
    }
    g_burst = {};
    g_kept_stats = {};
  }
  part.reset();
  part.emplace(ft::kPartStreamSweep);
  CardWriterReportLocked();  // fnxf_xiii (2026-10-27, fnxf_xiii_diag_card_writers)
  WatchCheckLocked("at a frame's start: written by the game (CPU, file loads) since the last check");  // fnxf_xiii
  ReleaseIdleExportBuffersLocked();  // fnxf_xiii
  SweepUnusedTargetsLocked();        // fnxf_xiii (2026-10-24, fnxf_xiii_renut_keep_unused_targets)
  // fnxf_xiii (2026-10-19): fnxf_xiii_renut_store_cards_verify's diagnostics, every 10 s.
  if (fnxf_xiii::renut_gpu::StoreCardsVerifyOn()) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(10)) {
      last = now;
      uint64_t host[3] = {};
#if !defined(__SWITCH__)
      for (int i = 0; i < 3; ++i)
        host[i] = RexStoreCardsHostBytes(i);
#endif
      // Written cards in each view's part of the table (0: written): A, C, E (512 MB each) and 7F (16 MB).
      uint32_t zero[4] = {};
      if (const u8 *cards = StoreCards()) {
        for (u32 i = 0; i < 0x20000u; ++i) {
          zero[0] += cards[0xA0000u + i] == 0;
          zero[1] += cards[0xC0000u + i] == 0;
          zero[2] += cards[0xE0000u + i] == 0;
        }
        for (u32 i = 0; i < 0x1000u; ++i)
          zero[3] += cards[0x7F000u + i] == 0;
      }
      std::string samples = fmt::format(" (written cards now: A {} C {} E {} 7F {} of 131072/131072/131072/4096)",
                                        zero[0], zero[1], zero[2], zero[3]);
      for (uint32_t i = 0; i < g_card_sample_count; ++i)
        samples += fmt::format(" {:05X}{}{}{}{}", g_card_samples[i] & 0xFFFFFFF, (g_card_samples[i] >> 28) & 1 ? "a" : "",
                               (g_card_samples[i] >> 29) & 1 ? "c" : "", (g_card_samples[i] >> 30) & 1 ? "e" : "",
                               (g_card_samples[i] >> 31) & 1 ? "f" : "");
      RGPU_INFO("[cards] 10 s: {} pages read, {} found written (view A {}, C {}, E {}, 7F {}); {} copies written only "
                "through the page before; {} hashed after a written page with their bytes unchanged; host writers "
                "marked {:.1f} MB of the physical views and {:.1f} MB elsewhere in {} calls; {} checks of memory outside the "
                "physical views; first pages:{}",
                g_card_diag[0], g_card_diag[1], g_card_diag[2], g_card_diag[3], g_card_diag[4], g_card_diag[5],
                g_card_diag[6], g_card_diag[7], double(host[0]) / double(1u << 20), double(host[1]) / double(1u << 20),
                host[2], g_card_diag[8], samples);
      std::fill(std::begin(g_card_diag), std::end(g_card_diag), 0);
      g_card_sample_count = 0;
    }
  }
  g_trace_draws = 0;
  part.reset();
  part.emplace(ft::kPartStreamVerify);
  // Copies whose guest bytes changed (ContentVerifier), unless made again
  // since they were checked.
  static std::vector<decltype(g_stream_verifier)::Check> stale;
  g_stream_verifier.Flush();
  g_stream_verifier.TakeStale(stale);
  for (const auto &check : stale) {
    auto it = g_stream_cache.find(check.key);
    if (it == g_stream_cache.end())
      continue;
    CachedStream &c = it->second;
    if (!c.copy || c.hash != check.hash)
      continue;
    DropCachedCopy(c);
    c.stable = 0;
    c.checked_frame = UINT64_MAX;
    c.dynamic = ++c.drops >= kMaxCacheDrops;
    fnxf_xiii::renut_gpu::CountLateStale(1);  // fnxf_xiii: drawn from its old copy until now
  }
  stale.clear();

  static std::vector<decltype(g_index_verifier)::Check> stale_indices;
  g_index_verifier.Flush();
  g_index_verifier.TakeStale(stale_indices);
  for (const auto &check : stale_indices) {
    auto it = g_index_cache.find(check.key);
    if (it == g_index_cache.end())
      continue;
    CachedIndices &c = it->second;
    if (!c.copy || c.hash != check.hash)
      continue;
    DropCachedIndices(c);
    c.stable = 0;
    c.checked_frame = UINT64_MAX;  // hashed again by TrackIndices
    c.dynamic = ++c.drops >= kMaxCacheDrops;
    fnxf_xiii::renut_gpu::CountLateStale(1);  // fnxf_xiii: drawn from its old copy until now
  }
  stale_indices.clear();

  part.reset();
  if (++g_stream_cache_frame % 60)
    return;
  part.emplace(ft::kPartStreamIdleDrop);
  // The memos may hold entries about to go.
  g_stream_memo.Clear();
  g_index_memo.Clear();
  // fnxf_xiii: fnxf_xiii_renut_memory_policy's idle time (Renut's kStreamCacheIdleFrames without it).
  const uint64_t idle_frames = fnxf_xiii::renut_gpu::StreamCacheIdleFrames(kStreamCacheIdleFrames);
  std::erase_if(g_stream_cache, [idle_frames](auto &entry) {
    if (g_stream_cache_frame - entry.second.last_used < idle_frames)
      return false;
    DropCachedCopy(entry.second);
    return true;
  });
  std::erase_if(g_index_cache, [idle_frames](auto &entry) {
    if (g_stream_cache_frame - entry.second.last_used < idle_frames)
      return false;
    DropCachedIndices(entry.second);
    return true;
  });
}

StreamCacheMemory StreamCacheMemoryLocked() {
  StreamCacheMemory usage;
  usage.pool_bytes = g_copy_pool.Bytes();
  usage.cached_bytes = g_stream_cache_bytes;
  usage.streams = g_stream_cache.size();
  usage.index_ranges = g_index_cache.size();
  return usage;
}

namespace {

// Clears on explicit targets (framebuffer bound as for a draw). A color clear
// reaches the targets over the same EDRAM in other host formats too
// (ColorAliasesLocked): on the console the clear writes the memory, whatever
// format a surface reads it as. N&B's paint shop clears its glow buffer
// through the 8-bit view of that EDRAM, then adds the glow particles through a
// 16-bit float view; the float target kept the frames before, and the glow
// piled up into drips.
void ClearTargets(const Targets &t, u32 flags, const plume::RenderRect &rect, const float color[4],
                  float depth, u32 stencil) {
  if (fnxf_xiii::renut_gpu::ResolveTraceOn()) {
    RGPU_INFO("[trace] clear {:X} rect ({},{})-({},{}) of {}x{}", flags, rect.left, rect.top, rect.right, rect.bottom,
              t.width, t.height);
  }
  if (rect.right <= rect.left || rect.bottom <= rect.top)
    return;
  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover): before the target is bound for the clear.
  if (t.depth && t.depth->handed)
    DepthHandOverBeforeBindLocked(t.depth, &rect, flags);
  if (t.depth && (flags & 0x20))
    t.depth->stencil_used = true;
  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): a colour target cleared while a resolve destination holds its content:
  // the destination takes the image at a clear of the whole target, else the resolve is drawn first.
  if (!g_colour_tracked.empty()) {
    for (uint32_t i = 0; i < t.color_count; ++i) {
      HostSurface *c = t.colors[i];
      if (!c || !(flags & (1u << i)) || !c->handed || c->depth)
        continue;
      const bool whole = rect.left <= 0 && rect.top <= 0 && rect.right >= int32_t(c->width) &&
                         rect.bottom >= int32_t(c->height);
      g_colour_mat_why = 9;
      ColourHandOverBeforeWriteLocked(c, whole ? 1 : 0);
    }
  }
  g_colour_ho_draw_checked = true;
  const bool clear_bound = BindTargets(t);
  g_colour_ho_draw_checked = false;
  if (!clear_bound)
    return;
  plume::RenderCommandList *cmd = State().command_list;
  const plume::RenderRect host_rect = ScaleRect(rect, t.scale);
  const plume::RenderColor clear_color(color[0], color[1], color[2], color[3]);
  // fnxf_xiii (2026-10-19, render-thread proposal 20): scratch vectors kept between clears (render thread only).
  static std::vector<HostSurface *> aliases;
  static std::vector<HostSurface *> found;
  aliases.clear();
  for (uint32_t i = 0; i < t.color_count; ++i) {
    if (!(flags & (1u << i)))
      continue;
    ++t.colors[i]->writes;  // fnxf_xiii (fnxf_xiii_renut_resolve_skip)
    EdramNoteClearLocked(t.colors[i]);  // fnxf_xiii (2026-10-31)
    cmd->clearColor(i, clear_color, &host_rect, 1);
    ColorAliasesLocked(t.colors[i], found);
    for (HostSurface *alias : found) {
      if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end() &&
          std::find(t.colors, t.colors + t.color_count, alias) == t.colors + t.color_count)
        aliases.push_back(alias);
    }
  }
  if (t.depth && (flags & 0x30)) {
    ++t.depth->writes;  // fnxf_xiii (fnxf_xiii_renut_resolve_skip)
    cmd->clearDepthStencil((flags & 0x10) != 0, (flags & 0x20) != 0, depth, stencil & 0xFF,
                           &host_rect, 1);
  }
  for (HostSurface *alias : aliases) {
    Targets a;
    a.colors[a.color_count++] = alias;
    a.width = alias->width;
    a.height = alias->height;
    a.scale = alias->scale;
    const plume::RenderRect alias_rect(
        std::min(rect.left, int32_t(alias->width)), std::min(rect.top, int32_t(alias->height)),
        std::min(rect.right, int32_t(alias->width)), std::min(rect.bottom, int32_t(alias->height)));
    if (alias_rect.right <= alias_rect.left || alias_rect.bottom <= alias_rect.top)
      continue;
    // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): the alias's own clear, seen to as the cleared target's above.
    if (alias->handed && !alias->depth && !g_colour_tracked.empty()) {
      const bool whole = alias_rect.left <= 0 && alias_rect.top <= 0 && alias_rect.right >= int32_t(alias->width) &&
                         alias_rect.bottom >= int32_t(alias->height);
      g_colour_mat_why = 9;
      ColourHandOverBeforeWriteLocked(alias, whole ? 1 : 0);
    }
    g_colour_ho_draw_checked = true;
    const bool alias_bound = BindTargets(a);
    g_colour_ho_draw_checked = false;
    if (!alias_bound)
      continue;
    const plume::RenderRect alias_host = ScaleRect(alias_rect, a.scale);
    ++alias->writes;  // fnxf_xiii (fnxf_xiii_renut_resolve_skip)
    cmd->clearColor(0, clear_color, &alias_host, 1);
  }
}

// Depth resolve (shaders/host/depth_resolve.hlsli): 'src' of the depth target
// to ('dest_x', 'dest_y') of the destination and of its guest words, in guest
// pixels (the destination is of the surface's scale).
bool ResolveDepth(HostSurface *surface, HostTexture *dest, const ClearRect &src, i32 dest_x,
                  i32 dest_y, bool words = true, uint32_t sample = 0) {
  const RenderScale scale = surface->scale;
  auto &s = State();
  const uint32_t variant = surface->samples != plume::RenderSampleCount::COUNT_1 ? 1 : 0;
  const uint32_t source_slot = SurfaceSampleSlotLocked(surface);
  if (!s.depth_resolve_float_pipeline[variant])
    words = true;  // fnxf_xiii (fnxf_xiii_renut_depth_words_lazy): no depth-only pipeline
  if (source_slot == kInvalidDescriptor || !dest->depth_framebuffer ||
      !s.depth_resolve_pipeline[variant]) {
    return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  gpu_profile::Mark(s, ResolveProfileLabel(surface, words ? 4 : 5));
  TransitionSurfaceLocked(surface, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COLOR_WRITE);
  TransitionTextureLocked(dest->words.get(), plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(dest->depth_framebuffer.get());
  // Pixel coordinates are destination texels.
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->alloc_width),
                                          float(dest->alloc_height)));
  cmd->setScissors(ScaleRect(plume::RenderRect(dest_x, dest_y, dest_x + (src.x2 - src.x1),
                                               dest_y + (src.y2 - src.y1)),
                             surface->scale));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(words ? s.depth_resolve_pipeline[variant].get() : s.depth_resolve_float_pipeline[variant].get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 2);  // fnxf_xiii: the stencil view, read as uint
  UtilityPush push{};
  push.scale[0] = float(u32(surface->samples));  // fnxf_xiii (2026-10-22): the sample count (ms_sample_deko3d.hlsli)
  push.scale[1] = float((sample < uint32_t(surface->samples) ? sample : 0u) + 1);  // fnxf_xiii: depth_resolve.hlsli's sample
  push.translate[0] = float(scale.Coord(src.x1) - scale.Coord(dest_x));
  push.translate[1] = float(scale.Coord(src.y1) - scale.Coord(dest_y));
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  // fnxf_xiii (2026-10-18, fnxf_xiii_renut_depth_words_stencil): the stencil in the words' low byte (depth_resolve.hlsli),
  // which N&B's depth of field reads to keep the characters sharp. The resolve reads no sampler: the index is the
  // stencil view's slot.
  if (fnxf_xiii::renut_gpu::DepthWordsStencilOn() && words) {
    if (const uint32_t stencil_slot = SurfaceStencilSlotLocked(surface); stencil_slot != kInvalidDescriptor) {
      push.sampler_index = stencil_slot;
      push.param1 = 1.0f;
    }
  }
  // D3DFMT_D24FS8 (GPU texture format 24_8_FLOAT) stores 20e4 float depth.
  push.param0 = GpuTextureFormat(surface->format_word & 0x3F) == GpuTextureFormat::Fmt24_8_Float
                    ? 1.0f
                    : 0.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);

  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

// ---------------------------------------------------------------------------
// Depth hand-over (fnxf_xiii, 2026-10-19, fnxf_xiii_deko3d_depth_handover)
// ---------------------------------------------------------------------------
// A depth resolve of a whole single-sampled depth target into a destination of its size, without the words
// (fnxf_xiii_renut_depth_words_lazy), is not drawn: the destination's content stays in the target's image (handed_from,
// with the generation and writes it had), and a bind of the destination samples that image through a depth view with
// the destination's component mapping. The float the resolve writes is the depth it loads (depth_resolve.hlsli), so the
// shaders read the same values. Before the target is bound again (a draw or a clear: BindTargets), or goes, the
// resolve is drawn after all (MaterialiseLocked), from the image still unchanged; so is it for a draw that has the
// target as its own depth target. A clear of the target's whole depth (and its stencil, or of a target whose stencil
// nothing uses) instead gives the image to the destination (held) and the destination's previous one, or a new one,
// to the target, which the clear then fills. The shadow maps are resolved once a frame, sampled by the scene and
// cleared whole at the start of the next frame's shadow pass: they are never copied.

std::vector<HostTexture *> g_handed;  // destinations whose content is a target's current image

void UnhandLocked(HostTexture &d) {
  if (!d.handed_from)
    return;
  if (d.handed_from->handed)
    --d.handed_from->handed;
  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): a colour target no destination holds any more is not tracked.
  if (!d.handed_from->handed && !d.handed_from->depth)
    g_colour_tracked.erase(d.handed_from);
  d.handed_from = nullptr;
  std::erase(g_handed, &d);
}

// fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): whether the content 'd' holds elsewhere is a colour target's.
bool HoldsColour(const HostTexture &d) {
  if (d.handed_from)
    return !d.handed_from->depth;
  return d.held && d.held_current && d.held->colour;
}
bool ColourMaterialiseLocked(HostTexture &d);
bool ColourTakeAtRetireLocked(HostTexture &d, HostSurface &s);

// Draws the resolve left out into the destination's own targets, from where its content is.
bool MaterialiseLocked(HostTexture &d, bool words) {
  if (HoldsColour(d))
    return ColourMaterialiseLocked(d);  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover)
  ++g_texture_epoch;
  const ClearRect src{d.last_src[0], d.last_src[1], d.last_src[2], d.last_src[3]};
  if (HostSurface *source = d.handed_from) {
    const bool same = source->texture && source->generation == d.handed_generation &&
                      source->writes == d.handed_writes;
    UnhandLocked(d);
    if (same && ResolveDepth(source, &d, src, d.last_dest_x, d.last_dest_y, words, d.last_sample)) {
      d.words_current = words;
      fnxf_xiii::renut_gpu::CountHandOver(3);
      return true;
    }
    fnxf_xiii::renut_gpu::CountHandOver(5);
    return false;
  }
  if (!d.held || !d.held_current)
    return false;
  d.held_current = false;
  // The taken image as the target it was taken from, for ResolveDepth.
  HostSurface image;
  image.width = d.width;
  image.height = d.height;
  image.scale = d.scale;
  image.depth = true;
  image.format = d.held->format;
  image.samples = d.held->samples;
  image.format_word = d.held->format_word;
  SwapDepthImageLocked(image, *d.held);
  const bool ok = ResolveDepth(&image, &d, src, d.last_dest_x, d.last_dest_y, words, d.last_sample);
  SwapDepthImageLocked(image, *d.held);
  if (ok) {
    d.words_current = words;
    fnxf_xiii::renut_gpu::CountHandOver(3);
    fnxf_xiii::renut_gpu::CountHandOver(4);
  } else {
    fnxf_xiii::renut_gpu::CountHandOver(5);
  }
  return ok;
}

// A resolve into 'd' left in the target's image when it can be (true). Otherwise (false, the resolve is drawn) a
// destination still holding other content gets it first when the resolve does not cover it.
bool HandOverResolveLocked(HostSurface *s, HostTexture *d, const ClearRect &src, i32 dest_x, i32 dest_y,
                           bool words) {
  const bool whole = src.x1 == 0 && src.y1 == 0 && src.x2 == int32_t(s->width) && src.y2 == int32_t(s->height) &&
                     dest_x == 0 && dest_y == 0;
  const bool can = IsDeko3D() && fnxf_xiii::renut_gpu::DepthHandOverOn() && !words && whole && s->texture &&
                   s->samples == plume::RenderSampleCount::COUNT_1 && d->width == s->width &&
                   d->height == s->height && d->scale == s->scale && d->alloc_width == s->scale.Size(s->width) &&
                   d->alloc_height == s->scale.Size(s->height) && d->dimension == TextureDimension::Tex2D &&
                   !d->stacked && d->extra_levels == 0 && d->mip_levels == 1;
  if (!can) {
    if (d->handed_from || d->held_current) {
      const bool covers = dest_x == 0 && dest_y == 0 && src.x2 - src.x1 >= int32_t(d->width) &&
                          src.y2 - src.y1 >= int32_t(d->height);
      if (covers) {
        UnhandLocked(*d);
        d->held_current = false;
      } else {
        MaterialiseLocked(*d, false);
      }
    }
    return false;
  }
  UnhandLocked(*d);  // what it held is overwritten
  d->held_current = false;
  d->handed_from = s;
  d->handed_generation = s->generation;
  d->handed_writes = s->writes;
  ++s->handed;
  g_handed.push_back(d);
  ++g_texture_epoch;
  fnxf_xiii::renut_gpu::CountHandOver(0);
  return true;
}

// The destination takes the target's image; the target takes the destination's previous one (or a new one).
bool TakeImageLocked(HostTexture &d, HostSurface &s) {
  if (d.held && (d.held->host_width != s.scale.Size(s.width) || d.held->host_height != s.scale.Size(s.height) ||
                 d.held->format != s.format || d.held->samples != s.samples)) {
    RetireDepthImageLocked(*d.held);
    d.held.reset();
  }
  if (!d.held) {
    auto image = std::make_shared<DepthImage>();
    if (!MakeDepthImageLocked(s, *image))
      return false;
    d.held = std::move(image);
  }
  UnhandLocked(d);
  SwapDepthImageLocked(s, *d.held);
  d.held_current = true;
  ++g_texture_epoch;
  fnxf_xiii::renut_gpu::CountHandOver(2);
  return true;
}

void DepthHandOverBeforeBindLocked(HostSurface *surface, const plume::RenderRect *clear_rect, u32 clear_flags) {
  if (!surface || !surface->handed)
    return;
  bool take = clear_rect && (clear_flags & 0x10) && ((clear_flags & 0x20) || !surface->stencil_used) &&
              clear_rect->left <= 0 && clear_rect->top <= 0 && clear_rect->right >= int32_t(surface->width) &&
              clear_rect->bottom >= int32_t(surface->height);
  std::vector<HostTexture *> holders;
  for (HostTexture *d : g_handed)
    if (d->handed_from == surface)
      holders.push_back(d);
  // The last resolve's destination takes the image; any other is drawn.
  for (size_t i = holders.size(); i-- > 0;) {
    HostTexture &d = *holders[i];
    if (take && surface->generation == d.handed_generation && surface->writes == d.handed_writes &&
        TakeImageLocked(d, *surface)) {
      take = false;
      continue;
    }
    MaterialiseLocked(d, false);
  }
}

// ---------------------------------------------------------------------------
// Colour hand-over (fnxf_xiii, 2026-11-17, fnxf_xiii_colour_handover)
// ---------------------------------------------------------------------------
// As the depth hand-over, for colour. A resolve of a whole single-sampled colour target into a 2D texture of its size
// and format is not drawn: the texture's content stays in the target's image (HostTexture::handed_from) and a bind of
// the texture samples that image through a view with the texture's own mapping (DepthHandOverSlotLocked). Before the
// target is written again (a draw with a colour write mask for it, a clear of it, an EDRAM carry into it), the texture
// either takes the image (the target gets the texture's previous one, or a new one: an exchange, nothing copied) or the
// resolve is drawn after all, from the image still unchanged (as it was drawn before, only later). The image is taken
// where the target's old content cannot matter to what follows:
// - a clear of the whole target's colour;
// - a draw that writes every pixel of it (ColourDrawOverwritesLocked): a 4-vertex quad, fan or strip over the whole
//   target through one of the game's pass-through vertex shaders (fnxf_xiii_colour_handover_fullscreen_vs), viewport and
//   scissor over the whole target, not blended, every channel written, no depth, stencil or alpha test, no kill in the
//   pixel shader. The game's post-processing passes are these: each resolves its target and draws into it again.
// The front buffer, when its resolve was left in the target's image, is shown from that image (FrontBufferImageLocked).
// Not done: the EDRAM cover, the part overwrite, the look-ahead, rows. Not for dynamic resolution's
// targets (their drawn part).

// 0 handed, 1 taken at a clear, 3 taken at an overwriting draw, 4 drawn after all, 5 lost, 6 new images, 7 draws that
// failed the overwrite test only at their vertices
uint64_t g_colour_ho_stats[9];
// With log_diagnostics: why a resolve left in its target's image was drawn after all, by the destination's size and
// format (g_colour_mat_why: 1 a bind that is no draw's or clear's own, 2 a second destination of the same target, 3 the
// take failed, 4 sampled by a draw into its own target, 5 no view, 6 an EDRAM carry, 7 a resolve of a part into the
// destination, 8 the front buffer, 9 a clear of a part, 10 FSR 3.1's copy, 11-18 a draw that is no overwrite (the
// test that failed), 21-23 a take whose destination no longer has the content, 0 anything else).
std::unordered_map<uint64_t, uint32_t> g_colour_mat_tally;

bool DrawResolveCopy(HostSurface *surface, HostTexture *dest, const plume::RenderRect &rect, uint32_t host_x,
                     uint32_t host_y, int32_t sample);

// The formats DrawResolveCopy writes back bit for bit (its own list).
bool ColourResolveDrawable(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R10G10B10A2_UNORM:
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R16_UNORM:
  case plume::RenderFormat::R16_FLOAT:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    return true;
  default:
    return false;
  }
}

// A colour resolve of host rect 'rect' of 's' to (host_x, host_y) of 'd' (level 0 of a 2D texture when 'level0') left
// in the target's image when it can be (true). Otherwise (false, the resolve is drawn) a destination still holding
// other content gets it first when the resolve does not cover it.
bool ColourHandOverResolveLocked(HostSurface *s, HostTexture *d, const plume::RenderRect &rect, uint32_t host_x,
                                 uint32_t host_y, bool level0) {
  static const bool on = REXCVAR_GET(fnxf_xiii_colour_handover);
  const uint32_t w = s->scale.Size(s->width), h = s->scale.Size(s->height);
  const bool whole = level0 && host_x == 0 && host_y == 0 && rect.left == 0 && rect.top == 0 &&
                     rect.right == int32_t(w) && rect.bottom == int32_t(h);
  const bool can = on && IsDeko3D() && fnxf_xiii::renut_gpu::DrawResolvesOn() && whole && s->texture && !s->depth &&
                   s->samples == plume::RenderSampleCount::COUNT_1 && !s->scale.drs && d->texture &&
                   d->render_target && d->host_format == s->format && d->dimension == TextureDimension::Tex2D &&
                   !d->stacked && d->extra_levels == 0 && d->mip_levels == 1 && d->alloc_width == w &&
                   d->alloc_height == h && d->width == s->width && d->height == s->height && d->scale == s->scale &&
                   !d->words && ColourResolveDrawable(s->format);
  if (!can) {
    if (d->handed_from || d->held_current) {
      const bool covers = level0 && host_x == 0 && host_y == 0 && rect.right - rect.left >= int32_t(d->alloc_width) &&
                          rect.bottom - rect.top >= int32_t(d->alloc_height);
      if (covers) {
        UnhandLocked(*d);
        d->held_current = false;
        ++g_texture_epoch;
      } else {
        g_colour_mat_why = 7;
        MaterialiseLocked(*d, false);
      }
    }
    return false;
  }
  UnhandLocked(*d);  // what it held is overwritten
  d->held_current = false;
  d->handed_from = s;
  d->handed_generation = s->generation;
  d->handed_writes = s->writes;
  ++s->handed;
  g_handed.push_back(d);
  ++g_texture_epoch;
  g_colour_tracked.insert(s);
  ++g_colour_ho_stats[0];
  return true;
}

// Draws the resolve left out into the destination's own texture, from where its content is.
bool ColourMaterialiseLocked(HostTexture &d) {
  ++g_texture_epoch;
  if (REXCVAR_GET(log_diagnostics))
    ++g_colour_mat_tally[uint64_t(g_colour_mat_why & 0xFF) << 56 | uint64_t(u32(d.host_format) & 0xFFFF) << 40 |
                         uint64_t(d.alloc_width & 0xFFFFF) << 20 | (d.alloc_height & 0xFFFFF)];
  g_colour_mat_why = 0;
  const plume::RenderRect rect(0, 0, int32_t(d.alloc_width), int32_t(d.alloc_height));
  bool ok = false;
  if (HostSurface *source = d.handed_from) {
    const bool same = source->texture && source->generation == d.handed_generation &&
                      source->writes == d.handed_writes;
    UnhandLocked(d);
    ok = same && DrawResolveCopy(source, &d, rect, 0, 0, -1);
  } else if (d.held && d.held_current) {
    d.held_current = false;
    // The taken image as the target it was taken from, for DrawResolveCopy: at its host size, scale 1
    // (2026-11-15: not d.scale, which SwapDepthImageLocked would multiply into the image's size).
    HostSurface image;
    image.width = d.held->host_width;
    image.height = d.held->host_height;
    image.scale = RenderScale{};
    image.depth = false;
    image.format = d.held->format;
    image.samples = d.held->samples;
    image.format_word = d.held->format_word;
    SwapDepthImageLocked(image, *d.held);
    ok = DrawResolveCopy(&image, &d, rect, 0, 0, -1);
    SwapDepthImageLocked(image, *d.held);
  }
  if (ok) {
    TransitionTextureLocked(&d, plume::RenderTextureLayout::SHADER_READ);
    ++g_colour_ho_stats[4];
  } else {
    d.resolve_signature = 0;  // its texture does not hold what the resolve would have written
    ++g_colour_ho_stats[5];
  }
  g_bound.framebuffer = nullptr;
  return ok;
}

// The destination takes the target's image; the target takes the destination's previous one (or a new one, cleared
// when it is first bound).
bool ColourTakeLocked(HostTexture &d, HostSurface &s) {
  if (d.held && (!d.held->colour || d.held->host_width != s.scale.Size(s.width) ||
                 d.held->host_height != s.scale.Size(s.height) || d.held->format != s.format ||
                 d.held->samples != s.samples)) {
    RetireDepthImageLocked(*d.held);
    d.held.reset();
  }
  bool fresh = false;
  if (!d.held) {
    auto image = std::make_shared<DepthImage>();
    if (!MakeDepthImageLocked(s, *image))
      return false;
    d.held = std::move(image);
    fresh = true;
    ++g_colour_ho_stats[6];
  }
  UnhandLocked(d);
  SwapDepthImageLocked(s, *d.held);
  d.held_current = true;
  ++g_texture_epoch;
  static const bool poison = REXCVAR_GET(fnxf_xiii_diag_colour_handover_poison);
  if (fresh || poison) {
    s.needs_clear = true;  // BindTargets: a new image starts transparent black (or, the test, any given one magenta)
    s.poison = poison;
  }
  return true;
}

// fnxf_xiii (2026-11-17): the target goes: the destination takes its image as it is (the target is left with the
// destination's previous one, or none, which go with it).
bool ColourTakeAtRetireLocked(HostTexture &d, HostSurface &s) {
  if (!d.held || !d.held->colour)
    d.held = std::make_shared<DepthImage>();  // empty: the target gets no image back
  else if (d.held->host_width != s.scale.Size(s.width) || d.held->host_height != s.scale.Size(s.height) ||
           d.held->format != s.format || d.held->samples != s.samples)
    d.held = std::make_shared<DepthImage>();  // the old one is retired with the target below (it gets it)
  UnhandLocked(d);
  SwapDepthImageLocked(s, *d.held);
  d.held->colour = true;
  d.held_current = true;
  ++g_texture_epoch;
  ++g_colour_ho_stats[2];
  return true;
}

// Before colour target 'surface' is written (a draw or a clear is about to bind it), or goes. 'take' 0: every
// destination holding its content gets the resolve drawn; 1 (a whole clear) or 3 (an overwriting draw): the last one
// takes the image, any other is drawn first.
void ColourHandOverBeforeWriteLocked(HostSurface *surface, int take) {
  if (!surface || !surface->handed || surface->depth)
    return;
  std::vector<HostTexture *> holders;
  for (HostTexture *d : g_handed)
    if (d->handed_from == surface)
      holders.push_back(d);
  HostTexture *taker = nullptr;
  if (take) {
    for (size_t i = holders.size(); i-- > 0;) {
      if (surface->generation == holders[i]->handed_generation && surface->writes == holders[i]->handed_writes) {
        taker = holders[i];
        break;
      }
    }
  }
  const int why = g_colour_mat_why ? g_colour_mat_why : 1;
  for (HostTexture *d : holders)
    if (d != taker) {
      g_colour_mat_why = taker ? 2 : why;
      ColourMaterialiseLocked(*d);
    }
  g_colour_mat_why = 0;
  if (taker) {
    if (ColourTakeLocked(*taker, *surface)) {
      ++g_colour_ho_stats[take & 3];
    } else {
      g_colour_mat_why = 3;
      ColourMaterialiseLocked(*taker);
    }
  }
}

// The vertex shaders of fnxf_xiii_colour_handover_fullscreen_vs (read once): -1 not listed, -2 the position passed through,
// n >= 0 ("hash:n") the position times the matrix in float constants n..n+3 (oPos = x c[n] + y c[n+1] + z c[n+2] +
// w c[n+3]: FF XIII's depth of field passes, an identity or a scale there).
int ColourPassThroughVs(uint64_t hash) {
  static const std::unordered_map<uint64_t, int> listed = [] {
    std::unordered_map<uint64_t, int> map;
    const std::string list = REXCVAR_GET(fnxf_xiii_colour_handover_fullscreen_vs);
    size_t at = 0;
    while (at < list.size()) {
      const size_t comma = list.find(',', at);
      const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      if (!item.empty()) {
        const size_t colon = item.find(':');
        const int reg = colon == std::string::npos ? -2 : std::atoi(item.c_str() + colon + 1);
        if (reg >= -2 && reg <= 252)
          map[std::strtoull(item.substr(0, colon).c_str(), nullptr, 16)] = reg;
      }
      if (comma == std::string::npos)
        break;
      at = comma + 1;
    }
    return map;
  }();
  const auto it = listed.find(hash);
  return it == listed.end() ? -1 : it->second;
}

// Float constant register 'r' of the draw's vertex shader as host floats.
void VsConstantLocked(const FloatConstants &c, u32 r, float out[4]) {
  const u8 *p = c.Register(r);
  for (u32 i = 0; i < 4; ++i) {
    u32 word;
    std::memcpy(&word, p + 4 * i, 4);
    word = __builtin_bswap32(word);
    std::memcpy(&out[i], &word, 4);
  }
}

// POSITION0 of vertex 'index' of the draw as its vertex shader gets it: a float2, float3 or float4 element, its
// components by the declaration's swizzle (0 and 1 for the ones the element does not hold).
bool DrawPositionLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                        const VertexDeclarationRecord &declaration, const u32 *shader_strides, u32 index,
                        float out[4]) {
  for (const GuestVertexElement &e : declaration.elements) {
    if (e.Usage != 0 || e.UsageIndex != 0)  // D3DDECLUSAGE_POSITION
      continue;
    const DeclType type = DeclType::Decode(u32(e.Type));
    const u32 words = type.format == 37 ? 2u : type.format == 57 ? 3u : type.format == 38 ? 4u : 0u;  // 32-bit floats
    const u32 stream = u32(e.Stream), offset = u32(e.Offset);
    if (!words || stream >= d3d::kMaxStreams)
      return false;
    const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
    if (!src.data || src.endian != 2 || src.stride < offset + 4 * words || index >= src.vertex_count)
      return false;
    float f[4] = {};
    for (u32 k = 0; k < words; ++k) {
      u32 word;
      std::memcpy(&word, src.data + size_t(index) * src.stride + offset + 4 * k, 4);
      word = __builtin_bswap32(word);
      std::memcpy(&f[k], &word, 4);
    }
    for (u32 i = 0; i < 4; ++i) {
      const u8 sw = type.swizzle[i];
      if (sw < words)
        out[i] = f[sw];
      else if (sw == 4)
        out[i] = 0.0f;
      else if (sw == 5)
        out[i] = 1.0f;
      else
        return false;
    }
    return true;
  }
  return false;
}

// Whether the draw writes every pixel of its one colour target whatever the target held: 0 yes; else the test it
// failed (1 targets or shaders, 2 blending or the write mask, 3 depth, stencil, alpha test or cull, 4 a kill in the
// pixel shader, 5 not a 4-vertex quad, fan or strip, 6 the vertex shader is not a listed pass-through one, 7 viewport
// or scissor, 8 the vertices are not a rectangle over the whole target).
// fnxf_xiii (2026-11-17): FF XIII's passes are 4-vertex triangle fans and strips, some in screen space (the viewport
// transform off: positions in pixels, ViewportState::screen_scale / screen_offset).
ViewportState g_colour_last_vp;  // (diagnostic) the viewport of the last overwrite test that got that far

int ColourDrawOverwritesLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord *vs,
                               const ShaderRecord *ps, const Targets &t,
                               const VertexDeclarationRecord *declaration, const u32 *shader_strides,
                               const FloatConstants &vs_constants) {
  if (t.color_count != 1 || !t.colors[0] || !vs || !ps || !ps->entry || !declaration || t.stand_in ||
      g_tiles.size() >= 2)
    return 1;
  const HostSurface *c = t.colors[0];
  const d3d::Device &dev = device.dev();
  const d3d::ControlPacket &cp = dev.m_ControlPacket;
  if ((u32(cp.BlendControl0) & 0x1FFF1FFFu) != 0x00010001u || (u32(dev.m_ValuesPacket.ColorMask) & 0xFu) != 0xFu)
    return 2;
  const u32 color_control = cp.ColorControl;
  if ((t.depth && (u32(cp.DepthControl) & 3u)) || ((color_control & 8u) && (color_control & 7u) != 7u) ||
      ((color_control >> 4) & 1u) || (u32(cp.ModeControl) & 3u))
    return 3;
  if (ps->entry->shaderFlags & (kShaderFlagPixelKill | kShaderFlagPixelDepth))
    return 4;
  const bool quad = call.primitive == kQuadList || call.primitive == kTriangleFan;  // triangles 0-1-2 and 0-2-3
  const bool strip = call.primitive == kTriangleStrip;                             // triangles 0-1-2 and 1-2-3
  if (!(quad || strip) || call.count != 4 || call.indexed || call.tessellation)
    return 5;
  const int matrix = ColourPassThroughVs(vs->hash);
  if (matrix == -1)
    return 6;
  float m[4][4] = {};
  if (matrix >= 0)
    for (u32 k = 0; k < 4; ++k)
      VsConstantLocked(vs_constants, u32(matrix) + k, m[k]);
  const ViewportState vp = ComputeViewport(device, t);
  g_colour_last_vp = vp;
  const float w = float(c->scale.Size(c->width)), h = float(c->scale.Size(c->height));
  // What is drawn is within the viewport and the scissor: both over every pixel centre (the left and top edges count).
  if (vp.viewport.x > 0.5f || vp.viewport.y > 0.5f || !(vp.viewport.x + vp.viewport.width > w - 0.5f) ||
      !(vp.viewport.y + vp.viewport.height > h - 0.5f) || vp.scissor.left > 0 || vp.scissor.top > 0 ||
      float(vp.scissor.right) < w || float(vp.scissor.bottom) < h)
    return 7;
  // The four positions (the shader passes them through): the corners of a rectangle, each corner once, the two
  // triangles sharing its diagonal; w 1, z within the clip volume. In screen space as host pixels.
  const bool screen = vp.screen_space;
  if (screen && !(vp.guest_size[0] > 0.0f && vp.guest_size[1] > 0.0f))
    return 7;
  float px[4], py[4];
  for (u32 v = 0; v < 4; ++v) {
    float p[4];
    if (!DrawPositionLocked(device, call, *vs, *declaration, shader_strides, call.start_vertex + v, p))
      return 8;
    if (matrix >= 0) {
      float q[4];
      for (u32 i = 0; i < 4; ++i)
        q[i] = p[0] * m[0][i] + p[1] * m[1][i] + p[2] * m[2][i] + p[3] * m[3][i];
      std::memcpy(p, q, sizeof(q));
    }
    if (!(p[2] >= 0.0f && p[2] <= 1.0f) || p[3] != 1.0f || !std::isfinite(p[0]) || !std::isfinite(p[1]))
      return 8;
    if (screen) {
      px[v] = vp.viewport.x + (p[0] * vp.screen_scale[0] + vp.screen_offset[0]) * vp.viewport.width / vp.guest_size[0];
      py[v] = vp.viewport.y + (p[1] * vp.screen_scale[1] + vp.screen_offset[1]) * vp.viewport.height / vp.guest_size[1];
    } else {
      px[v] = p[0];
      py[v] = p[1];
    }
  }
  const float x_lo = std::min({px[0], px[1], px[2], px[3]}), x_hi = std::max({px[0], px[1], px[2], px[3]});
  const float y_lo = std::min({py[0], py[1], py[2], py[3]}), y_hi = std::max({py[0], py[1], py[2], py[3]});
  if (!(x_hi > x_lo) || !(y_hi > y_lo))
    return 8;
  u32 id[4], corners = 0;
  for (u32 v = 0; v < 4; ++v) {
    if ((px[v] != x_lo && px[v] != x_hi) || (py[v] != y_lo && py[v] != y_hi))
      return 8;
    id[v] = (px[v] == x_hi ? 1u : 0u) | (py[v] == y_hi ? 2u : 0u);
    corners |= 1u << id[v];
  }
  if (corners != 0xFu || (quad ? (id[0] ^ id[2]) : (id[1] ^ id[2])) != 3u)
    return 8;
  const float kIn = 0.5f + 1.0f / 512.0f, kOut = 0.5f - 1.0f / 128.0f;
  if (screen) {
    if (!(x_lo <= kIn) || !(x_hi >= w - kOut) || !(y_lo <= kIn) || !(y_hi >= h - kOut))
      return 8;
    return 0;
  }
  const float left = vp.viewport.x + (x_lo + 1.0f) * 0.5f * vp.viewport.width;
  const float right = vp.viewport.x + (x_hi + 1.0f) * 0.5f * vp.viewport.width;
  if (!(left <= kIn) || !(right >= w - kOut))
    return 8;
  for (int flip = 0; flip < 2; ++flip) {
    const float a = flip ? 1.0f - y_hi : y_lo + 1.0f, b = flip ? 1.0f - y_lo : y_hi + 1.0f;
    const float top = vp.viewport.y + a * 0.5f * vp.viewport.height;
    const float bottom = vp.viewport.y + b * 0.5f * vp.viewport.height;
    if (!(top <= kIn) || !(bottom >= h - kOut))
      return 8;
  }
  return 0;
}

// Before the draw's textures are bound: its colour targets whose content a destination holds. An overwriting draw
// notes the take (made at its bind: ColourHandOverDrawBindLocked; until then a destination it samples is sampled where
// it is); any other write has the resolve drawn now.
void ColourHandOverDrawStartLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord *vs,
                                   const ShaderRecord *ps, const Targets &t,
                                   const VertexDeclarationRecord *declaration, const u32 *shader_strides,
                                   const FloatConstants &vs_constants) {
  const u32 mask = u32(device.dev().m_ValuesPacket.ColorMask) & 0xFFFFu;
  for (uint32_t i = 0; i < t.color_count; ++i) {
    HostSurface *c = t.colors[i];
    if (!c || !c->handed || c->depth || !((mask >> (4 * i)) & 0xFu))
      continue;  // not written by this draw: its content stays where it is
    int why = 1;
    if (i == 0 && (why = ColourDrawOverwritesLocked(device, call, vs, ps, t, declaration, shader_strides, vs_constants)) == 0) {
      g_colour_take_pending[g_colour_take_pending_count++] = {c, 3};
      continue;
    }
    if (why == 8)
      ++g_colour_ho_stats[7];
    static std::unordered_set<uint64_t> logged;  // each target / shaders / test once (the first 96)
    if (logged.size() < 96 && REXCVAR_GET(log_diagnostics) &&
        logged.insert((uint64_t(c->guest_va) << 8 | uint64_t(why)) ^ (vs ? vs->hash * 3 : 0) ^ (ps ? ps->hash * 7 : 0))
            .second) {
      const ViewportState &v = g_colour_last_vp;
      RGPU_INFO("[colourho] target {:08X} ({}x{} format {}) drawn into by vs {:016X} ps {:016X} (primitive {} count "
                "{}) with a destination holding its content: no overwrite (test {}): the resolve drawn after all{}",
                c->guest_va, c->width, c->height, u32(c->format), vs ? vs->hash : 0, ps ? ps->hash : 0,
                u32(call.primitive), call.count, why,
                why >= 7 ? fmt::format(" (viewport {} {} {} {}, scissor {} {} {} {}, screen space {})", v.viewport.x,
                                       v.viewport.y, v.viewport.width, v.viewport.height, v.scissor.left,
                                       v.scissor.top, v.scissor.right, v.scissor.bottom, v.screen_space)
                         : std::string());
    }
    g_colour_mat_why = 10 + why;
    ColourHandOverBeforeWriteLocked(c, 0);
  }
}

void ColourHandOverDrawBindLocked() {
  for (uint32_t i = 0; i < g_colour_take_pending_count; ++i) {
    g_colour_mat_why = 20 + g_colour_take_pending[i].kind;  // (diagnostic) a take with no destination that can take
    ColourHandOverBeforeWriteLocked(g_colour_take_pending[i].surface, g_colour_take_pending[i].kind);
  }
  g_colour_take_pending_count = 0;
}

bool ColourTakePendingFor(const HostSurface *surface) {
  for (uint32_t i = 0; i < g_colour_take_pending_count; ++i)
    if (g_colour_take_pending[i].surface == surface)
      return true;
  return false;
}

// With log_diagnostics: what became of the colour resolves, every 300 frames.
void ColourHandOverReportLocked() {
  static uint32_t frames = 0;
  if (++frames < 300)
    return;
  frames = 0;
  uint64_t any = 0;
  for (const uint64_t v : g_colour_ho_stats)
    any |= v;
  if (any && REXCVAR_GET(log_diagnostics))
    RGPU_INFO("[colourho] 300 frames: {} colour resolves left in their target's image; the image taken by the "
              "destination at a whole clear {}, at an overwriting draw {}, as its target went {}; drawn after all {}, lost {}; {} new images; "
              "{} draws no overwrite by their vertices alone; {} targets tracked now",
              g_colour_ho_stats[0], g_colour_ho_stats[1], g_colour_ho_stats[3], g_colour_ho_stats[2], g_colour_ho_stats[4],
              g_colour_ho_stats[5], g_colour_ho_stats[6], g_colour_ho_stats[7], g_colour_tracked.size());
  if (!g_colour_mat_tally.empty() && REXCVAR_GET(log_diagnostics)) {
    std::vector<std::pair<uint64_t, uint32_t>> rows(g_colour_mat_tally.begin(), g_colour_mat_tally.end());
    auto pixels = [](const std::pair<uint64_t, uint32_t> &r) {
      return uint64_t(r.second) * ((r.first >> 20) & 0xFFFFF) * (r.first & 0xFFFFF);
    };
    std::sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) { return pixels(a) > pixels(b); });
    std::string text;
    for (size_t i = 0; i < rows.size() && i < 12; ++i)
      text += fmt::format("{}{} x {}x{} format {} why {}", i ? "; " : "", rows[i].second, (rows[i].first >> 20) & 0xFFFFF,
                          rows[i].first & 0xFFFFF, (rows[i].first >> 40) & 0xFFFF, rows[i].first >> 56);
    RGPU_INFO("[colourho] drawn after all, by size (count x size, format, why): {}", text);
  }
  std::memset(g_colour_ho_stats, 0, sizeof(g_colour_ho_stats));
  g_colour_mat_tally.clear();
}

// fnxf_xiii (2026-10-18, fnxf_xiii_deko3d_draw_resolves): a single-sampled colour resolve drawn into the destination
// (shaders/host/resolve_copy_ps.hlsl) rather than copied. deko3d copies images on the copy engine, which plume_deko3d
// fences with a full barrier before and after (the 3D engine idle for the whole copy); a draw is ordered by the usual
// fragment barriers and overlaps the work around it. Same format both sides, 2D level 0, slice 0: every texel is written
// back bit for bit. 'rect' is the source rect and (host_x, host_y) its place in the destination, in host pixels.
bool DrawResolveCopy(HostSurface *surface, HostTexture *dest, const plume::RenderRect &rect, uint32_t host_x,
                     uint32_t host_y, int32_t sample = -1) {
  auto &s = State();
  if (sample >= int32_t(surface->samples))
    sample = 0;
  const bool one_sample = sample >= 0;  // fnxf_xiii: one sample of a multisampled source (resolve_copy_ms_ps)
  plume::RenderShader *shader = one_sample ? s.resolve_copy_ms_ps.get() : s.resolve_copy_ps.get();
  if (!shader || !dest->texture || !dest->render_target || dest->host_format != surface->format ||
      dest->dimension != TextureDimension::Tex2D || dest->stacked)
    return false;
  switch (surface->format) {  // formats a texel load gives back exactly when written as a colour
  case plume::RenderFormat::R10G10B10A2_UNORM:  // fnxf_xiii (2026-10-25, fnxf_xiii_deko3d_native_formats)
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R16_UNORM:
  case plume::RenderFormat::R16_FLOAT:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    break;
  default:
    return false;
  }
  const uint32_t source_slot = SurfaceSampleSlotLocked(surface);
  if (source_slot == kInvalidDescriptor)
    return false;
  auto &pipeline = s.resolve_copy_pipelines[uint32_t(surface->format) | (one_sample ? 0x10000u : 0u)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = shader;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = surface->format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return false;
    pipeline->setName("renut resolve copy");
  }
  if (!dest->copy_framebuffer) {
    const plume::RenderTexture *targets[] = {dest->texture.get()};
    dest->copy_framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
    if (!dest->copy_framebuffer)
      return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  TransitionSurfaceLocked(surface, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(dest->copy_framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->alloc_width), float(dest->alloc_height)));
  cmd->setScissors(plume::RenderRect(int32_t(host_x), int32_t(host_y), int32_t(host_x) + (rect.right - rect.left),
                                     int32_t(host_y) + (rect.bottom - rect.top)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  // fnxf_xiii (2026-10-22): the sample count for ms_sample_deko3d.hlsli (deko3d reads the samples itself).
  push.scale[0] = one_sample ? float(u32(surface->samples)) : 1.0f;
  push.scale[1] = one_sample ? float(sample + 1) : 1.0f;
  push.translate[0] = float(rect.left - int32_t(host_x));
  push.translate[1] = float(rect.top - int32_t(host_y));
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  if (!one_sample)
    fnxf_xiii::renut_gpu::CountDrawnResolve();
  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

// fnxf_xiii (2026-10-31): the EDRAM colour targets share. On the console a render target is a view of EDRAM, which keeps
// what was last written there; the game draws into some targets without clearing them first and leaves the pixels its
// geometry misses as the EDRAM held them. FINAL FANTASY XIII's scene (the Vestige, behind the crystal by Hope): no
// geometry there, the scene's colour never cleared, so the console showed what was last written at that EDRAM, the
// previous frame's picture (a soft light); here each EDRAM view is an image of its own, which held undefined memory
// (bright noise the bloom and light shafts spread) and, cleared on creation (HostSurface::needs_clear), black.
// So: a colour target first drawn in a frame without a clear in it, where a different target of the same EDRAM base
// and format was written after it in an earlier frame, starts with that target's picture, scaled to its size (the
// port's render resolution makes the console's exact EDRAM layout moot). Once a target and frame at most, across
// frames only (within a frame the game's passes resolve what they need).
// fnxf_xiii (2026-11-01): only from a target at least as large: a smaller one does not cover this one's EDRAM, so its
// picture scaled up is not what the console held there. The fog target (6D907A14: the fog volumes add their density to
// what it holds, draws 120-123 of a frame) then keeps its own last picture, as on the console; carried from the bloom's
// smaller target (seen in the log at 720p) it started from something else. In a replay of the game's post-processing
// from a frame dump, the fog target keeping its own picture is the closest to the PC release's light behind the crystal.
std::unordered_map<u32, uint64_t> g_edram_last;  // EDRAM base -> the colour target written there last (its generation)
uint64_t g_edram_write_order = 0;

void EdramNoteClearLocked(HostSurface *c) {
  if (!c || c->depth || c->edram_base == 0xFFFFFFFFu)
    return;
  c->cleared_frame = TextureFrameLocked();
  c->write_order = ++g_edram_write_order;
  g_edram_last[c->edram_base] = c->generation;
}

// fnxf_xiii (2026-11-07): a colour target written in full this frame outside a draw (FSR 3's output copied into it): its
// whole picture is defined, so it is drawn this frame (no EDRAM carry into it at its next bind, which would draw another
// target's older picture over it: the scene black in some frames at 1024x576 with FSR 3.1, the HUD drawn after it fine)
// and needs no clear on first bind.
void EdramNoteDrawnLocked(HostSurface *c) {
  if (!c || c->depth)
    return;
  c->needs_clear = false;
  if (c->edram_base == 0xFFFFFFFFu)
    return;
  c->drawn_frame = TextureFrameLocked();
  c->write_order = ++g_edram_write_order;
  g_edram_last[c->edram_base] = c->generation;
}

void EdramCarryLocked(const Targets &t) {
  auto &s = State();
  const uint64_t frame = TextureFrameLocked();
  for (uint32_t i = 0; i < t.color_count; ++i) {
    HostSurface *c = t.colors[i];
    if (!c || c->depth || c->edram_base == 0xFFFFFFFFu || c->drawn_frame == frame)
      continue;
    uint64_t &last = g_edram_last[c->edram_base];
    // By generation: a target retired or rebuilt since is not found (SurfaceByGenerationLocked).
    HostSurface *from = last && last != c->generation ? SurfaceByGenerationLocked(last) : nullptr;
    const bool carry = c->cleared_frame != frame && from && from != c && from->texture && c->texture &&
                       from->format == c->format && from->drawn_frame != UINT64_MAX && from->drawn_frame < frame &&
                       from->write_order > c->write_order && t.color_count == 1 && s.present_vs && s.present_ps &&
                       from->width >= c->width && from->height >= c->height;  // fnxf_xiii (2026-11-01): see below
    c->drawn_frame = frame;
    c->write_order = ++g_edram_write_order;
    last = c->generation;
    if (!carry)
      continue;
    // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): the carry writes it: a destination holding its content gets its resolve
    // first (which binds another framebuffer: the draw's targets again after it).
    if (c->handed) {
      g_colour_mat_why = 6;
      ColourHandOverBeforeWriteLocked(c, 0);
      g_colour_ho_draw_checked = true;
      BindTargets(t);
      g_colour_ho_draw_checked = false;
    }
    const uint32_t samples = u32(c->samples);
    static std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
    auto &pipeline = pipelines[uint64_t(c->format) | uint64_t(samples) << 32];
    if (!pipeline) {
      plume::RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = s.utility_layout.get();
      desc.vertexShader = s.present_vs.get();
      desc.pixelShader = s.present_ps.get();
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      desc.cullMode = plume::RenderCullMode::NONE;
      desc.renderTargetCount = 1;
      desc.renderTargetFormat[0] = c->format;
      desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
      desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
      desc.multisampling = plume::RenderMultisampling(c->samples);
      pipeline = s.device->createGraphicsPipeline(desc);
      if (!pipeline)
        continue;
      pipeline->setName("renut edram carry");
    }
    const uint32_t source_slot = SurfaceSampleSlotLocked(from);
    if (source_slot == kInvalidDescriptor)
      continue;
    static uint32_t logged = 0;
    if (logged++ < 8)
      if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[edram] target {:08X} ({}x{} samples {}) starts from {:08X}'s picture ({}x{}), as the console's EDRAM "
                "base {:03X} holds it", c->guest_va, c->width, c->height, samples, from->guest_va, from->width,
                from->height, c->edram_base);
    plume::RenderCommandList *cmd = s.command_list;
    TransitionSurfaceLocked(from, plume::RenderTextureLayout::SHADER_READ);
    // fnxf_xiii (2026-11-07, dynamic resolution): the part of each that is drawn (the whole without it).
    const float w = float(c->scale.DrawnSize(c->width)), h = float(c->scale.DrawnSize(c->height));
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, w, h));
    cmd->setScissors(plume::RenderRect(0, 0, int32_t(w), int32_t(h)));
    cmd->setGraphicsPipelineLayout(s.utility_layout.get());
    cmd->setPipeline(pipeline.get());
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
    UtilityPush push{};
    push.scale[0] = from->scale.drs ? g_drs_factor : 1.0f;
    push.scale[1] = from->scale.drs ? g_drs_factor : 1.0f;
    push.texture_index = source_slot;
    push.sampler_index = kLinearClampSamplerSlot;
    push.param0 = 1.0f;  // present_ps: no gamma
    push.param1 = 0.0f;  // no gamma ramp
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawInstanced(3, 1, 0, 0);
    ++c->writes;  // fnxf_xiii_renut_resolve_skip
    // The draw binds its own layout, pipeline and viewport again (its targets stay bound).
    g_bound.pipeline = nullptr;
    g_bound.layout = false;
    g_bound.viewport_set = false;
  }
}

plume::RenderRect ClampRect(const ClearRect *rect, uint32_t width, uint32_t height) {
  if (!rect)
    return plume::RenderRect(0, 0, int32_t(width), int32_t(height));
  const int32_t x1 = std::clamp(rect->x1, 0, int32_t(width));
  const int32_t y1 = std::clamp(rect->y1, 0, int32_t(height));
  const int32_t x2 = std::clamp(rect->x2, x1, int32_t(width));
  const int32_t y2 = std::clamp(rect->y2, y1, int32_t(height));
  return plume::RenderRect(x1, y1, x2, y2);
}

} // namespace

void ClearStateLocked(const u8 *device_state, u32 flags, const ClearRect *rect,
                      const float color[4], float depth, u32 stencil) {
  const rtw::Scope rtw_clear(rtw::kClear);  // fnxf_xiii (2026-11-05, [rtwork])
  auto &s = State();
  if (!Video::EnsureCommandListLocked())
    return;
  SyncBound(s);
  const GuestDevice device(device_state);
  if (!device.valid())
    return;
  Targets targets;
  if (!GatherTargets(device, targets))
    return;

  // D3D clamps clears to the viewport (CDevice viewport floats at 0x3168).
  ClearRect r = rect ? *rect : ClearRect{0, 0, int32_t(targets.width), int32_t(targets.height)};
  const float vx = device.F32(0x3218), vy = device.F32(0x321C);  // fnxf_xiii (N&B 0x3168)
  const float vw = device.F32(0x3220), vh = device.F32(0x3224);  // fnxf_xiii (N&B 0x3170)
  if (vw > 0.0f && vh > 0.0f) {
    int32_t vx2 = int32_t(vx + vw), vy2 = int32_t(vy + vh);
    // A target grown for tiling: D3D bounds the viewport by the tile-sized
    // surface, which covers the whole target across the tiles (as for the
    // scissor in ComputeViewport).
    if (targets.width > targets.guest_width && vx2 >= int32_t(targets.guest_width))
      vx2 = int32_t(targets.width);
    if (targets.height > targets.guest_height && vy2 >= int32_t(targets.guest_height))
      vy2 = int32_t(targets.height);
    r.x1 = std::max(r.x1, int32_t(vx));
    r.y1 = std::max(r.y1, int32_t(vy));
    r.x2 = std::min(r.x2, vx2);
    r.y2 = std::min(r.y2, vy2);
  }
  const plume::RenderRect clear_rect = ClampRect(&r, targets.width, targets.height);
  if (clear_rect.right <= clear_rect.left || clear_rect.bottom <= clear_rect.top)
    return;
  ClearTargets(targets, flags, clear_rect, color, depth, stencil);
}

// fnxf_xiii (2026-10-19, fnxf_xiii_renut_depth_words_lazy): a destination's first resolves write the words either way.
constexpr uint32_t kDepthWordsProbe = 300;

void EnsureDepthWordsLocked(HostTexture &dest) {
  if (!fnxf_xiii::renut_gpu::DepthWordsLazyOn())
    return;
  dest.words_wanted = true;
  if (dest.words_current)
    return;
  // fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover): left in a target's image: drawn now, with the words.
  if (dest.handed_from || dest.held_current) {
    fnxf_xiii::renut_gpu::CountLazyWords(MaterialiseLocked(dest, true) ? 1 : 2);
    return;
  }
  HostSurface *surface = SurfaceByGenerationLocked(dest.last_source_generation);
  if (surface && surface->writes == dest.last_source_writes) {
    const ClearRect src{dest.last_src[0], dest.last_src[1], dest.last_src[2], dest.last_src[3]};
    if (ResolveDepth(surface, &dest, src, dest.last_dest_x, dest.last_dest_y, true, dest.last_sample)) {
      dest.words_current = true;
      fnxf_xiii::renut_gpu::CountLazyWords(1);
      return;
    }
  }
  // The source has changed since: these words are older ones. From now on this destination writes them each time.
  fnxf_xiii::renut_gpu::CountLazyWords(2);
}

// fnxf_xiii (2026-10-19, fnxf_xiii_deko3d_depth_handover): see "Depth hand-over".
uint32_t DepthHandOverSlotLocked(HostTexture &d, const plume::RenderComponentMapping &mapping, BindEffects *fx) {
  if (HostSurface *source = d.handed_from) {
    if (!source->texture || source->generation != d.handed_generation || source->writes != d.handed_writes) {
      UnhandLocked(d);
      fnxf_xiii::renut_gpu::CountHandOver(5);
      return UINT32_MAX;
    }
    // Not the image of a target of the draw: its resolve is drawn after all.
    if (g_handover_targets && g_handover_targets->depth == source) {
      MaterialiseLocked(d, false);
      return UINT32_MAX;
    }
    // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): a colour target of the draw likewise, unless the draw is noted to give
    // this image to the destination before it binds its targets (ColourHandOverDrawStartLocked): then the image is
    // sampled where it is (it stays the destination's; the draw writes the target's other one).
    if (g_handover_targets && !source->depth && !ColourTakePendingFor(source)) {
      bool target = false;
      for (uint32_t i = 0; i < g_handover_targets->color_count; ++i)
        target = target || g_handover_targets->colors[i] == source;
      if (target) {
        g_colour_mat_why = 4;
        MaterialiseLocked(d, false);
        return UINT32_MAX;
      }
    }
    const uint32_t slot =
        DepthImageViewSlotLocked(source->texture.get(), source->format, source->handover_views, mapping);
    if (slot == kInvalidDescriptor) {
      g_colour_mat_why = source->depth ? 0 : 5;
      MaterialiseLocked(d, false);
      return UINT32_MAX;
    }
    TransitionSurfaceLocked(source, plume::RenderTextureLayout::SHADER_READ);
    if (fx)
      fx->handover_surface = source;
    fnxf_xiii::renut_gpu::CountHandOver(1);
    return slot;
  }
  if (!d.held || !d.held_current)
    return UINT32_MAX;
  DepthImage &image = *d.held;
  const uint32_t slot = DepthImageViewSlotLocked(image.texture.get(), image.format, image.views, mapping);
  if (slot == kInvalidDescriptor) {
    MaterialiseLocked(d, false);
    return UINT32_MAX;
  }
  TransitionDepthImageLocked(image, plume::RenderTextureLayout::SHADER_READ);
  if (fx)
    fx->handover_image = &image;
  fnxf_xiii::renut_gpu::CountHandOver(1);
  return slot;
}

// fnxf_xiii (2026-11-08): the destination's own texture holds its content (drawn now if a target's image held it).
int DepthHandOverMaterialiseLocked(HostTexture &d) {
  if (!d.handed_from && !d.held_current)
    return 0;
  const bool held = !d.handed_from;
  if (!g_colour_mat_why)
    g_colour_mat_why = 30;  // fnxf_xiii (2026-11-17, diagnostic): the destination's own texture wanted (composite, present)
  return MaterialiseLocked(d, false) ? (held ? 2 : 1) : (held ? 4 : 3);
}

void DepthHandOverForgetLocked(HostTexture &d) {
  UnhandLocked(d);
  d.held_current = false;
  if (d.held) {
    RetireDepthImageLocked(*d.held);
    d.held.reset();
  }
}

void DepthHandOverBeforeRetireLocked(HostSurface *surface) {
  if (!surface || !surface->handed)
    return;
  const bool list = Video::EnsureCommandListLocked();
  std::vector<HostTexture *> holders;
  for (HostTexture *d : g_handed)
    if (d->handed_from == surface)
      holders.push_back(d);
  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): a colour target that goes gives its image to the last destination that
  // holds its current content (no copy: the image would go with it); any other is drawn.
  HostTexture *taker = nullptr;
  if (!surface->depth)
    for (size_t i = holders.size(); i-- > 0;)
      if (surface->texture && surface->generation == holders[i]->handed_generation &&
          surface->writes == holders[i]->handed_writes) {
        taker = holders[i];
        break;
      }
  for (HostTexture *d : holders) {
    if (d == taker) {
      if (ColourTakeAtRetireLocked(*d, *surface))
        continue;
    }
    if (list) {
      g_colour_mat_why = 31;  // fnxf_xiii (2026-11-17, diagnostic): the target goes
      MaterialiseLocked(*d, false);
    } else {
      UnhandLocked(*d);
      fnxf_xiii::renut_gpu::CountHandOver(5);
    }
  }
}

// fnxf_xiii (2026-10-19, fnxf_xiii_renut_resolve_skip): what a resolve wrote into its destination: the source surface (its
// identity, rebuilds and writes), the rectangle and the place. Never 0.
static uint64_t ResolveSignature(const HostSurface *surface, const ClearRect &src, i32 dest_x, i32 dest_y,
                                 u32 level, u32 slice) {
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0x100000001b3ull;
  };
  mix(uint64_t(uintptr_t(surface)));
  mix(surface->generation);
  mix(surface->writes);
  mix(surface->format_word);
  mix(uint64_t(uint32_t(src.x1)) | uint64_t(uint32_t(src.y1)) << 32);
  mix(uint64_t(uint32_t(src.x2)) | uint64_t(uint32_t(src.y2)) << 32);
  mix(uint64_t(uint32_t(dest_x)) | uint64_t(uint32_t(dest_y)) << 32);
  mix(uint64_t(level) | uint64_t(slice) << 32);
  return h ? h : 1;
}

void ResolveStateLocked(const u8 *device_state, u32 flags, const ClearRect *source_rect,
                        u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                        i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                        const float clear_color[4], float clear_depth, u32 clear_stencil) {
  const rtw::Scope rtw_resolve(rtw::kResolve);  // fnxf_xiii (2026-11-05, [rtwork])
  auto &s = State();
  // Pending memexport results stay on the GPU: a resolve writes host textures
  // only, and a clear reads no memory.
  if (!Video::EnsureCommandListLocked())
    return;
  SyncBound(s);
  const GuestDevice device(device_state);
  if (!device.valid())
    return;

  const u32 source = flags & 7;
  // fnxf_xiii: Resolve flags bits 4-6 (RB_COPY_CONTROL's sample select as D3D sets it): 1-4 one sample (0-3), 5 samples
  // 0+1, 6 samples 2+3, 7 all (the game's Resolve makes 0 the surface's: 1 for 1x, 5 for 2x, 7 for 4x). A colour resolve
  // of one sample copies that sample; others average. Depth always takes one sample (the first of those selected).
  const u32 fragments = (flags >> 4) & 7;
  // fnxf_xiii (2026-10-22, fnxf_xiii_diag_ms_average): the hardware resolve (samples averaged), no sample fetch in a shader.
  // fnxf_xiii (2026-10-22, fnxf_xiii_diag_ms_sample_mode): 1 every one-sample resolve reads sample 0, 2 samples 0 and 1
  // swapped (a test of the per-sample addressing of the 2x targets).
  const int32_t sample_mode = REXCVAR_GET(fnxf_xiii_diag_ms_sample_mode);
  auto remap = [sample_mode](uint32_t s) -> uint32_t { return sample_mode == 1 ? 0u : sample_mode == 2 && s < 2 ? s ^ 1u : s; };
  const int32_t colour_sample =
      fragments >= 1 && fragments <= 4 && !REXCVAR_GET(fnxf_xiii_diag_ms_average) ? int32_t(remap(fragments - 1)) : -1;
  const uint32_t depth_sample = remap(fragments >= 1 && fragments <= 4 ? fragments - 1 : fragments == 6 ? 2u : 0u);
  const u32 source_va = source == 4 ? device.U32(d3d::kDeviceOffsetDepthStencil)
                                    : device.U32(d3d::kDeviceOffsetRenderTargets + source * 4);
  HostSurface *surface = GetHostSurfaceLocked(source_va);
  // gpu_profile: by the surface resolved (fnxf_xiii 2026-10-25; was one "resolves" for all).
  static const uint32_t kProfileLabel = gpu_profile::Label("resolves");
  gpu_profile::Mark(s, surface ? ResolveProfileLabel(surface, (dest_texture_va ? 1u : 0u) | ((flags & 0x300) ? 2u : 0u))
                               : kProfileLabel);
  static const bool final_reads = REXCVAR_GET(fnxf_xiii_diag_final_reads);
  if (final_reads && surface && !surface->depth && surface->width == 1280 && surface->height == 720) {
    struct Entry {
      uint32_t count = 0;
      uint64_t first_frame = 0;
    };
    static std::map<std::string, Entry> table;
    static auto last = std::chrono::steady_clock::now();
    const ClearRect r = source_rect ? *source_rect : ClearRect{0, 0, -1, -1};
    uint32_t dw = 0, dh = 0, dd = 0;
    uint32_t dfmt = 0;
    if (dest_format) {
      const TextureFetch tf = TextureFetch::FromHeader(*dest_format);
      if (tf.IsTexture())
        tf.Size(dw, dh, dd);
      dfmt = u32(tf.Format());
    }
    const std::string key = fmt::format("{:08X} fmt {} -> {:08X} {}x{} fmt {} level {} rect ({},{})-({},{}) at ({},{}) "
                                         "flags {:X}",
                                         source_va, u32(surface->format), dest_texture_va, dw, dh, dfmt, dest_level,
                                         r.x1, r.y1, r.x2, r.y2, dest_x, dest_y, flags);
    Entry &e = table[key];
    if (!e.count++)
      e.first_frame = TextureFrameForLog();
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(10)) {
      last = now;
      RGPU_INFO("[final reads] {} kinds in 10 s (frame {})", table.size(), TextureFrameForLog());
      for (const auto &[k, v] : table)
        RGPU_INFO("[final reads]   {} x{} (first at frame {})", k, v.count, v.first_frame);
      table.clear();
    }
  }
  if (fnxf_xiii::renut_gpu::ResolveTraceOn() && surface) {
    const ClearRect r = source_rect ? *source_rect : ClearRect{0, 0, -1, -1};
    RGPU_INFO("[trace] resolve {} {:08X} ({}x{} fmt {} samples {}) -> texture {:08X} at ({},{}) level {} slice {}, "
              "rect ({},{})-({},{}), flags {:X} ({}{})",
              surface->depth ? "depth" : "colour", source_va, surface->width, surface->height, u32(surface->format),
              u32(surface->samples), dest_texture_va, dest_x, dest_y, dest_level, dest_slice, r.x1, r.y1, r.x2, r.y2,
              flags, (flags & 0x100) ? "clear colour after " : "", (flags & 0x200) ? "clear depth after" : "");
  }
  // fnxf_xiii (diagnostic): every resolve destination once (its memory), to find what writes a texture's memory.
  if (dest_format && surface && REXCVAR_GET(fnxf_xiii_diag_log)) {
    static std::unordered_set<uint64_t> seen;
    const TextureFetch f = TextureFetch::FromHeader(*dest_format);
    u32 w = 0, h = 0, d = 0;
    if (f.IsTexture())
      f.Size(w, h, d);
    const uint64_t key = uint64_t(dest_texture_va) << 32 | f.BasePage() | uint64_t(dest_level) << 24;
    if (seen.insert(key).second && seen.size() < 4000)
      RGPU_INFO("[resolvedest] frame {}: {} {:08X} ({}x{} fmt {} samples {}) -> texture {:08X} base {:05X} fmt {} "
                "{}x{}x{} level {} at ({},{}) flags {:X}",
                TextureFrameForLog(), surface->depth ? "depth" : "colour", source_va, surface->width,
                surface->height, u32(surface->format), u32(surface->samples), dest_texture_va, f.BasePage(),
                u32(f.Format()), w, h, d, dest_level, dest_x, dest_y, flags);
  }
  plume::RenderCommandList *cmd = s.command_list;
  static uint32_t depth_logged = 0;

  // The source rect within the target and the destination level.
  auto fit = [&](const HostTexture *dest, ClearRect &src) {
    const uint32_t level_w = std::max(1u, dest->width >> dest_level);
    const uint32_t level_h = std::max(1u, dest->height >> dest_level);
    src = source_rect ? *source_rect : ClearRect{0, 0, int32_t(level_w), int32_t(level_h)};
    src.x1 = std::clamp(src.x1, 0, int32_t(surface->width));
    src.y1 = std::clamp(src.y1, 0, int32_t(surface->height));
    src.x2 = std::clamp(src.x2, src.x1, int32_t(surface->width));
    src.y2 = std::clamp(src.y2, src.y1, int32_t(surface->height));
    dest_x = std::clamp(dest_x, 0, int32_t(level_w));
    dest_y = std::clamp(dest_y, 0, int32_t(level_h));
    src.x2 = std::min(src.x2, src.x1 + int32_t(level_w) - dest_x);
    src.y2 = std::min(src.y2, src.y1 + int32_t(level_h) - dest_y);
    return src.x2 > src.x1 && src.y2 > src.y1;
  };

  if (surface && dest_texture_va && dest_format && dest_level == 0)  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_resolve_reads)
    ResolveReadWatchLocked(dest_texture_va, TextureFetch::FromHeader(*dest_format));
  if (surface && dest_texture_va) {
    if (surface->depth) {
      HostTexture *dest =
          dest_level == 0 && dest_slice == 0
              ? GetDepthResolveTextureLocked(dest_texture_va, surface->scale, dest_format)
              : nullptr;
      ClearRect src;
      if (!dest) {
        if (LogLimited(depth_logged, 2)) {
          RGPU_WARN("Depth resolve into {:08X} (level {}, slice {}) not done", dest_texture_va,
                    dest_level, dest_slice);
        }
      } else if (fit(dest, src)) {
        // fnxf_xiii (2026-10-19, fnxf_xiii_renut_resolve_skip): the same resolve of an unchanged source is in dest already.
        const uint64_t sig = ResolveSignature(surface, src, dest_x, dest_y, dest_level, dest_slice);
        // fnxf_xiii (2026-10-19, fnxf_xiii_renut_depth_words_lazy): the words only where something reads them (or while
        // probing a new destination).
        const bool lazy = fnxf_xiii::renut_gpu::DepthWordsLazyOn();
        const bool words = !lazy || dest->words_wanted || dest->words_probe < kDepthWordsProbe;
        // fnxf_xiii (2026-11-08, shadows): a frame dump lists each depth resolve and dumps its destination after it.
        if (g_dump.active && g_dump.file) {
          std::fprintf(g_dump.file,
                       "resolve depth %08X (%ux%u, host %llX img %llX gen %llu writes %llu) -> %08X rect %d,%d-%d,%d at %d,%d: %s\n",
                       source_va, surface->width, surface->height, (unsigned long long)uintptr_t(surface),
                       (unsigned long long)uintptr_t(surface->texture.get()), (unsigned long long)surface->generation,
                       (unsigned long long)surface->writes, dest_texture_va, src.x1, src.y1, src.x2, src.y2, dest_x,
                       dest_y,
                       fnxf_xiii::renut_gpu::ResolveSkipOn() && dest->resolve_signature == sig ? "skipped (same signature)"
                                                                                            : "done");
          if (dest_format)
            g_dump.pending.push_back({dest_texture_va, TextureFetch::FromHeader(*dest_format), 9000 + int(g_dump.draw)});
        }
        if (fnxf_xiii::renut_gpu::ResolveSkipOn() && dest->resolve_signature == sig) {
          fnxf_xiii::renut_gpu::CountSkippedResolve(true);
        } else if (HandOverResolveLocked(surface, dest, src, dest_x, dest_y, words) ||
                   ResolveDepth(surface, dest, src, dest_x, dest_y, words, depth_sample)) {
          dest->resolve_signature = sig;
          dest->words_current = words;
          // fnxf_xiii (2026-11-07, dynamic resolution): the level it is drawn at (its bindings' code).
          if (const uint32_t code = surface->scale.drs ? DrsCode() : 0u; code != dest->drs_code) {
            dest->drs_code = code;
            if (dest->words)
              dest->words->drs_code = code;
            ++g_texture_epoch;
          }
          if (!dest->words_wanted && dest->words_probe < kDepthWordsProbe)
            ++dest->words_probe;
          if (!words)
            fnxf_xiii::renut_gpu::CountLazyWords(0);
          dest->last_source_generation = surface->generation;
          dest->last_source_writes = surface->writes;
          dest->last_src[0] = src.x1;
          dest->last_src[1] = src.y1;
          dest->last_src[2] = src.x2;
          dest->last_src[3] = src.y2;
          dest->last_dest_x = dest_x;
          dest->last_dest_y = dest_y;
          dest->last_sample = depth_sample;
        } else {
          dest->resolve_signature = 0;
        }
      }
    } else if (HostTexture *dest = GetResolveTextureLocked(dest_texture_va, surface->format,
                                                           surface->scale, dest_format)) {
      ClearRect src;
      // A volume's slices are depth; stacked textures and cubes have layers.
      const bool volume = dest->dimension == TextureDimension::Tex3D && !dest->stacked;
      const uint32_t slice = volume ? 0 : dest_slice;
      const uint32_t z = volume ? dest_slice : 0;
      const bool fits = fit(dest, src);
      const uint64_t sig = fits ? ResolveSignature(surface, src, dest_x, dest_y, dest_level, dest_slice) : 0;
      // fnxf_xiii (2026-11-08, FSR 3.1 before the post-processing): a resolve of the scene's colour target: made again from the
      // upscaled scene if it is the scene's last (EarlyFsrBeforePostLocked).
      if (fits && dest_level == 0 && slice == 0 && z == 0 && dest_format && EarlyFsrOn() &&
          surface == g_motion_target.scene && g_motion_target.cleared_frame == g_motion_target.frame) {
        if (g_early_fsr.frame != g_motion_target.frame) {
          g_early_fsr.frame = g_motion_target.frame;
          g_early_fsr.resolves.clear();
        }
        g_early_fsr.resolves.push_back({dest_texture_va, *dest_format, src, dest_x, dest_y});
        ++g_early_noted;
      }
      if (fits && fnxf_xiii::renut_gpu::ResolveSkipOn() && dest->resolve_signature == sig) {
        // fnxf_xiii (2026-10-19, fnxf_xiii_renut_resolve_skip): the same resolve of an unchanged source is in dest already.
        fnxf_xiii::renut_gpu::CountSkippedResolve(false);
      } else if (fits) {
        dest->resolve_signature = sig;
        // fnxf_xiii (2026-11-07, dynamic resolution): the level it is drawn at (its bindings' code).
        if (const uint32_t code = surface->scale.drs ? DrsCode() : 0u; code != dest->drs_code) {
          dest->drs_code = code;
          ++g_texture_epoch;  // the slot memos' descriptor index has it
        }
        g_bound.framebuffer = nullptr;
        if (g_load_log_active)  // fnxf_xiii (2026-11-08, fnxf_xiii_diag_load_draw_log)
          g_load_lines.push_back(fmt::format(
              "  resolve {:08X} {}x{} c{}{} rect {},{}-{},{} -> tex {:08X} slot {:X} {}x{} c{} at {},{} drs_code {}",
              surface->guest_va, surface->scale.Size(surface->width), surface->scale.Size(surface->height),
              surface->scale.code, surface->scale.drs ? "d" : "", src.x1, src.y1, src.x2, src.y2, dest_texture_va,
              dest->slot, dest->alloc_width, dest->alloc_height, dest->scale.code, dest_x, dest_y, dest->drs_code));
        // The destination is of the surface's scale.
        plume::RenderRect rect =
            ScaleRect(plume::RenderRect(src.x1, src.y1, src.x2, src.y2), surface->scale);
        const uint32_t host_x = uint32_t(surface->scale.Coord(dest_x));
        const uint32_t host_y = uint32_t(surface->scale.Coord(dest_y));
        // At a fractional scale the source rect, rounded at its own edges, can
        // pass the destination's host size by a texel.
        rect.right = std::min(rect.right, rect.left + int32_t(dest->alloc_width - host_x));
        rect.bottom = std::min(rect.bottom, rect.top + int32_t(dest->alloc_height - host_y));
        const plume::RenderRect dest_rect(int32_t(host_x), int32_t(host_y),
                                          int32_t(host_x) + (rect.right - rect.left),
                                          int32_t(host_y) + (rect.bottom - rect.top));
        const bool multisampled = surface->samples != plume::RenderSampleCount::COUNT_1;
        // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): left in the target's image when it can be.
        const bool colour_handed =
            ColourHandOverResolveLocked(surface, dest, rect, host_x, host_y, dest_level == 0 && slice == 0 && z == 0);
        if (colour_handed) {
          // Nothing drawn: binds of the destination sample the target's image.
        } else if (multisampled && dest->extra_levels && IsVulkan()) {
          // A destination with host levels takes it through a single-level
          // target (ResolveStagingLocked): plume's Vulkan resolve writes every
          // level of its destination (D3D12's writes level 0 only, and
          // resolves in place below). Without one (device lost) nothing of
          // the resolve lands.
          if (HostTexture *staging = ResolveStagingLocked(dest)) {
            TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
            TransitionTextureLocked(staging, plume::RenderTextureLayout::RESOLVE_DEST);
            cmd->resolveTextureRegion(staging->texture.get(), host_x, host_y,
                                      surface->texture.get(), &rect);
            TransitionTextureLocked(staging, plume::RenderTextureLayout::COPY_SOURCE);
            TransitionTextureLocked(dest, plume::RenderTextureLayout::COPY_DEST);
            const plume::RenderBox box(dest_rect.left, dest_rect.top, dest_rect.right,
                                       dest_rect.bottom);
            cmd->copyTextureRegion(
                plume::RenderTextureCopyLocation::Subresource(dest->texture.get(), 0, slice),
                plume::RenderTextureCopyLocation::Subresource(staging->texture.get()), host_x,
                host_y, z, &box);
          }
        } else if (multisampled && colour_sample >= 0 &&
                   DrawResolveCopy(surface, dest, rect, host_x, host_y, colour_sample)) {
          // fnxf_xiii: one sample (D3DRESOLVE_FRAGMENTn), drawn.
        } else if (multisampled) {
          TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
          TransitionTextureLocked(dest, plume::RenderTextureLayout::RESOLVE_DEST);
          cmd->resolveTextureRegion(dest->texture.get(), host_x, host_y, surface->texture.get(),
                                    &rect);
        } else if (IsDeko3D() && fnxf_xiii::renut_gpu::DrawResolvesOn() && slice == 0 && z == 0 &&
                   DrawResolveCopy(surface, dest, rect, host_x, host_y)) {
          // fnxf_xiii (2026-10-18): drawn, not copied (deko3d).
        } else {
          TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COPY_SOURCE);
          TransitionTextureLocked(dest, plume::RenderTextureLayout::COPY_DEST);
          const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(dest->texture.get(), 0, slice),
              plume::RenderTextureCopyLocation::Subresource(surface->texture.get()), host_x,
              host_y, z, &box);
        }
        TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);
        if (dest->extra_levels) {
          UpdateResolveMipsLocked(dest, dest_rect);
          // The draw path binds its own layout, pipeline and viewport again.
          g_bound.pipeline = nullptr;
          g_bound.layout = false;
          g_bound.viewport_set = false;
        }
      }
    }
  }

  // Resolve-and-clear.
  if ((flags & 0x300) && surface) {
    Targets t;
    if (surface->depth)
      t.depth = surface;
    else
      t.colors[t.color_count++] = surface;
    t.width = surface->width;
    t.height = surface->height;
    t.scale = surface->scale;
    const plume::RenderRect rect = ClampRect(source_rect, t.width, t.height);
    static const float kBlack[4] = {0, 0, 0, 0};
    const u32 clear_flags = ((flags & 0x100) ? 1u : 0u) | ((flags & 0x200) ? 0x30u : 0u);
    ClearTargets(t, clear_flags, rect, clear_color ? clear_color : kBlack, clear_depth,
                 clear_stencil);
  }
}

// fnxf_xiii (2026-11-08, FSR 3.1 before the post-processing): see EarlyFsrState. At a draw while resolves of the scene's colour
// target since its last scene draw are kept: a draw into another target is the post-processing's first.
void EarlyFsrBeforePostLocked(const GuestDevice &device) {
#if defined(PLUME_DEKO3D)
  MotionTargetState &mt = g_motion_target;
  HostSurface *scene = mt.scene;
  if (!EarlyFsrOn() || g_early_fsr.frame != mt.frame || mt.cleared_frame != mt.frame || !scene || !scene->texture ||
      g_early_fsr.ran_frame == mt.frame) {
    if (g_early_fsr.ran_frame != mt.frame)
      g_early_reason = !EarlyFsrOn() ? 11 : g_early_fsr.frame != mt.frame ? 12 : mt.cleared_frame != mt.frame ? 13 : 14;
    g_early_fsr.resolves.clear();
    return;
  }
  if (device.U32(d3d::kDeviceOffsetRenderTargets) == scene->guest_va) {
    g_early_reason = 2;
    return;  // still the scene's target (the restores after the opaque pass, its transparent draws)
  }
  std::vector<SceneResolve> resolves = std::move(g_early_fsr.resolves);
  g_early_fsr.resolves.clear();
  const Fsr3SceneInputs in = MotionSceneInputsLocked();
  g_early_reason = 3;
  if (!in.width || !in.height)
    return;
  auto &s = State();
  // The output phase from here: the scene's target's variant of it is FSR's output; the post-processing's 1280x720
  // targets are promoted as they are drawn (DrawStateLocked's promotion, g_output_copied_frame).
  bool changed = SetOutputPhaseLocked(true);
  changed |= PromoteOutputTargetLocked(scene->out_key);
  if (const d3d::SurfaceDesc *desc = SurfaceDescLocked(scene->guest_va))  // its header now (may differ from its build's)
    changed |= PromoteOutputTargetLocked(OutputTargetKey(*desc));
  if (changed)
    BumpSurfaceEpochLocked();
  g_output_copied_frame = TextureFrameLocked();
  // fnxf_xiii (2026-11-08): another scene than 1280x720 takes nothing of the surface over the scene's address now (FSR's
  // output size is worked out below; the scene's own target gives the format): that header is whatever the game rewrote
  // the surface to, at a camera cut the HUD's depth, and FSR did not run then (the frame after a load, the chapter
  // screen's cuts): the post-processing read the scene's resolves at the dynamic resolution's part, the first frame of
  // the field showed the chapter screen's view half drawn.
  // fnxf_xiii (2026-11-09): nor a 1280x720 scene's: since FSR runs at the first draw that reads the scene (s202), the
  // surface over that address at that draw was now the promoted target, now the game's other one: FSR's output switched
  // between 1920x1080 and 1280x720 frame by frame after a load at 1280x720 with dynamic resolution (its textures made
  // anew 590 times in 43 s, the GPU at full load, the post-processing reading the other size's stale picture every
  // other frame) until the textures could not be made any more and FSR turned itself off.
  HostSurface *up = scene;
  g_early_reason = 4;
  if (!up || !up->texture || up->depth)
    return;
  const uint32_t slot = SurfaceSampleSlotLocked(scene);
  g_early_reason = 5;
  if (slot == kInvalidDescriptor)
    return;
  const uint32_t image = plume::Deko3DImageId(s.texture_set.get(), slot);
  // fnxf_xiii (2026-11-08): the output's scale and size: the output's size over the scene's (3/2, 6/5, 15/8, 5/4, 1;
  // 2026-11-09 for a 1280x720 scene too, see above). The target's header is not of the scene's size there
  // (1600x900: four 400x900 tiles, then a 1280x720 header over it at this point, whose variant is 1.5x: FSR went to
  // 2400x1350 in the first test).
  RenderScale out_scale{};  // not dynamic
  uint32_t out_w = 0, out_h = 0;
  {
    const u32 rh = fnxf_xiii::RenderHeight();
    g_early_reason = 7;
    if (!RenderScale::FromRatio(Output1080p() ? 1080u : 720u, rh, out_scale))
      return;
    out_w = out_scale.Size(rh * 16 / 9);
    out_h = out_scale.Size(rh);
  }
  static uint32_t logged = 0;
  if (logged++ < 4)
    RGPU_INFO("[fsr3] before the post-processing: scene {}x{} (target {}x{} host) -> {}x{} host (code {}), {} resolve(s) "
              "again",
              in.width, in.height, scene->scale.Size(scene->width), scene->scale.Size(scene->height), out_w, out_h,
              out_scale.code, resolves.size());
  // The output stays in FSR's texture and goes straight into the resolves' textures below (not into 'up' first).
  g_early_reason = 6;
  if (!Fsr3UpscaleLocked(image, in.width, in.height, up, in, false, out_w, out_h))
    return;
  g_early_reason = 9;
  mt.native_frame = mt.frame;  // not again at the picture's copy
  g_early_fsr.ran_frame = mt.frame;
  g_motion_target.layout = plume::RenderTextureLayout::SHADER_READ;
  // The resolves again, from the upscaled scene (of its scale; not dynamic).
  std::vector<std::pair<HostTexture *, plume::RenderRect>> mips;
  for (const SceneResolve &r : resolves) {
    HostTexture *dest = GetResolveTextureLocked(r.dest_va, up->format, out_scale, &r.format);
    if (!dest || !dest->texture)
      continue;
    plume::RenderRect rect = ScaleRect(plume::RenderRect(r.src.x1, r.src.y1, r.src.x2, r.src.y2), out_scale);
    const uint32_t host_x = uint32_t(out_scale.Coord(r.dest_x));
    const uint32_t host_y = uint32_t(out_scale.Coord(r.dest_y));
    if (host_x >= dest->alloc_width || host_y >= dest->alloc_height)
      continue;
    rect.right = std::min(rect.right, rect.left + int32_t(dest->alloc_width - host_x));
    rect.bottom = std::min(rect.bottom, rect.top + int32_t(dest->alloc_height - host_y));
    rect.right = std::min(rect.right, int32_t(out_w));
    rect.bottom = std::min(rect.bottom, int32_t(out_h));
    if (rect.right <= rect.left || rect.bottom <= rect.top)
      continue;
    dest->resolve_signature = 0;
    if (dest->drs_code != 0) {
      dest->drs_code = 0;
      ++g_texture_epoch;
    }
    g_bound.framebuffer = nullptr;
    const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
    if (g_load_log_active)  // fnxf_xiii (2026-11-08, fnxf_xiii_diag_load_draw_log)
      g_load_lines.push_back(fmt::format("  early FSR copy {},{}-{},{} -> tex {:08X} slot {:X} {}x{} c{} at {},{}",
                                         rect.left, rect.top, rect.right, rect.bottom, r.dest_va, dest->slot,
                                         dest->alloc_width, dest->alloc_height, dest->scale.code, host_x, host_y));
    if (!Fsr3CopyOutputLocked(dest->texture.get(), host_x, host_y, box))
      continue;
    dest->layout = plume::RenderTextureLayout::COPY_DEST;
    TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);
    if (dest->extra_levels) {
      // The levels once per texture, over all its resolves (the per-tile ones: two or four a texture).
      const plume::RenderRect dest_rect(int32_t(host_x), int32_t(host_y), int32_t(host_x) + (rect.right - rect.left),
                                        int32_t(host_y) + (rect.bottom - rect.top));
      auto it = std::find_if(mips.begin(), mips.end(), [&](const auto &m) { return m.first == dest; });
      if (it == mips.end()) {
        mips.emplace_back(dest, dest_rect);
      } else {
        it->second.left = std::min(it->second.left, dest_rect.left);
        it->second.top = std::min(it->second.top, dest_rect.top);
        it->second.right = std::max(it->second.right, dest_rect.right);
        it->second.bottom = std::max(it->second.bottom, dest_rect.bottom);
      }
    }
  }
  for (const auto &[dest, rect] : mips)
    UpdateResolveMipsLocked(dest, rect);
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
#else
  (void)device;
  g_early_fsr.resolves.clear();
#endif
}

bool ReadRenderTargetLocked(const u8 *device_state, u32 source, u32 &width, u32 &height,
                            std::vector<u32> &words) {
  auto &s = State();
  if (!s.target_readback_pipeline || !Video::EnsureCommandListLocked())
    return false;
  SyncBound(s);
  const GuestDevice device(device_state);
  if (!device.valid() || source >= d3d::kMaxRenderTargets)
    return false;
  HostSurface *surface =
      GetHostSurfaceLocked(device.U32(d3d::kDeviceOffsetRenderTargets + source * 4));
  if (!surface || surface->depth || !surface->texture)
    return false;
  width = std::min(width, surface->width);
  height = std::min(height, surface->height);
  if (!width || !height)
    return false;
  static const uint32_t kProfileLabel = gpu_profile::Label("render target readback");
  gpu_profile::Mark(s, kProfileLabel);

  // A single-sampled copy of the rect, which the shader reads by its slot.
  const RenderScale scale = surface->scale;
  const plume::RenderRect rect(0, 0, int32_t(scale.Size(width)), int32_t(scale.Size(height)));
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      uint32_t(rect.right), uint32_t(rect.bottom), 1, surface->format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  std::unique_ptr<plume::RenderTexture> copy = s.device->createTexture(desc);
  if (!TextureCreated(copy.get()))
    return false;
  std::unique_ptr<plume::RenderTextureView> view =
      copy->createTextureView(plume::RenderTextureViewDesc::Texture2D(surface->format));
  const uint64_t bytes = uint64_t(width) * height * 4;
  std::unique_ptr<plume::RenderBuffer> output = s.device->createBuffer(
      plume::RenderBufferDesc::DefaultBuffer(bytes, StorageBufferFlags()));
  std::unique_ptr<plume::RenderBuffer> readback =
      s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
  std::unique_ptr<plume::RenderDescriptorSet> set = detail::CreateTargetReadbackSet(s);
  const uint32_t slot = view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
  if (!view || !output || !readback || !set || slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(slot, copy.get(), view.get());
  set->setBuffer(0, output.get());

  plume::RenderCommandList *cmd = s.command_list;
  g_bound.framebuffer = nullptr;
  if (surface->samples != plume::RenderSampleCount::COUNT_1) {
    TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(copy.get(), plume::RenderTextureLayout::RESOLVE_DEST));
    cmd->resolveTextureRegion(copy.get(), 0, 0, surface->texture.get(), &rect);
  } else {
    TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COPY_SOURCE);
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(copy.get(), plume::RenderTextureLayout::COPY_DEST));
    const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(copy.get()),
                           plume::RenderTextureCopyLocation::Subresource(surface->texture.get()),
                           0, 0, 0, &box);
  }
  const plume::RenderTextureBarrier read(copy.get(), plume::RenderTextureLayout::SHADER_READ);
  const plume::RenderBufferBarrier write(output.get(), plume::RenderBufferAccess::WRITE);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE, &write, 1, &read, 1);
  cmd->setComputePipelineLayout(s.target_readback_layout.get());
  cmd->setPipeline(s.target_readback_pipeline.get());
  cmd->setComputeDescriptorSet(s.texture_set.get(), 0);
  cmd->setComputeDescriptorSet(set.get(), 1);
  TargetReadbackPush push{};
  push.texture_index = slot;
  push.width = width;
  push.height = height;
  push.scale_numerator = RenderScale::kNumerators[scale.code];
  push.scale_denominator = RenderScale::kDenominators[scale.code];
  cmd->setComputePushConstants(0, &push, 0, sizeof(push));  // fnxf_xiii: the size (the record worker copies it)
  cmd->dispatch((width + 7) / 8, (height + 7) / 8, 1);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderBufferBarrier(output.get(), plume::RenderBufferAccess::READ));
  cmd->copyBufferRegion(plume::RenderBufferReference(readback.get()),
                        plume::RenderBufferReference(output.get()), bytes);
  // The compute pipeline replaced the graphics one (D3D12), and the barriers
  // ended any render pass.
  g_bound.pipeline = nullptr;
  g_bound.layout = false;

  detail::SubmitAndWaitLocked(s);
  bool read_back = false;
  if (const auto *data = static_cast<const u32 *>(readback->map())) {
    words.assign(data, data + size_t(width) * height);
    readback->unmap();
    read_back = true;
  }
  Video::FreeTextureSlotLocked(slot);
  Video::RetireLocked(std::move(view));
  Video::RetireLocked(std::move(copy));
  Video::RetireLocked(std::move(set));
  Video::RetireLocked(std::move(output));
  Video::RetireLocked(std::move(readback));
  return read_back;
}

bool FrontBufferWrittenLocked(u32 texture_va) {
  const HostTexture *texture = FindHostTextureLocked(texture_va);
  return texture && texture->texture && texture->gpu_written;
}

namespace {
i32 g_scaler_source[4] = {};  // the last Swap's (FrontBufferImageLocked)
}  // namespace

// fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): see draw.h.
void ColourHandOverBeforeCopyIntoLocked(HostSurface *surface) {
  if (surface && surface->handed && !surface->depth) {
    g_colour_mat_why = 10;
    ColourHandOverBeforeWriteLocked(surface, 0);
  }
}

PresentImage FrontBufferImageLocked(u32 texture_va) {
  return FrontBufferImageLocked(texture_va, g_scaler_source[0], g_scaler_source[1], g_scaler_source[2],
                                g_scaler_source[3]);
}

PresentImage FrontBufferImageLocked(u32 texture_va, i32 x1, i32 y1, i32 x2, i32 y2) {
  g_scaler_source[0] = x1;
  g_scaler_source[1] = y1;
  g_scaler_source[2] = x2;
  g_scaler_source[3] = y2;
  PresentImage image;
  HostTexture *texture = FindHostTextureLocked(texture_va);
  if (!texture || !texture->texture || texture->slot == UINT32_MAX)
    return image;
  image.texture = texture->texture.get();
  image.layout = &texture->layout;
  image.descriptor = texture->slot;
  // fnxf_xiii (2026-11-17, fnxf_xiii_colour_handover): a front buffer whose resolve was left in its target's image is shown from
  // that image, through a view with the texture's own mapping (fnxf_xiii_colour_handover_present); if it cannot be, the
  // resolve is drawn into the texture first.
  if ((texture->handed_from || texture->held_current) && Video::EnsureCommandListLocked()) {
    static const bool direct = REXCVAR_GET(fnxf_xiii_colour_handover_present);
    uint32_t slot = UINT32_MAX;
    if (direct && HoldsColour(*texture))
      slot = DepthHandOverSlotLocked(*texture, texture->view_desc.componentMapping, nullptr);
    if (slot != UINT32_MAX && texture->handed_from && texture->handed_from->texture) {
      image.texture = texture->handed_from->texture.get();
      image.layout = &texture->handed_from->layout;
      image.descriptor = slot;
    } else if (slot != UINT32_MAX && texture->held && texture->held_current && texture->held->texture) {
      image.texture = texture->held->texture.get();
      image.layout = &texture->held->layout;
      image.descriptor = slot;
    } else {
      g_colour_mat_why = 8;
      DepthHandOverMaterialiseLocked(*texture);
    }
  }
  image.width = texture->alloc_width;
  image.height = texture->alloc_height;
  // fnxf_xiii (2026-11-07, dynamic resolution): a front buffer resolved from a dynamic-resolution target (a 1280x720
  // scene's frame without its copy into the output's targets: menus) shows its drawn part.
  const float fill = texture->scale.drs ? DrsFillOfCode(texture->drs_code) : 1.0f;
  auto drawn = [&](u32 guest) {
    return std::max<uint32_t>(uint32_t(std::lround(double(texture->scale.Size(guest)) * double(fill))), 1u);
  };
  image.source_width = drawn(texture->width);
  image.source_height = drawn(texture->height);
  // fnxf_xiii: the scaler's source rect (Swap), when it is a part of the texture from its corner.
  if (x1 == 0 && y1 == 0 && x2 > 0 && y2 > 0 && u32(x2) <= texture->width && u32(y2) <= texture->height) {
    image.source_width = drawn(u32(x2));
    image.source_height = drawn(u32(y2));
  } else if (x2 > x1 && (x1 || y1)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("Swap: scaler source ({},{})-({},{}) not from the corner of the {}x{} front buffer: shown whole", x1, y1,
                x2, y2, texture->width, texture->height);
    }
  }
  return image;
}

// fnxf_xiii (2026-10-19, render-thread proposal 1): see draw.h.
bool GuestRangeCardsClean(const u8 *data, size_t bytes, uint64_t proven) {
  return PagesClean(data, bytes, proven);
}

uint64_t CardsInterval() {
  return g_flush_serial;
}

// fnxf_xiii (2026-10-27): draw.h, the textures made from memexport windows on the GPU.
// fnxf_xiii (2026-10-27, fnxf_xiii_diag_watch): for the textures' log (textures.cpp).
bool WatchedRangeLocked(u32 base, u32 size) {
  return WatchHit(base, size);
}

// fnxf_xiii (2026-10-27): video.cpp's fence waits, for the [burst] line.
void NoteGpuWaitMsLocked(double ms) {
  g_burst.gpu_wait_ms += ms;
}

bool ExportsPendingForRangeLocked(u32 base, u32 size) {
  return base && size && PendingExportSlots(base, size) != 0;
}

bool ExportWindowForRangeLocked(u32 base, u32 size, u32 &slot, uint64_t &offset) {
  return IsDeko3D() && State().stream_convert_pipeline && ExportsPendingForRangeLocked(base, size) &&
         FindExportWindow(base, size, slot, offset) &&
         (!ZeroCopyOn() || ZeroCopyReadOnGpuLocked(base, size));  // fnxf_xiii (2026-10-30): a kernel's output
}

// fnxf_xiii (2026-10-30): draw.h.
bool ExportsZeroCopyLocked() {
  return ZeroCopyOn() && State().stream_convert_pipeline != nullptr;
}

void GuestRangeForGpuLocked(u32 base, u32 size) {
  guest_gpu::CleanForGpu(base, size);
}

void GuestRangeWrittenByHostLocked(u32 base, u32 size) {
  guest_gpu::CleanForGpuNow(base, size);
}

bool UntileFromExportLocked(const ExportUntile &u, plume::RenderBufferReference &out) {
  auto &s = State();
  // fnxf_xiii (2026-11-05): source kFramesInFlight is the slot's own stream buffer (UntileFromGuestLocked).
  if (!s.stream_convert_pipeline || u.slot > kFramesInFlight || (u.slot < kFramesInFlight && !ExportBuffer(s, u.slot)) ||
      u.log2_block_bytes < 2 || u.log2_block_bytes > 4 || !u.blocks_w || !u.blocks_h)
    return false;
  uint64_t &used = s.exported_streams_used[s.frame];
  const uint64_t bytes = uint64_t(u.row_pitch) * u.blocks_h;
  plume::RenderBuffer *dest = StreamConvertDestLocked(s, bytes + 512);
  if (!dest)
    return false;
  used = (used + 511u) & ~uint64_t(511);  // a copy's placed footprint: 512-byte aligned, as the upload staging
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier barriers[kFramesInFlight + 1];
  u32 barrier_count = 0;
  barriers[barrier_count++] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE);
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (plume::RenderBuffer *window = ExportBuffer(s, slot)) {  // fnxf_xiii (2026-10-30): ExportBuffer
      barriers[barrier_count++] = plume::RenderBufferBarrier(window, plume::RenderBufferAccess::WRITE);
      if (ZeroCopyOn())
        break;
    }
  }
  // fnxf_xiii (2026-10-30, ZeroCopyOn): no L2 invalidation (UploadVertexFetchData says why).
  static const uint32_t kProfileLabel = gpu_profile::Label("textures untiled from memexport windows [bytes]");
  gpu_profile::Mark(s, kProfileLabel);
  gpu_profile::AddAmount(s, kProfileLabel, bytes);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE, barriers, barrier_count);
  cmd->setComputePipelineLayout(s.stream_convert_layout.get());
  cmd->setPipeline(s.stream_convert_pipeline.get());
  cmd->setComputeDescriptorSet(s.stream_convert_set.get(), 0);
  StreamConvertPush push{};
  push.source = u.slot;
  push.source_offset = u32(u.source_offset);
  push.dest_offset = u32(used);
  push.vertices = u.blocks_w * u.blocks_h;
  push.guest_stride_words = u.pitch_blocks;
  push.host_stride_words = u.blocks_w;
  push.element_count = 0x80000000u | (u.log2_block_bytes << 8) | (u.endian & 3u) |
                       (u.linear ? 0x10000u : 0u);  // fnxf_xiii (2026-10-30): a linear level
  push.pad = (u.x_blocks & 0xFFFFu) | (u.y_blocks << 16);
  push.elements[0] = u.row_pitch;
  cmd->setComputePushConstants(0, &push, 0, sizeof(push));
  cmd->dispatch((push.vertices + 63) / 64, 1, 1);
  cmd->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ));
  out = plume::RenderBufferReference(dest, used);
  used += (bytes + 511u) & ~uint64_t(511);
  // The compute pipeline replaced the graphics one, and the barriers end any render pass.
  g_bound.pipeline = nullptr;
  g_bound.framebuffer = nullptr;
  static uint32_t logged = 0;
  if (logged++ < 8)
    FNXF_XIII_BURST_LOG(RGPU_INFO, "[exporttex] level of {}x{} blocks {} on the GPU from slot {} +{:X} ({} bytes)", u.blocks_w,
              u.blocks_h, u.linear ? "copied" : "untiled", u.slot, u.source_offset, bytes);
  return true;
}

bool UntileFromGuestLocked(ExportUntile u, const uint8_t *guest, uint32_t guest_bytes, plume::RenderBufferReference &out) {
  auto &s = State();
  if (!IsDeko3D() || !s.stream_convert_pipeline || !guest || !guest_bytes || !s.command_list ||
      u.log2_block_bytes < 2 || u.log2_block_bytes > 4 || !u.blocks_w || !u.blocks_h)
    return false;
  const uint64_t raw = (uint64_t(guest_bytes) + 511u) & ~uint64_t(511);
  const uint64_t rows = uint64_t(u.row_pitch) * u.blocks_h;
  UploadAllocation staging = s.uploads[s.frame].Allocate(raw, 512);
  if (!staging)
    return false;
  ParallelMemcpy(staging.cpu, guest, guest_bytes);  // fnxf_xiii (2026-11-05, seen15): over the copy helpers
  // Room for the copy and the rows (UntileFromExportLocked asks for its rows again: they fit after this).
  plume::RenderBuffer *dest = StreamConvertDestLocked(s, raw + rows + 1024);
  if (!dest)
    return false;
  uint64_t &used = s.exported_streams_used[s.frame];
  used = (used + 511u) & ~uint64_t(511);
  const uint64_t raw_at = used;
  if (raw_at + raw > 0xFFFFFFFFull)
    return false;  // the kernel's offsets are 32-bit
  used += raw;
  plume::RenderCommandList *cmd = s.command_list;
  cmd->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE));
  cmd->copyBufferRegion(plume::RenderBufferReference(dest, raw_at), staging.ref, guest_bytes);
  // A copy stage barrier (deko3d: Full, which waits for the copy): the kernel's own barrier is a compute one.
  cmd->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ));
  g_bound.framebuffer = nullptr;  // the barriers end any render pass
  u.slot = kFramesInFlight;  // the stream buffer itself (StreamConvertDestLocked's set: its last source)
  u.source_offset = raw_at;
  u.linear = false;
  return UntileFromExportLocked(u, out);  // its barrier before the dispatch orders the copy's writes
}

} // namespace renut::gpu
