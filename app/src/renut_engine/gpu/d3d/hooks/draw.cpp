/**
 * @file    gpu/d3d/hooks/draw.cpp
 * @brief   Draw, clear and resolve entry points, and the shader setters.
 *
 * The draws replace N&B's D3D entirely: the originals would flush the device's
 * dirty state into GPU packets, including binding shaders by reading their
 * microcode, which renderer-owned shader objects do not carry. Clear, Resolve
 * and tiling queue their host work and do the device work of N&B's D3D
 * natively (d3d_native.h): dirty masks, the tiling state, fences on the
 * destination.
 *
 * The host work runs on the render thread (queued.h); a device recording a
 * command buffer records its draws, clears and resolves (command_buffers.h)
 * instead. Neither takes State().mutex here.
 */
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <mutex>
#include <map>
#include <unordered_map>
#include <string>

#include <rex/cvar.h>
#include <vector>

#include <rex/hook.h>

#include "renut_engine/gpu/render/draw.h"  // fnxf_xiii (2026-11-05): rtw ([rtwork])
#include "renut_engine/gpu/fnxf_xiii_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/texture_layout.h"  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare)
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;
namespace renut::gpu {
extern std::atomic<u32> g_glare_texture_va;  // render/draw.cpp (fnxf_xiii_diag_glare)
}

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): which game code submits the crystal's glare sprite (a draw of 6 vertices
// as a triangle list with a 256x256 k_8_8_8_8 texture in slot 0): the guest return address of the draw call and the
// stack's back chain, logged once per distinct chain ([glarecall]). The draw hooks below note their caller first.
REXCVAR_DECLARE(bool, fnxf_xiii_diag_glare);
// fnxf_xiii (2026-11-16): TEST, the same as the Xenia patch "TEST: crystal glare particles never pass the sphere
// test": sub_8284CD68's sphere test (sub_82843470) answers 0 (off screen) for every effect part, so none of its
// particles (the crystal glare's sparkles among them) are drawn. With it, Xenia still shows the crystal glare.
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_glare_cull_all, false, "Game/Diagnostics",
                    "fnxf_xiii test (2026-11-16): no effect particle drawn through sub_8284CD68 passes its sphere test "
                    "(the crystal glare's sparkles disappear): what is left of the glare comes from elsewhere")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace {
thread_local u32 t_draw_lr = 0, t_draw_sp = 0;
thread_local u32 t_cc90_lr = 0, t_cc90_r3 = 0;  // fnxf_xiii (2026-11-16): sub_8278CC90's caller (it draws the glare)
thread_local int t_item_glare = -1;  // fnxf_xiii (2026-11-16): glare draws inside the current sub_8278C470 (-1: none)

}  // namespace (the hook below has external linkage)

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): sub_8284CD68 (an effect's update and draw) draws the glare
// (sub_82847160) only when sub_82842F50(obj + 12, float(limit [r28+96]), mode [r28+98]) returns true: a counter at
// obj+12 against a limit, which in mode 2 returns false (no draw) while the counter is below it. Its calls from
// sub_8284CD68 are logged whenever an object's result changes ([glaretimer]).
namespace {
struct Call30E8 {
  u32 param = 0, counter_before = 0, counter_after = 0, type = 0, result = 0;
  int mode = -1;
  double limit = 0;
  int calls = 0;
};
thread_local Call30E8 t_30e8;          // the last call on this thread
thread_local int t_in_30e8 = 0;
std::unordered_map<u32, Call30E8> g_glare_30e8;  // param -> this Swap's (g_glaremodel_mutex)
u32 g_glare_param = 0;
}  // namespace

REX_EXTERN(__imp__sub_82842F50);
extern "C" REX_FUNC(sub_82842F50) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  const u32 lr = uint32_t(ctx.lr);
  if (on && t_in_30e8 && lr >= 0x828430E8u && lr < 0x82843310u) {  // fnxf_xiii (2026-11-16): for [glaretime]
    t_30e8.mode = int(ctx.r5.s8);
    t_30e8.limit = ctx.f1.f64;
  }
  if (!on || lr < 0x8284CD68u || lr > 0x8284D600u) {
    __imp__sub_82842F50(ctx, base);
    return;
  }
  const u32 obj = ctx.r3.u32;
  const double limit = ctx.f1.f64;
  const int mode = int(ctx.r5.s8);
  const auto *p = obj ? GuestPtr<const be_u32>(obj) : nullptr;
  const int32_t before = p ? int32_t(u32(*p)) : 0;
  __imp__sub_82842F50(ctx, base);
  const int32_t after = p ? int32_t(u32(*p)) : 0;
  const u32 result = ctx.r3.u32 & 0xFF;
  static std::mutex mutex;
  static std::unordered_map<u32, std::pair<u32, uint64_t>> last;  // obj -> (result + 1, calls since it changed)
  std::lock_guard lock(mutex);
  auto &e = last[obj];
  ++e.second;
  if (e.first != result + 1) {
    RGPU_INFO("[glaretimer] obj {:08X} lr {:08X}: {} (counter {} -> {}, limit {:g}, mode {}), the other answer for {} calls",
              obj, lr, result ? "draw" : "NO draw", before, after, limit, mode, e.second - 1);
    e.first = result + 1;
    e.second = 0;
  }
}

