// fnxf_xiii (2026-10-25, fnxf_xiii_render_720p): the game's 3D scene at 1280x720 instead of 1024x576 (the boot menu's
// "Resolution").
//
// sub_8272D760 builds the engine's start-up parameters (the CDev framework's, extended by GameMain's sub_82201C90 and
// kept as its singleton at 0x82AAA004): +24/+28 = 1024 x 576, +36/+40 = 1280 x 720, the only place in the code where
// these two sizes are set together. Taken to be the scene's render size and the output's (the game's UI is 1280x720
// and its upscale draw scales the 1024x576 picture to 1280x720). Here +24/+28 become 1280 x 720 right after it.
// A first step, to be confirmed on the console: the log line says what was there and what was written; if the scene
// is still drawn at 1024x576 (the upscale draw's c0 in the log's [upscale] line stays 1024 x 576), the render size is
// taken from elsewhere.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>

#include <rex/system/kernel_state.h>  // fnxf_xiii (2026-11-11): the pool monitor (guest physical memory)
#include <rex/system/xmemory.h>
#include "renut_engine/gpu/guest/guest_records.h"
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(fnxf_xiii_render_720p, false, "Game/Enhancements",
                    "Experimental: the game's 3D scene at 1280x720 instead of its own 1024x576 (the boot menu's "
                    "\"Resolution\"; the upscaler is not used then). About 56% more GPU work a frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// fnxf_xiii (2026-11-07): the scene's height (16:9: 576 = 1024x576, the game's; 720, 900, 1080). Over render_720p.
REXCVAR_DEFINE_INT32(fnxf_xiii_render_height, 576, "Game/Enhancements",
                     "The game's 3D scene height, 16:9: 576 (1024x576, the game's own), 720 (1280x720), 900 "
                     "(1600x900) or 1080 (1920x1080). Above 720 the scene is drawn larger than the console's 1280x720 "
                     "output: shown at 1080p docked (a 1080p TV), scaled down in hand")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fnxf_xiii {
// The scene's height in effect (fnxf_xiii_render_height, or 720 with fnxf_xiii_render_720p).
uint32_t RenderHeight() {
  const int32_t h = REXCVAR_GET(fnxf_xiii_render_height);
  if (h == 720 || h == 900 || h == 1080)
    return uint32_t(h);
  return REXCVAR_GET(fnxf_xiii_render_720p) ? 720u : 576u;
}
}  // namespace fnxf_xiii

REX_EXTERN(__imp__sub_8272D760);

namespace {
uint32_t Load(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
void Store(uint8_t *base, uint32_t ea, uint32_t v) {
  *reinterpret_cast<uint32_t *>(base + ea) = __builtin_bswap32(v);
}
}  // namespace

extern "C" REX_FUNC(sub_8272D760) {
  const uint32_t params = ctx.r3.u32;
  __imp__sub_8272D760(ctx, base);
  if (!params)
    return;
  const uint32_t w = Load(base, params + 24), h = Load(base, params + 28);
  const uint32_t out_w = Load(base, params + 36), out_h = Load(base, params + 40);
  const uint32_t rh = fnxf_xiii::RenderHeight(), rw = rh * 16 / 9;
  if (rh != 576 && w == 1024 && h == 576 && out_w == 1280 && out_h == 720) {
    Store(base, params + 24, rw);
    Store(base, params + 28, rh);
    REXLOG_INFO("[resolution] start-up parameters at {:08X}: scene {}x{} -> {}x{} (output {}x{})", params, w, h, rw, rh,
                out_w, out_h);
  } else {
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[resolution] start-up parameters at {:08X}: scene {}x{}, output {}x{}", params, w, h, out_w, out_h);
  }
}

// fnxf_xiii (2026-11-07, fnxf_xiii_render_height above 720): sub_827C3C60 gives the number of tiles the game splits a
// render target into for EDRAM (predicated tiling, up to 4 side by side: sub_827C4060 builds 4 rectangles): 1 at
// 1024x576 or less, else its table at 0x82127DE8 by the multisample mode (1x 1, 2x 2, 4x 4). The game's EDRAM allocator
// (sub_827C42C0, a free list of up to ~35 inline 76-byte blocks) has no fallback when a target does not fit: its node
// bitmap runs out (sub_827C38D8 returns -1) and the list is corrupted, the main thread then spins in the list walk
// (a 1600x900 or 1920x1080 scene hung at start, 2026-11-07). A larger scene gets more tiles: the fewest (up to 4) at
// which a colour and a depth target of a tile fit in about the room a 1280x720 2x scene takes (2 x 720 EDRAM tiles of
// 80x16 samples: here 1728 for both, of the 2048).
REX_EXTERN(__imp__sub_827C3C60);

extern "C" REX_FUNC(sub_827C3C60) {
  const uint32_t list = ctx.r3.u32, fallback = ctx.r4.u32;
  __imp__sub_827C3C60(ctx, base);
  if (fnxf_xiii::RenderHeight() <= 720 || !ctx.r3.u32)
    return;
  uint32_t s = list ? Load(base, list) : 0;
  if (!s)
    s = fallback;
  if (!s)
    return;
  const uint32_t w = Load(base, s + 12), h = Load(base, s + 16), ms = Load(base, s + 28);
  if (w <= 1280 && h <= 720)
    return;
  const uint32_t samples = ms == 0 ? 1 : ms == 1 ? 2 : 4;
  uint32_t tiles = ctx.r3.u32;
  auto edram = [&](uint32_t t) { return ((w / t + 79) / 80) * ((h + 15) / 16) * samples; };
  // And a count that divides the width: the game's tiles are w / tiles wide side by side, so 1600 / 3 = 533 left the
  // last column undrawn (1599 wide: FSR 3.1 took the scene as 1599x900, 2026-11-07).
  while (tiles < 4 && (2 * edram(tiles) > 1728 || w % tiles != 0))
    ++tiles;
  static uint32_t logged = 0;
  if (tiles != ctx.r3.u32 && logged < 4) {
    ++logged;
    REXLOG_INFO("[resolution] EDRAM tiling of a {}x{} target ({} samples): {} tiles instead of {}", w, h, samples,
                tiles, ctx.r3.u32);
  }
  ctx.r3.u64 = tiles;
}

// fnxf_xiii (2026-11-07): GameMain's start-up parameters (sub_82201C90, right after sub_8272D760) give the game's memory
// pools their sizes: +76..+136 its heaps (summed against +140), +144 0xCC00000 (204 MB) of write-combined physical
// memory (MmAllocatePhysicalMemoryEx, protect 0x20000404: textures and render targets), +148 0x600000. A 1920x1080
// scene's targets did not fit in +144 and the field's load waited forever for room (no allocation failed in the
// kernel; a 1600x900 scene fits). Grown before sub_821F37F8 (the next call, then sub_82200BD8) makes the pools.
// 64 MB more left too little guest physical memory for Renut's own 64 MB record heap ("Reserving 67108864 bytes of
// guest physical memory failed"); 32 MB loads the field. 1600x900 needs none (not grown: the memory stays free).
// fnxf_xiii (2026-11-11): 32 MB was not enough at 1080p either: the first battle against a new enemy type never
// showed its results (the game waited for room, the background still drawn: three times, at 1080p with dynamic
// resolution), and 1600x900 (not grown) crashed at a movie's start (a call through a bad pointer). Grown more now,
// and at 900p too, measured by the monitor below (the pool's touched part, the guest physical memory left, the
// record heap's peak): to be lowered to what is used. Renut's record heap takes what is left (64 MiB down to 16:
// guest_records.cpp).
// Measured (the pool's own accounting below, 2026-11-11, at 1080p through that FMV, battle, cutscene and the battle
// that hung): its peak 237.0 MB, above the 236 MB it had (204 + 32): the hang. Now +64 at 1080p (268 MB: 31 MB, 13 %
// over that peak; 50 MB of guest physical memory left, the record heap 48 MiB, its peak 0) and +48 at 900p (252 MB:
// its render targets 0.69 of 1080p's, the peak there estimated at 223 MB at most).
REXCVAR_DEFINE_INT32(fnxf_xiii_render_pool_extra_mb, 64, "Game/Enhancements",
                     "With a 1920x1080 scene (fnxf_xiii_render_height): this many MB more for the game's render "
                     "target and texture pool (its start-up parameter +144, 204 MB)")
    .range(0, 160)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fnxf_xiii_render_pool_extra_900_mb, 48, "Game/Enhancements",
                     "With a 1600x900 scene: this many MB more for the game's render target and texture pool")
    .range(0, 160)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// fnxf_xiii (2026-11-11): off by default (the sizes are set); the hooks below log only a pool without room (once).
REXCVAR_DEFINE_INT32(fnxf_xiii_render_pool_monitor_s, 0, "Game/Diagnostics",
                     "Every this many seconds, log the render target and texture pool's touched part, the guest "
                     "physical memory left and the record heap's peak, when they changed (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
// fnxf_xiii (2026-11-11): the pool's own accounting. Its heap object (82C03910; vtable 82120938, made by
// sub_82796630 from the start-up sizes) allocates with its vtable +12, sub_82799A88(heap, size, alignment, flags) ->
// a block (0 when there is no room), and frees with +16, sub_82799E38(heap, block). Only that heap's calls are
// counted: the bytes in use, their peak and the allocations that found no room (the game then waits and asks again).
std::mutex g_pool_mutex;
std::unordered_map<uint32_t, uint32_t> g_pool_blocks;  // block -> size
uint64_t g_pool_used = 0, g_pool_peak = 0, g_pool_failed = 0, g_pool_largest_failed = 0;
}  // namespace

REX_EXTERN(__imp__sub_82799A88);
REX_EXTERN(__imp__sub_82799E38);

extern "C" REX_FUNC(sub_82799A88) {
  const uint32_t heap = ctx.r3.u32, size = ctx.r4.u32;
  __imp__sub_82799A88(ctx, base);
  if (heap != Load(base, 0x82C03910u))
    return;
  const uint32_t block = ctx.r3.u32;
  std::lock_guard lock(g_pool_mutex);
  if (!block) {
    if (!g_pool_failed)
      REXLOG_WARN("[pool] the render target/texture pool had no room for {:.1f} MB ({:.1f} MB in use): the game waits "
                  "for room (fnxf_xiii_render_pool_extra_mb)", double(size) / (1 << 20), double(g_pool_used) / (1 << 20));
    ++g_pool_failed;
    g_pool_largest_failed = std::max<uint64_t>(g_pool_largest_failed, size);
    return;
  }
  g_pool_blocks[block] = size;
  g_pool_used += size;
  g_pool_peak = std::max(g_pool_peak, g_pool_used);
}

extern "C" REX_FUNC(sub_82799E38) {
  const uint32_t heap = ctx.r3.u32, block = ctx.r4.u32;
  if (heap == Load(base, 0x82C03910u)) {
    std::lock_guard lock(g_pool_mutex);
    auto it = g_pool_blocks.find(block);
    if (it != g_pool_blocks.end()) {
      g_pool_used -= it->second;
      g_pool_blocks.erase(it);
    }
  }
  __imp__sub_82799E38(ctx, base);
}

namespace {
// The pool (sub_829FE600: MmAllocatePhysicalMemory through sub_829875C0): its size 82C03914 (start-up +144), its
// address 82C22434. Fresh memory is zero: its 64 KiB pages that hold anything but zero (16 words of each looked at)
// are its "touched" part, never smaller than its peak use (a freed block stays touched; a block all zero is missed).
void PoolMonitor(uint8_t *base) {
  const int period = REXCVAR_GET(fnxf_xiii_render_pool_monitor_s);
  if (period <= 0)
    return;
  uint32_t last_touched = ~0u, last_free = ~0u, min_free = ~0u;
  size_t last_peak = ~size_t(0);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(period));
    const uint32_t size = Load(base, 0x82C03914u), pool = Load(base, 0x82C22434u);
    if (!size || pool < 0xA0000000u || uint64_t(pool) + size > 0x100000000ull)
      continue;
    uint32_t touched = 0;
    for (uint32_t page = 0; page < size; page += 0x10000u) {
      const uint32_t *p = reinterpret_cast<const uint32_t *>(base + pool + page);
      for (int k = 0; k < 16; ++k)
        if (p[k * 1024]) {
          ++touched;
          break;
        }
    }
    auto *mem = REX_KERNEL_MEMORY();
    uint32_t free_mb = 0;
    if (auto *heap = mem->LookupHeapByType(true, 64u * 1024u))
      free_mb = uint32_t((uint64_t(heap->GetUnreservedPageCount()) * 64u) >> 10);
    min_free = std::min(min_free, free_mb);
    size_t cap = 0, peak = 0;
    uint64_t oom = 0;
    renut::gpu::records::HeapStats(&cap, &peak, &oom);
    uint64_t used, used_peak, failed, largest_failed;
    size_t blocks;
    {
      std::lock_guard lock(g_pool_mutex);
      used = g_pool_used;
      used_peak = g_pool_peak;
      failed = g_pool_failed;
      largest_failed = g_pool_largest_failed;
      blocks = g_pool_blocks.size();
    }
    static uint64_t last_used = ~0ull;
    if (touched != last_touched || free_mb != last_free || peak != last_peak || used != last_used) {
      last_used = used;
      REXLOG_INFO("[pool] render target/texture pool {} MB at {:08X}: in use {:.1f} MB ({} blocks), peak {:.1f} MB, "
                  "{} allocations found no room (largest {:.1f} MB); {} MB touched; guest physical memory free {} MB "
                  "(least {}); record heap {} MB, peak {:.1f} MB{}",
                  size >> 20, pool, double(used) / (1 << 20), blocks, double(used_peak) / (1 << 20), failed,
                  double(largest_failed) / (1 << 20), touched / 16, free_mb, min_free, cap >> 20,
                  double(peak) / (1 << 20), oom ? fmt::format(", {} failed", oom) : std::string());
      last_touched = touched;
      last_free = free_mb;
      last_peak = peak;
    }
  }
}
}  // namespace

REX_EXTERN(__imp__sub_821F37F8);

extern "C" REX_FUNC(sub_821F37F8) {
  const uint32_t params = ctx.r3.u32;
  const uint32_t extra_mb = fnxf_xiii::RenderHeight() >= 1080 ? uint32_t(REXCVAR_GET(fnxf_xiii_render_pool_extra_mb))
                            : fnxf_xiii::RenderHeight() >= 900 ? uint32_t(REXCVAR_GET(fnxf_xiii_render_pool_extra_900_mb))
                                                              : 0u;
  static std::once_flag monitor;
  // std::thread::detach throws here (libnx: not implemented): the thread is kept, never joined (the process
  // ends with a hard exit).
  std::call_once(monitor, [base] { new std::thread(PoolMonitor, base); });
  if (params && extra_mb && Load(base, params + 144) == 0xCC00000u) {
    const uint32_t extra = extra_mb << 20;
    Store(base, params + 144, 0xCC00000u + extra);
    REXLOG_INFO("[resolution] the render target and texture pool: {} MB (+{} MB for the {}-line scene)",
                (0xCC00000u + extra) >> 20, extra >> 20, fnxf_xiii::RenderHeight());
  }
  __imp__sub_821F37F8(ctx, base);
}
