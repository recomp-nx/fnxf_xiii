// fnxf_xiii (2026-11-09, fnxf_xiii_fix_load_fade): the frames shown after a loaded save's chapter screen, drawn from an
// unplaced camera (the field's ground black below the horizon, the party missing) while the fade-in runs.
//
// A save is loaded at its save point, inside the save point's event (zone state STATE_EVENT_ON_FIELD_IDLE, cinema
// com_ucon_ev), with the chapter screen over it. When the screen closes, two threads finish the event: the script ends
// it (STATE_EVENT_ON_FIELD_UNINIT, then its "ModeChange" callback and STATE_FIELD_IDLE) and the main thread creates the
// field's camera (createCinema <DistanceCinema>) and draws the field, while the fade-in (StartFade 4, 1 s from black)
// runs. In their order of the clean loads (the game's log, 2026-11-09) the camera is created after the mode change.
// Created first (in about a third of the loads here), the frames until the mode change are drawn from the new camera
// before it is placed: one frame most of the time (near black, early in the fade), 16 to 52 frames when the mode change
// came 0.5-1.7 s later (seen in two loads, the half-black field fading in). This shows black instead of those frames:
// after a save's load (main state STATE_LOAD_END), a DistanceCinema created during the event's end holds the present at
// black until the zone reaches STATE_FIELD_IDLE (3 s at most); the game runs as before.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(fnxf_xiii_fix_load_fade, true, "Game/Fixes",
                    "After a loaded save's chapter screen, the frames the game draws from the field's camera before "
                    "the save point event has ended (the ground black, for up to 1.7 s of the fade-in) are shown "
                    "black")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82463578);  // the main state's change (object, ?, new, old), logged "main state change"
REX_EXTERN(__imp__sub_82448EC8);  // the zone state's change (zone, ?, new, old, ...), logged "zone state change"
REX_EXTERN(__imp__sub_82618DC8);  // createCinema (manager, ?, name, succession, ...)
REX_EXTERN(__imp__sub_8243BB18);  // a name's text into a buffer (name, buffer): createCinema's log uses it

namespace {
constexpr uint32_t kMainStateNames = 0x82A5E370;  // the main states' names, by index (sub_82463578's log)
constexpr uint32_t kZoneStateNames = 0x82A5B500;  // the zone states' names (sub_82448EC8's log)

uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
// A state's name from its table (empty for an index out of reason).
const char *StateName(const uint8_t *base, uint32_t table, uint32_t index) {
  if (index > 256)
    return "";
  const uint32_t p = Load32(base, table + index * 4);
  return p ? reinterpret_cast<const char *>(base + p) : "";
}

std::mutex g_mutex;
bool g_armed = false;  // a save's load until its event's end
bool g_event_end = false;  // the zone in STATE_EVENT_ON_FIELD_IDLE or STATE_EVENT_ON_FIELD_UNINIT
std::atomic<bool> g_hold{false};
std::chrono::steady_clock::time_point g_hold_since;
std::atomic<uint32_t> g_held{0};  // frames shown black
}  // namespace

extern "C" REX_FUNC(sub_82463578) {
  static const bool on = REXCVAR_GET(fnxf_xiii_fix_load_fade);
  if (on && std::strcmp(StateName(base, kMainStateNames, ctx.r5.u32), "STATE_LOAD_END") == 0) {
    std::lock_guard lock(g_mutex);
    g_armed = true;
    g_hold = false;
  }
  __imp__sub_82463578(ctx, base);
}

extern "C" REX_FUNC(sub_82448EC8) {
  static const bool on = REXCVAR_GET(fnxf_xiii_fix_load_fade);
  if (on) {
    const char *to = StateName(base, kZoneStateNames, ctx.r5.u32);
    const char *from = StateName(base, kZoneStateNames, ctx.r6.u32);
    std::lock_guard lock(g_mutex);
    g_event_end = std::strcmp(to, "STATE_EVENT_ON_FIELD_IDLE") == 0 || std::strcmp(to, "STATE_EVENT_ON_FIELD_UNINIT") == 0;
    if (g_armed && std::strcmp(to, "STATE_FIELD_IDLE") == 0 && std::strcmp(from, "STATE_EVENT_ON_FIELD_UNINIT") == 0) {
      g_armed = false;
      if (g_hold.exchange(false))
        REXLOG_INFO("[load fade] the save point event ended: {} frame(s) shown black", g_held.exchange(0));
    }
  }
  __imp__sub_82448EC8(ctx, base);
}

extern "C" REX_FUNC(sub_82618DC8) {
  static const bool on = REXCVAR_GET(fnxf_xiii_fix_load_fade);
  bool check = false;
  if (on) {
    std::lock_guard lock(g_mutex);
    check = g_armed && g_event_end && !g_hold;
  }
  if (check) {
    // The name's text through the game's own conversion, into a buffer below the stack pointer (this call's frame
    // and the conversion's own below it); the registers as they were afterwards.
    const PPCContext saved = ctx;
    const uint32_t buffer = (ctx.r1.u32 - 0x200) & ~15u;
    std::memset(base + buffer, 0, 64);
    ctx.r3.u64 = saved.r5.u64;
    ctx.r4.u64 = buffer;
    ctx.r1.u64 = buffer - 0x200;
    __imp__sub_8243BB18(ctx, base);
    ctx = saved;
    char name[65] = {};
    std::memcpy(name, base + buffer, 64);
    if (std::strcmp(name, "DistanceCinema") == 0) {
      std::lock_guard lock(g_mutex);
      if (g_armed && g_event_end) {
        g_hold = true;
        g_hold_since = std::chrono::steady_clock::now();
        g_held = 0;
        REXLOG_INFO("[load fade] the field's camera created before the save point event's end: shown black until it");
      }
    }
  }
  __imp__sub_82618DC8(ctx, base);
}

// The Swap's question (renut_engine/gpu/d3d/hooks/device.cpp): this frame shown black.
bool FnxfXiiiLoadFadeHold() {
  if (!g_hold.load(std::memory_order_relaxed))
    return false;
  std::lock_guard lock(g_mutex);
  if (!g_hold)
    return false;
  if (std::chrono::steady_clock::now() - g_hold_since > std::chrono::seconds(3)) {
    g_hold = false;
    g_armed = false;
    REXLOG_INFO("[load fade] 3 s without the save point event's end: shown again ({} frame(s) black)",
                g_held.exchange(0));
    return false;
  }
  ++g_held;
  return true;
}