namespace {

}  // namespace

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): sub_8284CD68's other ways out before the glare's draw. Its first
// sub_8284C728 call (lr ~8284CE5C) is followed by a test of the float at its frame +108 against the constant at
// 820A8F38 (<= : no draw), and its sub_82829AB0 calls (lr ~8284D010 / 8284D164) return true when it gives up (no draw).
// Each is logged when its answer changes for an effect object (the caller's r31): [glarecull].
namespace {
float GuestFloat(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  const u32 w = p ? u32(*p) : 0;
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
std::mutex g_glarecull_mutex;
std::unordered_map<uint64_t, std::pair<int, uint64_t>> g_glarecull;  // (obj, test) -> (answer, calls since it changed)
void GlareCullNote(u32 obj, int test, int answer, const std::string &detail) {
  std::lock_guard lock(g_glarecull_mutex);
  auto &e = g_glarecull[uint64_t(obj) << 8 | uint64_t(test)];
  ++e.second;
  if (e.first != answer + 1) {
    RGPU_INFO("[glarecull] obj {:08X} test {}: {} {} (the other answer for {} calls)", obj, test,
              answer ? "PASS" : "FAIL", detail, e.second - 1);
    e.first = answer + 1;
    e.second = 0;
  }
}
}  // namespace

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): sub_8283B9D0 (a lens effect's draw: it calls sub_82847160) does
// nothing unless the w of the vector at [this+148] (its colour's alpha) is above the constant at 820A8F38. Each
// object's alpha is kept for its last 12 calls and logged when it falls to the threshold or leaves it ([glarealpha]).
REX_EXTERN(__imp__sub_8283B9D0);
namespace {
thread_local int t_b9d0_drew = -1;  // -1: not inside sub_8283B9D0
}
extern "C" REX_FUNC(sub_8283B9D0) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on) {
    __imp__sub_8283B9D0(ctx, base);
    return;
  }
  const u32 obj = ctx.r3.u32;
  const auto *vp = GuestPtr<const be_u32>(obj + 148);
  const u32 vec = vp ? u32(*vp) : 0;
  const float alpha = vec ? GuestFloat(vec + 12) : 0.0f;
  const int saved = t_b9d0_drew;
  t_b9d0_drew = 0;
  __imp__sub_8283B9D0(ctx, base);
  const int drew = t_b9d0_drew;
  t_b9d0_drew = saved;
  struct State {
    int sig = -1;
    uint64_t calls = 0;
    float a[12] = {};
    uint32_t n = 0;
  };
  static std::mutex mutex;
  static std::unordered_map<u32, State> objects;
  static uint32_t lines = 0;
  static const auto t0 = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  State &st = objects[obj];
  st.a[st.n++ % 12] = alpha;
  ++st.calls;
  const int sig = (alpha > 0.0f ? 2 : 0) | (drew ? 1 : 0);
  if (st.sig != sig) {
    std::string past;
    for (uint32_t i = st.n > 12 ? st.n - 12 : 0; i < st.n; ++i)
      past += fmt::format(" {:.4f}", st.a[i % 12]);
    if (lines++ < 3000)
      RGPU_INFO("[glareobj] {:.0f} ms obj {:08X}: alpha {:.4f}, drew {} (the other answers for {} calls); last alphas{}",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), obj, alpha,
                drew, st.calls - 1, past);
    st.sig = sig;
    st.calls = 0;
  }
}

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): one call of sub_8284CD68 (an effect part's update + draw: it reaches
// the glare's draw sub_82847160 only past its exits), with what each exit test answered inside it. Logged per object
// ([glarerun]) whenever that set of answers changes: the test that flips when the glare vanishes is the switch.
namespace {
struct GlareRun {
  bool active = false;
  int a658 = -1, t108 = -1, ab0a = -1, ab0b = -1, drew = 0;
  int c30e8 = 0, sprites = 0;  // fnxf_xiii (2026-11-16): sub_828430E8 calls / glare sprites inside this call
  // fnxf_xiii (2026-11-16): sub_82843470 (sphere against 8 planes at r3, SoA, 2 x 4 vectors): result, the sphere,
  // the nearest plane and its distance
  int vis = -1, plane = -1;
  float px = 0, py = 0, pz = 0, radius = 0, dmin = 0;
  u32 planes = 0;
  u32 obj = 0;  // fnxf_xiii (2026-11-16): sub_8284CD68's r3, for [glarecull]
  float pw[32] = {};  // fnxf_xiii (2026-11-16): the plane words at the call (they live on the stack)
  float v108 = 0;
};
thread_local GlareRun t_glare_run;
// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): [glareinst] per Swap: the sub_8284CD68 calls for the glare's emitter
// ([obj+4], the one whose calls drew glare sprites) and the answers of their exits, counted: are there no instances
// on the blink frames, or do they take an exit?
std::mutex g_glareinst_mutex;
u32 g_glare_desc = 0;
int g_glareinst_calls = 0;
u32 g_glareinst_planes = 0;
std::string g_glareinst_spheres, g_glareinst_pw;
std::map<std::string, int> g_glareinst;
// fnxf_xiii (2026-11-16): [glaredraw] per Swap: sub_82846138's sprite draws (its sub_8278C398 call, lr 8284625C) per
// r3, as the Xenia patch's [glaredraw] lines (to compare which effect items draw in each frame).
std::mutex g_glaredraw_mutex;
std::map<u32, int> g_glaredraw;
// fnxf_xiii (2026-11-16): [flare] the crystal glare the user means (frame dump fd18 probe: the scene turns white at
// draws 271 / 272, vs BFB74BBE764EB3ED ps F5136E65FC8748A9, slot 0 = 6DADC6AC (256x128) / 6DADD15C (8x8), slot 16 =
// the resolved depth 6DA2E26C): per Swap, how many such draws and vertices; each new caller chain once.
std::mutex g_flare_mutex;
std::map<u32, std::pair<int, int>> g_flare;  // texture -> (draws, vertices)
std::mutex g_glarecam_mutex;  // [glarecam]: see sub_82844ED8's hook
float g_glarecam[16] = {};
u32 g_glarecam_effect = 0;
}  // namespace

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): sub_82785180 draws a model's items (count [this+148], items at
// [this+136], 144 bytes each) through sub_8278C470(item, model). The items that ever drew a glare sprite are kept;
// whenever one of them stops or starts drawing it, its 144 bytes now and when it last drew are logged ([glareitem]):
// the field that differs is the switch.
// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): Renut draws every glare draw the game issues ([glaredrop] never
// fired), so the game skips the glare model whole on some frames. sub_82785180(model) draws a model's items; the
// models that ever drew a glare are kept, and each Present checks whether they were drawn since the last one:
// [glaremodel] lines when that changes, with the caller of sub_82785180 (the code that decides).
namespace {
std::mutex g_glaremodel_mutex;
struct GlareModel {
  bool drawn_this_frame = false;
  int last = -1;
  u32 caller = 0;
  uint64_t frames = 0;
};
std::unordered_map<u32, GlareModel> g_glaremodels;
thread_local u32 t_model = 0, t_model_lr = 0;
}  // namespace

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): the crystal's glare comes from sub_82846138 (sub_8278CC90 called
// from 8284625C): for each of an effect's items it draws only when byte [item+143] == 1, word [item+128] != 0 and
// sub_82845AD8(..., r7 = item + 64, ...) returns true. The items that drew a glare are kept; at each Swap: whether
// sub_82845AD8 ran for them since the last Swap and what it returned, and their +143 / +128, logged when that changes
// ([glarefx]).
namespace {
struct GlareFx {
  int ran = 0, result = -1, drew = 0;
  std::string last;
  uint64_t presents = 0;
};
std::unordered_map<u32, GlareFx> g_glarefx;  // g_glaremodel_mutex
thread_local u32 t_fx_item = 0;
}  // namespace

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): the glare's particles are made by sub_8284CD68 -> sub_828430E8 (it
// fills the list sub_82846BF8 -> sub_82846138 draws), which first asks sub_82842F50 about a counter at [r6+4] (frames
// or time since the effect started?) and gives the effect counter * const as its time. Per Swap for the glare's
// effect: those values and the particles drawn ([glaretime]).

REX_EXTERN(__imp__sub_828430E8);
extern "C" REX_FUNC(sub_828430E8) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on) {
    __imp__sub_828430E8(ctx, base);
    return;
  }
  Call30E8 c;
  c.param = ctx.r6.u32;
  if (const auto *t = GuestPtr<const u8>(ctx.r4.u32 + 4))
    c.type = *t;
  const auto *cp = c.param ? GuestPtr<const be_u32>(c.param + 4) : nullptr;
  c.counter_before = cp ? u32(*cp) : 0;
  ++t_in_30e8;
  if (t_glare_run.active)
    ++t_glare_run.c30e8;
  t_30e8 = c;
  __imp__sub_828430E8(ctx, base);
  --t_in_30e8;
  c.mode = t_30e8.mode;
  c.limit = t_30e8.limit;
  c.counter_after = cp ? u32(*cp) : 0;
  c.result = ctx.r3.u32 & 0xFF;
  t_30e8 = c;
  std::lock_guard lock(g_glaremodel_mutex);
  if (c.param == g_glare_param) {
    Call30E8 &g = g_glare_30e8[c.param];
    const int calls = g.calls + 1;
    g = c;
    g.calls = calls;
  }
}

REX_EXTERN(__imp__sub_82845AD8);
extern "C" REX_FUNC(sub_82845AD8) {
  const u32 item = ctx.r7.u32 - 64;
  __imp__sub_82845AD8(ctx, base);
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on)
    return;
  t_fx_item = item;
  std::lock_guard lock(g_glaremodel_mutex);
  auto it = g_glarefx.find(item);
  if (it != g_glarefx.end()) {
    ++it->second.ran;
    it->second.result = int(ctx.r3.u32 & 0xFF);
  }
}

void GlareFxPresentLocked() {
  static const auto t0 = std::chrono::steady_clock::now();
  static uint64_t presents = 0;
  ++presents;
  {  // fnxf_xiii (2026-11-16): [glaretime]
    static uint32_t time_lines = 0;
    auto it = g_glare_30e8.find(g_glare_param);
    int particles = 0;
    for (auto &[item, fx] : g_glarefx)
      particles += fx.drew;
    if (g_glare_param && time_lines < 6000) {
      ++time_lines;
      if (it == g_glare_30e8.end())
        RGPU_INFO("[glaretime] present {}: param {:08X}: sub_828430E8 not called; glare sprites {}", presents,
                  g_glare_param, particles);
      else
        RGPU_INFO("[glaretime] present {}: param {:08X}: calls {} type {} counter {} -> {} limit {:g} mode {} returned "
                  "{}; glare sprites {}", presents, g_glare_param, it->second.calls, it->second.type,
                  int32_t(it->second.counter_before), int32_t(it->second.counter_after), it->second.limit,
                  it->second.mode, it->second.result, particles);
    }
    g_glare_30e8.clear();
  }
  if (presents % 30 == 0) {  // fnxf_xiii (2026-11-16): [glarecam]
    std::lock_guard clock(g_glarecam_mutex);
    std::string m;
    for (int k = 0; k < 16; ++k)
      m += fmt::format(" {:.5g}", g_glarecam[k]);
    RGPU_INFO("[glarecam] present {}: effect {:08X} view x proj:{}", presents, g_glarecam_effect, m);
  }
  {  // fnxf_xiii (2026-11-16): [flare] per Swap
    static uint32_t flare_lines = 0;
    static std::string last;
    std::lock_guard flock(g_flare_mutex);
    std::string list;
    for (auto &[tex, dv] : g_flare)
      list += fmt::format(" {:08X} x{} ({} vertices)", tex, dv.first, dv.second);
    if (list.empty())
      list = " none";
    if (list != last && flare_lines < 20000) {
      ++flare_lines;
      RGPU_INFO("[flare] present {}:{} (was{})", presents, list, last.empty() ? " -" : last);
      last = list;
    }
    g_flare.clear();
  }
  {  // fnxf_xiii (2026-11-16): [glaredraw]
    static uint32_t draw_lines = 0;
    std::lock_guard dlock(g_glaredraw_mutex);
    if (g_glare_desc && draw_lines < 6000) {
      ++draw_lines;
      std::string list;
      for (auto &[r3, n] : g_glaredraw)
        list += fmt::format(" {:08X} x{}", r3, n);
      RGPU_INFO("[glaredraw] present {}:{}", presents, list);
    }
    g_glaredraw.clear();
  }
  {  // fnxf_xiii (2026-11-16): [glareinst]
    static uint32_t inst_lines = 0;
    std::lock_guard ilock(g_glareinst_mutex);
    if (g_glare_desc && inst_lines < 6000) {
      ++inst_lines;
      std::string list;
      for (auto &[sig, n] : g_glareinst)
        list += fmt::format(" [{} x{}]", sig, n);
      RGPU_INFO("[glareinst] present {}: emitter {:08X}: {} calls{}", presents, g_glare_desc, g_glareinst_calls, list);
      if (!g_glareinst_spheres.empty()) {
        const std::string &pw = g_glareinst_pw;
        RGPU_INFO("[glaresphere] present {}:{}; planes {:08X}:{}", presents, g_glareinst_spheres, g_glareinst_planes,
                  pw);
      }
    }
    g_glareinst.clear();
    g_glareinst_calls = 0;
    g_glareinst_spheres.clear();
  }
  static uint32_t lines = 0;
  for (auto &[item, fx] : g_glarefx) {
    const auto *b143 = GuestPtr<const u8>(item + 143);
    const auto *w128 = GuestPtr<const be_u32>(item + 128);
    const std::string state = fmt::format("ran {} returned {} drew {} [+143] {} [+128] {:08X}", fx.ran, fx.result,
                                          fx.drew, b143 ? int(*b143) : -1, w128 ? u32(*w128) : 0);
    ++fx.presents;
    if (state != fx.last && lines++ < 3000)
      RGPU_INFO("[glarefx] {:.0f} ms present {}: item {:08X}: {} (was: {} for {} presents)",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), presents,
                item, state, fx.last.empty() ? "-" : fx.last, fx.presents - 1);
    if (state != fx.last)
      fx.presents = 0;
    // fnxf_xiii (2026-11-16): the glare is a particle effect; its live particles ("ran") go to 0 between bursts. The
    // words of the item (256 bytes) and of the object at [item+128] (512 bytes) that changed since the last Swap, each
    // frame, with that frame's particle count ([glarewords]): which field leads the bursts.
    {
      static std::unordered_map<u32, std::vector<u32>> prev;
      static uint32_t word_lines = 0;
      const u32 owner = w128 ? u32(*w128) : 0;
      std::vector<u32> now(64 + 128, 0);
      if (const auto *p = GuestPtr<const be_u32>(item))
        for (int i = 0; i < 64; ++i)
          now[i] = u32(p[i]);
      if (const auto *p = owner ? GuestPtr<const be_u32>(owner) : nullptr)
        for (int i = 0; i < 128; ++i)
          now[64 + i] = u32(p[i]);
      auto &old = prev[item];
      if (!old.empty() && word_lines < 4000) {
        std::string diff;
        for (int i = 0; i < 64 + 128; ++i)
          if (now[i] != old[i]) {
            float f;
            std::memcpy(&f, &now[i], 4);
            diff += fmt::format(" {}+{:X}={:08X}({:g})", i < 64 ? "i" : "o", (i < 64 ? i : i - 64) * 4, now[i], f);
          }
        ++word_lines;
        RGPU_INFO("[glarewords] present {} item {:08X} particles {}:{}", presents, item, fx.ran,
                  diff.empty() ? " (no change)" : diff);
      }
      old = std::move(now);
    }
    fx.last = state;
    fx.ran = 0;
    fx.result = -1;
    fx.drew = 0;
  }
}

REX_EXTERN(__imp__sub_82785180);
extern "C" REX_FUNC(sub_82785180) {
  const u32 saved = t_model, saved_lr = t_model_lr;
  t_model = ctx.r3.u32;
  t_model_lr = uint32_t(ctx.lr);
  __imp__sub_82785180(ctx, base);
  t_model = saved;
  t_model_lr = saved_lr;
}

void GlareModelsPresent() {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on)
    return;
  static const auto t0 = std::chrono::steady_clock::now();
  static uint64_t presents = 0;
  ++presents;
  std::lock_guard lock(g_glaremodel_mutex);
  GlareFxPresentLocked();  // fnxf_xiii (2026-11-16)
  for (auto &[model, m] : g_glaremodels) {
    const int now = m.drawn_this_frame ? 1 : 0;
    ++m.frames;
    if (m.last != now)
      RGPU_INFO("[glaremodel] {:.0f} ms present {}: model {:08X} {} (the other state for {} presents; sub_82785180 "
                "called from {:08X})",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), presents,
                model, now ? "DRAWN" : "NOT DRAWN", m.frames - 1, m.caller);
    if (m.last != now)
      m.frames = 0;
    m.last = now;
    m.drawn_this_frame = false;
  }
}

REX_EXTERN(__imp__sub_8278C470);
extern "C" REX_FUNC(sub_8278C470) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on) {
    __imp__sub_8278C470(ctx, base);
    return;
  }
  const u32 item = ctx.r3.u32, model = ctx.r4.u32;
  u32 before[36] = {};
  if (const auto *p = GuestPtr<const be_u32>(item))
    for (int i = 0; i < 36; ++i)
      before[i] = u32(p[i]);
  const int saved = t_item_glare;
  t_item_glare = 0;
  __imp__sub_8278C470(ctx, base);
  const int drew = t_item_glare;
  t_item_glare = saved;
  if (t_model) {  // fnxf_xiii (2026-11-16): the model a glare item belongs to, drawn this frame
    std::lock_guard mlock(g_glaremodel_mutex);
    auto mit = g_glaremodels.find(t_model);
    if (drew && mit == g_glaremodels.end())
      mit = g_glaremodels.emplace(t_model, GlareModel{}).first;
    if (mit != g_glaremodels.end()) {
      mit->second.drawn_this_frame = true;
      mit->second.caller = t_model_lr;
    }
  }
  struct Item {
    int drew = -1;
    u32 last_drawn[36] = {};
    uint64_t calls = 0;
  };
  static std::mutex mutex;
  static std::unordered_map<u32, Item> items;
  static uint32_t lines = 0;
  static const auto t0 = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  auto it = items.find(item);
  if (it == items.end()) {
    if (!drew)
      return;
    it = items.emplace(item, Item{}).first;
  }
  Item &st = it->second;
  ++st.calls;
  const int now = drew ? 1 : 0;
  if (st.drew != now && lines++ < 1500) {
    std::string diff;
    for (int i = 0; i < 36; ++i)
      if (before[i] != st.last_drawn[i])
        diff += fmt::format(" +{:X}: {:08X}->{:08X}", i * 4, st.last_drawn[i], before[i]);
    RGPU_INFO("[glareitem] {:.0f} ms item {:08X} (model {:08X}): {} (after {} calls); words differing from when it last "
              "drew:{}", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), item,
              model, now ? "DRAWS" : "NO DRAW", st.calls - 1, diff.empty() ? " none" : diff);
    st.calls = 0;
  }
  st.drew = now;
  if (now)
    std::memcpy(st.last_drawn, before, sizeof(before));
}

REX_EXTERN(__imp__sub_8278CC90);
extern "C" REX_FUNC(sub_8278CC90) {
  const u32 saved_lr = t_cc90_lr, saved_r3 = t_cc90_r3;
  t_cc90_lr = uint32_t(ctx.lr);
  t_cc90_r3 = ctx.r3.u32;
  __imp__sub_8278CC90(ctx, base);
  t_cc90_lr = saved_lr;
  t_cc90_r3 = saved_r3;
}

REX_EXTERN(__imp__sub_8284CD68);
extern "C" REX_FUNC(sub_8284CD68) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on) {
    __imp__sub_8284CD68(ctx, base);
    return;
  }
  const u32 obj = ctx.r3.u32;
  const auto *pp = GuestPtr<const be_u32>(obj + 12);
  const u32 part = pp ? u32(*pp) : 0;
  const auto *fp = part ? GuestPtr<const u8>(part + 31) : nullptr;
  const int flag = fp ? int((*fp >> 2) & 1) : -1;  // bit 0x04 of [part+31]: set = no draw at all
  const GlareRun saved = t_glare_run;
  t_glare_run = GlareRun{};
  t_glare_run.active = true;
  t_glare_run.obj = obj;
  __imp__sub_8284CD68(ctx, base);
  const GlareRun r = t_glare_run;
  t_glare_run = saved;
  {
    const auto *dp = GuestPtr<const be_u32>(obj + 4);
    const u32 desc = dp ? u32(*dp) : 0;
    std::lock_guard ilock(g_glareinst_mutex);
    if (r.sprites > 0)
      g_glare_desc = desc;
    if (desc && desc == g_glare_desc) {
      ++g_glareinst_calls;
      ++g_glareinst[fmt::format("f{} l{} t{}({:.3g}) c{} vis{} c{} 30e8 {} s{}", flag, r.a658, r.t108, r.v108,
                                r.ab0a, r.vis, r.ab0b, r.c30e8, r.sprites)];
      if (r.vis >= 0) {
        if (g_glareinst_spheres.empty()) {
          g_glareinst_pw.clear();
          for (int k = 0; k < 32; ++k)
            g_glareinst_pw += fmt::format(" {:.4g}", r.pw[k]);
        }
        g_glareinst_planes = r.planes;
        if (g_glareinst_spheres.size() < 1500)
          g_glareinst_spheres += fmt::format(" {}({:.1f},{:.1f},{:.1f} r{:.2f} p{} d{:.2f})", r.vis ? "in" : "OUT", r.px,
                                             r.py, r.pz, r.radius, r.plane, r.dmin);
      }
    }
  }
  const std::string sig = fmt::format("flag {} lookup {} t108 {} cull1 {} cull2 {} drew {}", flag, r.a658, r.t108,
                                      r.ab0a, r.ab0b, r.drew);
  static std::mutex mutex;
  static std::unordered_map<u32, std::pair<std::string, uint64_t>> last;
  static uint32_t lines = 0;
  static const auto t0 = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  auto &e = last[part];  // fnxf_xiii (2026-11-16): by part (this is one shared object for all of them)
  ++e.second;
  if (e.first != sig) {
    if (lines++ < 2000)
      RGPU_INFO("[glarerun] {:.0f} ms obj {:08X} part {:08X}: {} (v108 {:g}; the previous answers for {} calls: {})",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), obj, part,
                sig, r.v108, e.second - 1, e.first.empty() ? "-" : e.first);
    e.first = sig;
    e.second = 0;
  }
}

REX_EXTERN(__imp__sub_82843470);
extern "C" REX_FUNC(sub_82843470) {
  static const bool cull_all = REXCVAR_GET(fnxf_xiii_diag_glare_cull_all);
  if (cull_all && uint32_t(ctx.lr) == 0x8284D0B0u) {
    ctx.r3.u64 = 0;
    return;
  }
  if (!t_glare_run.active) {
    __imp__sub_82843470(ctx, base);
    return;
  }
  const u32 pl = ctx.r3.u32, pos = ctx.r4.u32;
  const float r = float(ctx.f1.f64);
  __imp__sub_82843470(ctx, base);
  GlareRun &g = t_glare_run;
  g.vis = int(ctx.r3.u32 & 1);
  g.planes = pl;
  g.radius = r;
  g.px = GuestFloat(pos);
  g.py = GuestFloat(pos + 4);
  g.pz = GuestFloat(pos + 8);
  g.dmin = 1e30f;
  // fnxf_xiii (2026-11-16): [glarecull], the same lines as the Xenia patch (535107E4 patch "Crystal glare: log the
  // culling sphere") writes through DbgPrint, for a side by side: sphere at the crystal (x 50..150, y 200..250,
  // z -1200..-1050), result, obj, state, radius; [desc+140] ([obj+8] + 140), [obj+44], [obj+48], [[obj+16]]; the
  // matrix [[obj+24]] row 3 and row 0.
  if (g.obj && g.px >= 50 && g.px <= 150 && g.py >= 200 && g.py <= 250 && g.pz >= -1200 && g.pz <= -1050) {
    static uint32_t cull_lines = 0;
    if (cull_lines < 90000) {
      cull_lines += 3;
      const auto W = [](u32 va) {
        const auto *q = GuestPtr<const be_u32>(va);
        return q ? u32(*q) : 0u;
      };
      const u32 o = g.obj, d8 = W(o + 8), lp = W(o + 16), mp = W(o + 24);
      RGPU_INFO("[glarecull] vis {} obj {:08X} state {:08X} pos {:f} {:f} {:f} radius {:f}", g.vis, o, W(o + 12), g.px,
                g.py, g.pz, double(r));
      RGPU_INFO("[glarecull] k140 {:f} k44 {:f} k48 {:f} local {:f} {:f} {:f}", GuestFloat(d8 + 140),
                GuestFloat(o + 44), GuestFloat(o + 48), GuestFloat(lp), GuestFloat(lp + 4), GuestFloat(lp + 8));
      RGPU_INFO("[glarecull] m3 {:f} {:f} {:f} m0 {:f} {:f} {:f}", GuestFloat(mp + 48), GuestFloat(mp + 52),
                GuestFloat(mp + 56), GuestFloat(mp), GuestFloat(mp + 4), GuestFloat(mp + 8));
    }
  }
  for (int k = 0; k < 32; ++k)
    g.pw[k] = GuestFloat((pl & ~0xFu) + k * 4);
  for (int set = 0; set < 2; ++set)
    for (int i = 0; i < 4; ++i) {
      const u32 b = (pl & ~0xFu) + set * 64 + i * 4;
      const float d = GuestFloat(b) * g.px + GuestFloat(b + 16) * g.py + GuestFloat(b + 32) * g.pz + GuestFloat(b + 48);
      if (d < g.dmin) {
        g.dmin = d;
        g.plane = set * 4 + i;
      }
    }
}

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): sub_82844ED8(effect, planes) multiplies its camera's two matrices
// (vfunc 12 x vfunc 16, sub_82226FB0 into its frame +80) and makes the culling planes from them: the product is kept
// for [glarecam].
REX_EXTERN(__imp__sub_8278C398);
extern "C" REX_FUNC(sub_8278C398) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (on && uint32_t(ctx.lr) == 0x8284625Cu) {
    std::lock_guard lock(g_glaredraw_mutex);
    ++g_glaredraw[ctx.r3.u32];
  }
  __imp__sub_8278C398(ctx, base);
}

REX_EXTERN(__imp__sub_82844ED8);
extern "C" REX_FUNC(sub_82844ED8) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  const u32 sp = ctx.r1.u32, effect = ctx.r3.u32;
  __imp__sub_82844ED8(ctx, base);
  if (!on)
    return;
  std::lock_guard lock(g_glarecam_mutex);
  for (int k = 0; k < 16; ++k)
    g_glarecam[k] = GuestFloat(sp - 176 + 80 + 4 * k);
  g_glarecam_effect = effect;
}

REX_EXTERN(__imp__sub_8284A658);
extern "C" REX_FUNC(sub_8284A658) {
  const bool mine = t_glare_run.active;
  const u32 lr = uint32_t(ctx.lr);
  __imp__sub_8284A658(ctx, base);
  if (mine && lr >= 0x8284CD68u && lr <= 0x8284D600u)
    t_glare_run.a658 = int(ctx.r3.u32 & 0xFF);
}

REX_EXTERN(__imp__sub_82847160);
extern "C" REX_FUNC(sub_82847160) {
  if (t_glare_run.active)
    ++t_glare_run.drew;
  if (t_b9d0_drew >= 0)
    ++t_b9d0_drew;
  __imp__sub_82847160(ctx, base);
}

REX_EXTERN(__imp__sub_8284C728);
extern "C" REX_FUNC(sub_8284C728) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  const u32 lr = uint32_t(ctx.lr), sp = ctx.r1.u32, obj = ctx.r31.u32;
  __imp__sub_8284C728(ctx, base);
  if (!on || lr < 0x8284CE50u || lr > 0x8284CE70u)
    return;
  const float v = GuestFloat(sp + 108), threshold = GuestFloat(0x820A8F38u);
  if (t_glare_run.active) {
    t_glare_run.t108 = v > threshold ? 1 : 0;
    t_glare_run.v108 = v;
    return;
  }
  GlareCullNote(obj, 1, v > threshold ? 1 : 0,
                fmt::format("(frame +108 = {:g}, threshold {:g}, returned {:g}, lr {:08X})", v, threshold, ctx.f1.f64, lr));
}

REX_EXTERN(__imp__sub_82829AB0);
extern "C" REX_FUNC(sub_82829AB0) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  const u32 lr = uint32_t(ctx.lr), obj = ctx.r31.u32;
  __imp__sub_82829AB0(ctx, base);
  if (!on || lr < 0x8284CD68u || lr > 0x8284D600u)
    return;
  const u32 r = ctx.r3.u32 & 0xFF;
  if (t_glare_run.active) {
    (lr < 0x8284D100u ? t_glare_run.ab0a : t_glare_run.ab0b) = int(r);
    return;
  }
  GlareCullNote(obj, lr < 0x8284D100u ? 2 : 3, r ? 0 : 1, fmt::format("(returned {}, lr {:08X})", r, lr));
}

namespace {

void GlareCallersNote(u32 device, u32 primitive, u32 count) {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_glare);
  if (!on)
    return;
  const auto *slot = GuestPtr<const be_u32>(device + d3d::kDeviceOffsetTextures);
  const u32 tex = slot ? u32(*slot) : 0;
  if (tex == 0x6DADC6ACu || tex == 0x6DADD15Cu) {  // fnxf_xiii (2026-11-16): [flare]
    std::string chain = fmt::format("{:08X}", t_draw_lr);
    u32 sp = t_draw_sp;
    for (int k = 0; k < 12 && sp; ++k) {
      const auto *p = GuestPtr<const be_u32>(sp);
      if (!p)
        break;
      const u32 next = u32(*p);
      if (next <= sp || next - sp > 0x100000)
        break;
      const auto *lr = GuestPtr<const be_u32>(next - 8);
      if (!lr)
        break;
      chain += fmt::format(" {:08X}", u32(*lr));
      sp = next;
    }
    std::lock_guard flock(g_flare_mutex);
    auto &e = g_flare[tex];
    ++e.first;
    e.second += int(count);
    static std::unordered_map<std::string, int> seen;
    if (seen[fmt::format("{:08X} {}", tex, chain)]++ == 0 && seen.size() <= 40)
      RGPU_INFO("[flare] texture {:08X} prim {} count {}: caller {}", tex, primitive, count, chain);
  }
  if (primitive != 4 || count != 6)
    return;
  const auto *bt = tex ? GuestPtr<const d3d::BaseTexture>(tex) : nullptr;
  if (!bt)
    return;
  const TextureFetch f = TextureFetch::FromHeader(bt->Format);
  u32 w = 0, h = 0, d = 0;
  if (!f.IsTexture())
    return;
  f.Size(w, h, d);
  if (u32(f.Format()) != 20 || w != 256 || h != 256)
    return;
  // fnxf_xiii (2026-11-16): the counts below only for the crystal glare's own texture (other effects' sprites share
  // the size and format: the user's catch); render/draw.cpp learns it from the glare's shader.
  {
    const u32 glare_tex = ::renut::gpu::g_glare_texture_va.load(std::memory_order_relaxed);
    if (!glare_tex || tex != glare_tex)
      return;
  }
  std::string chain = fmt::format("{:08X}", t_draw_lr);
  u32 sp = t_draw_sp;
  for (int k = 0; k < 12 && sp; ++k) {
    const auto *p = GuestPtr<const be_u32>(sp);
    if (!p)
      break;
    const u32 next = u32(*p);
    if (next <= sp || next - sp > 0x100000)
      break;
    const auto *lr = GuestPtr<const be_u32>(next - 8);
    if (!lr)
      break;
    chain += fmt::format(" {:08X}", u32(*lr));
    sp = next;
  }
  static std::mutex mutex;
  static std::unordered_map<std::string, uint64_t> seen;
  if (t_item_glare >= 0)
    ++t_item_glare;
  if (t_fx_item && t_cc90_lr >= 0x82846138u && t_cc90_lr < 0x828462B0u) {  // drawn by sub_82846138
    std::lock_guard flock(g_glaremodel_mutex);
    ++g_glarefx[t_fx_item].drew;
    if (t_glare_run.active)
      ++t_glare_run.sprites;
    if (t_30e8.param)
      g_glare_param = t_30e8.param;  // fnxf_xiii (2026-11-16): the effect whose list held the glare
  }
  std::lock_guard lock(mutex);
  uint64_t &n = seen[chain];
  if (n++ == 0 && seen.size() <= 40)
    RGPU_INFO("[glarecall] texture {:08X} (device {:08X}): caller {}; sub_8278CC90 called from {:08X} (r3 {:08X})",
              tex, device, chain, t_cc90_lr, t_cc90_r3);
}
}  // namespace

// fnxf_xiii (2026-10-26, diagnostic: the profiling build's black screen, 0 draws on the render thread): what the game's
// draw, clear and resolve calls did at the hooks, for the heartbeat (d3d/hooks/device.cpp, log_diagnostics):
// [0] draws seen, [1] of them skipped (not Active: host_ready false or shutting down), [2] recorded into a command
// buffer, [3] clears and resolves seen, [4] of them skipped, [5] recorded.
std::atomic<uint64_t> g_fnxf_xiii_hook_counts[6];

// A float argument also takes the GPR (or stack) slot of its position, which
// the marshaling does not skip: the u32 after each double in the hooks'
// parameters stands in for it.

namespace {

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;
// D3DVIEWPORT9 {0, 0, 0xFFFF, 0xFFFF, 0.0, 1.0} in N&B's data (unk_820018DC):
// the whole surface.
constexpr u32 kFullViewport = 0x820018DC;
// D3DRS_HIZWRITEENABLE's index in the render state table.
constexpr u32 kRenderStateHiZWriteEnable = 97;

// Guest memory Begin*Vertices hands out; the game writes one draw's vertices
// into it before its next D3D call. One per device, as devices record on
// different threads.
constexpr u32 kVerticesScratchSize = 4u << 20;
std::mutex g_vertices_scratch_mutex;  // devices call from their own threads
std::unordered_map<u32, u32> g_vertices_scratch;

u32 VerticesScratch(u32 device) {
  std::lock_guard lock(g_vertices_scratch_mutex);
  u32 &scratch = g_vertices_scratch[device];
  if (!scratch)
    scratch = records::AllocPhysicalBlock(kVerticesScratchSize);
  return scratch;
}

bool Active() {
  auto &s = State();
  return s.host_ready && !s.shutting_down.load(std::memory_order_acquire);
}

// fnxf_xiii (2026-10-26): g_fnxf_xiii_hook_counts. kind 0: a draw, 3: a clear or a resolve.
void NoteHook(u32 device, int kind) {
  g_fnxf_xiii_hook_counts[kind].fetch_add(1, std::memory_order_relaxed);
  if (!Active())
    g_fnxf_xiii_hook_counts[kind + 1].fetch_add(1, std::memory_order_relaxed);
  else if (IsRecordingCommandBuffer(device))
    g_fnxf_xiii_hook_counts[kind + 2].fetch_add(1, std::memory_order_relaxed);
}

void StoreU32(u32 va, u32 value) {
  if (auto *p = GuestPtr<be_u32>(va))
    *p = value;
}

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

bool ReadRect(u32 va, ClearRect &rect) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  rect = {i32(u32(p[0])), i32(u32(p[1])), i32(u32(p[2])), i32(u32(p[3]))};
  return true;
}

bool ReadVector4(u32 va, float out[4]) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  for (int i = 0; i < 4; ++i) {
    const u32 bits = p[i];
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

// void D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex,
//                             UINT VertexCount) [0x8222C7A0]
void D3DDevice_DrawVertices_hook(u32 device, u32 primitive, u32 start_vertex, u32 vertex_count) {
  GlareCallersNote(device, primitive, vertex_count);  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare)
  NoteHook(device, 0);  // fnxf_xiii (2026-10-26, diagnostic)
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.start_vertex = start_vertex;
  call.count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex,
//                                    UINT StartIndex, UINT IndexCount) [0x826567C8]
void D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                        u32 start_index, u32 index_count) {
  GlareCallersNote(device, primitive, index_count);  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare)
  NoteHook(device, 0);  // fnxf_xiii (2026-10-26, diagnostic)
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedTessellatedVertices(D3DDevice*, D3DTESSPRIMITIVETYPE,
//     INT BaseVertexIndex, UINT StartIndex, UINT IndexCount) [0x82230838]
// Replaced like the other draws: the original's state flush binds the shader
// from its microcode, and bugchecks (0xF5) on a tessellation shader without
// the tessellation state the original SetVertexShader keeps. The draw is
// tessellated on the host (DrawCall::tessellation).
void D3DDevice_DrawIndexedTessellatedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                                   u32 start_index, u32 index_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.tessellation = true;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               const void *pVertexStreamZeroData, UINT Stride) [0x8222E520]
void D3DDevice_DrawVerticesUP_hook(u32 device, u32 primitive, u32 vertex_count, u32 data,
                                   u32 stride) {
  GlareCallersNote(device, primitive, vertex_count);  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare)
  NoteHook(device, 0);  // fnxf_xiii (2026-10-26, diagnostic)
  if (!Active() || !data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(data);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_count * stride, 0);
  else
    QueueDraw(device, call, vertex_count * stride, 0);
}

// void D3DDevice_DrawIndexedVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT MinVertexIndex,
//     UINT NumVertices, UINT IndexCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
//     const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) [0x82656728]
void D3DDevice_DrawIndexedVerticesUP_hook(u32 device, u32 primitive, u32 min_vertex,
                                          u32 num_vertices, u32 index_count, u32 index_data,
                                          u32 index_format, u32 vertex_data, u32 stride) {
  GlareCallersNote(device, primitive, index_count);  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare)
  NoteHook(device, 0);  // fnxf_xiii (2026-10-26, diagnostic)
  if (!Active() || !index_data || !vertex_data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.count = index_count;
  call.up_indices = GuestPtr<const u8>(index_data);
  call.up_indices_32 = (index_format & 4) != 0;  // D3DFMT_INDEX32
  call.up_vertices = GuestPtr<const u8>(vertex_data);
  call.up_stride = stride;
  call.up_vertex_count = min_vertex + num_vertices;
  const u32 vertex_bytes = call.up_vertex_count * stride;
  const u32 index_bytes = index_count * (call.up_indices_32 ? 4 : 2);
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_bytes, index_bytes);
  else
    QueueDraw(device, call, vertex_bytes, index_bytes);
}

// void *D3DDevice_BeginVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               UINT VertexStreamZeroStride) [0x8222CC50]
// Callers write the vertices, then end inline by restoring m_pRing from the
// device, so the draw's state is captured now and its vertices copied at the
// device's next hook (FlushDeferredVertices).
u32 D3DDevice_BeginVertices_hook(u32 device, u32 primitive, u32 vertex_count, u32 stride) {
  NoteHook(device, 0);  // fnxf_xiii (2026-10-26, diagnostic)
  const u64 bytes = u64(vertex_count) * stride;
  if (!bytes || bytes > kVerticesScratchSize)
    return 0;
  // The inline end: m_pRing = [device + kDeviceOffsetVerticesRingEnd].
  StoreU32(device + d3d::kDeviceOffsetVerticesRingEnd,
           LoadU32(device + u32(offsetof(d3d::Device, m_pRing))));

  const u32 scratch = VerticesScratch(device);
  if (!scratch)
    return 0;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(scratch);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  // Either way the device's previous Begin*Vertices data leaves the scratch
  // before the game writes this draw's over it.
  if (IsRecordingCommandBuffer(device))
    RecordDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else if (Active())
    QueueDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else
    FlushDeferredVertices(device);
  return scratch;
}

// A clear: the renderer's, recorded or queued, then N&B's device work.
// whole_target: BeginTiling's, of one tile's worth of EDRAM, which on the
// host is the whole tiled target.
void Clear(u32 device, u32 flags, const ClearRect *rect, const float color[4], float z,
           u32 stencil, bool whole_target) {
  NoteHook(device, 3);  // fnxf_xiii (2026-10-26, diagnostic)
  const ClearRect *host_rect = whole_target ? nullptr : rect;
  if (IsRecordingCommandBuffer(device))
    RecordClear(device, flags, host_rect, color, z, stencil);
  else if (Active())
    QueueClear(device, flags, host_rect, color, z, stencil);
  // fnxf_xiii: the device work is the game's own D3DDevice_ClearF, run by the hook.
}

// A resolve: the renderer's copy (and clear), then N&B's device work.
void Resolve(u32 device, u32 flags, const ClearRect *rect, u32 dest_texture, i32 dest_x,
             i32 dest_y, u32 dest_level, u32 dest_slice, u32 color_va, float clear_z,
             u32 clear_stencil, u32 parameters) {
  NoteHook(device, 3);  // fnxf_xiii (2026-10-26, diagnostic)
  float color[4] = {0, 0, 0, 0};
  const bool has_color = color_va && ReadVector4(color_va, color);
  if (IsRecordingCommandBuffer(device)) {
    RecordResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                  has_color ? color : nullptr, clear_z, clear_stencil);
  } else if (Active()) {
    QueueResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                 has_color ? color : nullptr, clear_z, clear_stencil);
  }
  // fnxf_xiii: the device work is the game's own D3DDevice_Resolve, run by the hook.
  (void)parameters;
}

REX_EXTERN(__imp__sub_826DFA20);  // fnxf_xiii: the game's D3DDevice_ClearF
REX_EXTERN(__imp__sub_826DD968);  // fnxf_xiii: the game's D3DDevice_Resolve
REX_EXTERN(__imp__sub_826D27A0);  // fnxf_xiii: the game's D3DDevice_BeginTiling
REX_EXTERN(__imp__sub_826D2B10);  // fnxf_xiii: the game's D3DDevice_EndTiling

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect,
//                       const D3DVECTOR4 *pColor, float Z, DWORD Stencil) [0x822811A0]
// Z takes r7's slot, so Stencil arrives in r8.
void D3DDevice_ClearF_hook(u32 device, u32 flags, u32 rect_va, u32 color_va, double z,
                           u32 /*z_slot*/, u32 stencil) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};  // unk_82001160 without one
  if (color_va)
    ReadVector4(color_va, color);
  Clear(device, flags, has_rect ? &rect : nullptr, color, float(z), stencil, false);
  // fnxf_xiii: the game's ClearF for the device work (its packets go to the ring sink).
  rex::ppc::GuestToHostFunction<void>(__imp__sub_826DFA20, device, flags, rect_va, color_va, z, 0u, stencil);
}

// void D3DDevice_Clear(D3DDevice*, DWORD Count, const D3DRECT *pRects, DWORD Flags,
//     D3DCOLOR Color, float Z, DWORD Stencil) [0x822812C8]: ClearF with the
// color as a vector (XMLoadColor: each byte as 1 + b / 2^23, scaled by
// 2^23 / 255 and offset), per rect. Z takes r8's slot.
void D3DDevice_Clear_hook(u32 device, u32 count, u32 rects_va, u32 flags, u32 color_argb,
                          double z, u32 /*z_slot*/, u32 stencil) {
  const float scale = std::bit_cast<float>(0x47008081u);  // aG, unk_82001150
  auto channel = [scale](u32 byte) {
    return std::fmaf(std::bit_cast<float>(0x3F800000u | (byte & 0xFF)), scale, -scale);
  };
  const float color[4] = {channel(color_argb >> 16), channel(color_argb >> 8),
                          channel(color_argb), channel(color_argb >> 24)};
  if (!rects_va) {
    Clear(device, flags, nullptr, color, float(z), stencil, false);
    return;
  }
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (ReadRect(rects_va + i * 16, rect))
      Clear(device, flags, &rect, color, float(z), stencil, false);
  }
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect,
//     D3DBaseTexture *pDestTexture, const D3DPOINT *pDestPoint, UINT DestLevel,
//     UINT DestSliceOrFace, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil, const D3DRESOLVE_PARAMETERS *pParameters) [0x8222D6C8]
// ClearZ is in f1 and takes the first stack slot; ClearStencil and pParameters
// are in the two after it.
void D3DDevice_Resolve_hook(u32 device, u32 flags, u32 rect_va, u32 dest_texture, u32 point_va,
                            u32 dest_level, u32 dest_slice, u32 color_va, double clear_z,
                            u32 /*z_slot*/, u32 clear_stencil, u32 parameters) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = GuestPtr<const be_u32>(point_va)) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  Resolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level,
          dest_slice, color_va, float(clear_z), clear_stencil, parameters);
  // fnxf_xiii: the game's Resolve for the device work.
  rex::ppc::GuestToHostFunction<void>(__imp__sub_826DD968, device, flags, rect_va, dest_texture, point_va,
                                      dest_level, dest_slice, color_va, clear_z, 0u, clear_stencil, parameters);
}

// void D3DDevice_BeginTiling(D3DDevice*, DWORD Flags, DWORD Count,
//     const D3DRECT *pTileRects, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil) [0x822788A0]
// The surfaces bound from here to EndTiling are backed by targets covering
// every tile (SetTilingExtentLocked). N&B's BeginTiling clears with Z 0.0 and
// stencil 0 (its ClearZ and ClearStencil go unused), unless the device records
// a one-pass Z pass (Flags3 0x20); its ring segment for the tiles is left out.
void D3DDevice_BeginTiling_hook(u32 device, u32 flags, u32 count, u32 rects_va, u32 color_va,
                                double clear_z, u32 /*z_slot*/, u32 clear_stencil) {
  // fnxf_xiii: the surfaces bound from here to EndTiling are backed by targets covering every tile; the game's
  // BeginTiling then does the device work (its clears come through the hooked ClearF, its tile segment goes to the
  // ring sink and the draws are recorded once, on the full-size targets).
  std::vector<ClearRect> tiles;
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (!ReadRect(rects_va + i * 16, rect))
      break;
    tiles.push_back(rect);
  }
  if (!IsRecordingCommandBuffer(device)) {
    FlushDeferredVertices(device);
    u32 width = 0, height = 0;
    for (const ClearRect &rect : tiles) {
      width = std::max(width, u32(rect.x2));
      height = std::max(height, u32(rect.y2));
    }
    render_thread::Queue([tiles, width, height] {
      const ::renut::gpu::rtw::Scope rtw_misc(::renut::gpu::rtw::kMiscItems);  // fnxf_xiii (2026-11-05, [rtwork])
      SetTilingExtentLocked(width, height);
      SetTilesLocked(tiles.data(), u32(tiles.size()));
    });
  }
  rex::ppc::GuestToHostFunction<void>(__imp__sub_826D27A0, device, flags, count, rects_va, color_va, clear_z, 0u,
                                      clear_stencil);
}

// HRESULT D3DDevice_EndTiling(D3DDevice*, DWORD ResolveFlags, const D3DRECT *pResolveRects,
//     D3DBaseTexture *pDestTexture, const D3DVECTOR4 *pClearColor, float ClearZ, DWORD ClearStencil,
//     const D3DRESOLVE_PARAMETERS *pParameters)
// fnxf_xiii: the game's own EndTiling (its per-tile resolves come through the hooked Resolve, each reading its rect
// from the full-size target), then the host's tiling state is dropped.
u32 D3DDevice_EndTiling_hook(u32 device, u32 resolve_flags, u32 rects_va, u32 dest_texture, u32 color_va,
                             double clear_z, u32 /*z_slot*/, u32 clear_stencil, u32 parameters) {
  const u32 result = rex::ppc::GuestToHostFunction<u32>(__imp__sub_826D2B10, device, resolve_flags, rects_va,
                                                         dest_texture, color_va, clear_z, 0u, clear_stencil,
                                                         parameters);
  if (!IsRecordingCommandBuffer(device)) {
    render_thread::Queue([] {
      const ::renut::gpu::rtw::Scope rtw_misc(::renut::gpu::rtw::kMiscItems);  // fnxf_xiii (2026-11-05, [rtwork])
      SetTilingExtentLocked(0, 0);
      SetTilesLocked(nullptr, 0);
    });
  }
  return result;
}

#if 0  // fnxf_xiii: N&B-only (export, Present and shader setters run as the game's own code)
// void D3DDevice_BeginExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x82269618]. The renderer finds a draw's memexport streams in its shaders.
void D3DDevice_BeginExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::BeginExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_EndExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x822693B0]: the resource is busy until the current fence, which completes
// once the render thread has written the stream's results.
void D3DDevice_EndExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::EndExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_Present(D3DDevice*) [0x82654778]: the automatic back buffer
// resolved into the automatic front buffer, then Swap. N&B creates its device
// without an automatic front buffer, so its callers (XUI, the threaded movie
// player) have nothing to resolve to.
void D3DDevice_Present_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  FlushDeferredVertices(device);
  float viewport[6];
  for (u32 i = 0; i < 6; ++i)
    viewport[i] = d.F(0x3168 + i * 4);
  i32 scissor[4];
  for (u32 i = 0; i < 4; ++i)
    scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
  native::SetRenderTarget(d, 0, d.W(native::off::AutoBackBuffer));
  // SynchronizeToPresentationInterval (0x8228A748): its swap callback and wait
  // go into the ring; the swap is pending.
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
  const u32 front_buffer = d.W(native::off::AutoFrontBuffer);
  if (front_buffer)
    Resolve(device, 0, nullptr, front_buffer, 0, 0, 0, 0, 0, 1.0f, 0, 0);
  native::KickOff(device);
  native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                      viewport[5]);
  native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  native::Swap(device, front_buffer, device + 0x3548);
}

// void D3DDevice_SetVertexShader(D3DDevice*, D3DVertexShader*) [0x8222A0A8]
void D3DDevice_SetVertexShader_hook(u32 device, u32 shader) {
  native::SetVertexShader(native::DeviceAt(device), shader);
}

// void D3DDevice_SetPixelShader(D3DDevice*, D3DPixelShader*) [0x82226730]
void D3DDevice_SetPixelShader_hook(u32 device, u32 shader) {
  native::SetPixelShader(native::DeviceAt(device), shader);
}
#endif


} // namespace

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_glare): FNXF_XIII_RENUT_HOOK, the guest caller (lr, stack) noted first.
#define FNXF_XIII_GLARE_DRAW_HOOK(subroutine, function)                                   \
  REX_EXTERN(__imp__##subroutine);                                                     \
  extern "C" REX_FUNC(subroutine) {                                                    \
    if (::fnxf_xiii::renut_gpu::Active()) {                                                \
      t_draw_lr = uint32_t(ctx.lr);                                                    \
      t_draw_sp = ctx.r1.u32;                                                          \
      FNXF_XIII_RENUT_HOOK_TIMER(#function);                                               \
      ::rex::ppc::HostToGuestFunction<function>(ctx, base);                            \
      return;                                                                          \
    }                                                                                  \
    __imp__##subroutine(ctx, base);                                                    \
  }
FNXF_XIII_GLARE_DRAW_HOOK(sub_829949D0, D3DDevice_DrawVertices_hook)  /* N&B sub_8222C7A0 */;  // D3DDevice_DrawVertices
FNXF_XIII_GLARE_DRAW_HOOK(sub_82994DC0, D3DDevice_DrawIndexedVertices_hook)  /* N&B sub_826567C8 */;  // D3DDevice_DrawIndexedVertices
// fnxf_xiii: not hooked (no match in the game): FNXF_XIII_RENUT_HOOK_SHARED(sub_82230838, D3DDevice_DrawIndexedTessellatedVertices_hook);  // D3DDevice_DrawIndexedTessellatedVertices
FNXF_XIII_GLARE_DRAW_HOOK(sub_829943D0, D3DDevice_DrawVerticesUP_hook)  /* N&B sub_8222E520 */;  // D3DDevice_DrawVerticesUP
FNXF_XIII_GLARE_DRAW_HOOK(sub_82994930, D3DDevice_DrawIndexedVerticesUP_hook)  /* N&B sub_82656728 */;  // D3DDevice_DrawIndexedVerticesUP
FNXF_XIII_RENUT_HOOK(sub_82993F38, D3DDevice_BeginVertices_hook)  /* N&B sub_8222CC50 */;  // D3DDevice_BeginVertices
FNXF_XIII_RENUT_HOOK(sub_826DFA20, D3DDevice_ClearF_hook)  /* N&B sub_822811A0 */;  // D3DDevice_ClearF
// fnxf_xiii: not hooked (the game's own code runs): D3DDevice_Clear_hook
FNXF_XIII_RENUT_HOOK(sub_826DD968, D3DDevice_Resolve_hook)  /* N&B sub_8222D6C8 */;  // D3DDevice_Resolve
FNXF_XIII_RENUT_HOOK(sub_826D27A0, D3DDevice_BeginTiling_hook);  // D3DDevice_BeginTiling (fnxf_xiii: manual match)
FNXF_XIII_RENUT_HOOK(sub_826D2B10, D3DDevice_EndTiling_hook)  /* N&B sub_82212328 */;  // D3DDevice_EndTiling
// fnxf_xiii: not hooked (no match in the game): FNXF_XIII_RENUT_HOOK_SHARED(sub_82269618, D3DDevice_BeginExport_hook);  // D3DDevice_BeginExport
// fnxf_xiii: not hooked (the game's own code runs): D3DDevice_EndExport_hook
// fnxf_xiii: not hooked (no match in the game): FNXF_XIII_RENUT_HOOK_SHARED(sub_82654778, D3DDevice_Present_hook);  // D3DDevice_Present
// fnxf_xiii: not hooked (the game's own code runs): D3DDevice_SetVertexShader_hook
// fnxf_xiii: not hooked (the game's own code runs): D3DDevice_SetPixelShader_hook
