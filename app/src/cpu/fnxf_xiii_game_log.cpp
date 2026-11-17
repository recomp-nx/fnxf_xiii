// fnxf_xiii (2026-11-01, fnxf_xiii_diag_game_log): the game's own debug messages in the game log, for diagnostics.
//
// The game keeps its development messages in the release build: sub_824A4E00(format, ...) is its printf-style log
// (232 call sites; the error reporter sub_824A4F00(file, line, message), 1225 call sites, formats "file(line): message"
// and passes it on to it). It does nothing unless bit 0x40000000 of the word at 0x82BF0B48 is set; when set it formats
// the message into a 1 KB buffer (after a prefix "%X:%X> ": a counter and another number) and hands it to sub_82457D70, its output (one
// caller: this function), which on the console went to a debug channel. Examples the game has: "[SceneDataSheet] push
// command for MoveFloor <%s - %s>", "[Loader] load Navimap id = %08x ...", "Navimap layer id not found".
// With the cvar the bit is set for each call (and put back after it), and the output writes the message to this log
// ("[game] ...") instead of the game's channel. Off (the default): both functions run as before, a cvar read each.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_game_log, false, "Game/Diagnostics",
                    "Diagnostic: the game's own debug messages (its development log: scene commands, loader, navigation "
                    "map, asserts) into the game log as [game] lines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824A4E00);  // the game's log: (format, ...)
REX_EXTERN(__imp__sub_82457D70);  // its output: (message)
REX_EXTERN(__imp__sub_82462538);  // fnxf_xiii (2026-11-09): the field's "deny counter" (object, 1 up / 0 down)

namespace {
constexpr uint32_t kLogFlags = 0x82BF0B48;    // bit 0x40000000: the log is on
constexpr uint32_t kLogOn = 0x40000000u;
thread_local int t_inside = 0;
thread_local uint32_t t_caller = 0;  // fnxf_xiii (2026-11-09): the log call's return address (who logged)

uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
void Store32(uint8_t *base, uint32_t ea, uint32_t v) {
  *reinterpret_cast<uint32_t *>(base + ea) = __builtin_bswap32(v);
}
}  // namespace

extern "C" REX_FUNC(sub_824A4E00) {
  if (!REXCVAR_GET(fnxf_xiii_diag_game_log)) {
    __imp__sub_824A4E00(ctx, base);
    return;
  }
  const uint32_t flags = Load32(base, kLogFlags);
  Store32(base, kLogFlags, flags | kLogOn);
  if (!t_inside)
    t_caller = uint32_t(ctx.lr);
  ++t_inside;
  __imp__sub_824A4E00(ctx, base);
  --t_inside;
  Store32(base, kLogFlags, (Load32(base, kLogFlags) & ~kLogOn) | (flags & kLogOn));
}

extern "C" REX_FUNC(sub_82457D70) {
  if (!REXCVAR_GET(fnxf_xiii_diag_game_log) || !t_inside) {
    __imp__sub_82457D70(ctx, base);
    return;
  }
  const uint32_t msg = ctx.r3.u32;
  if (!msg)
    return;
  const char *s = reinterpret_cast<const char *>(base + msg);
  std::string text(s, strnlen(s, 1024));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  // The same message many frames running: logged once, then how many times it came.
  static std::mutex m;
  static std::string last;
  static uint32_t repeats = 0;
  std::lock_guard lock(m);
  // The game's prefix ("%X:%X> ": a counter and another number) differs every time: compared without it.
  const size_t cut = text.find("> ");
  const std::string body = cut == std::string::npos ? text : text.substr(cut + 2);
  if (body == last) {
    if (++repeats == 1000)
      REXLOG_INFO("[game] (the last message 1000 more times; no more counted)");
    return;
  }
  if (repeats >= 1 && repeats < 1000)
    REXLOG_INFO("[game] (the last message {} more times)", repeats);
  repeats = 0;
  last = body;
  REXLOG_INFO("[game] {} (from {:08X})", text, t_caller);
}

// fnxf_xiii (2026-11-09, the load glitch, fnxf_xiii_diag_deny): who moves the field's "deny counter"
// (sub_82462538(object, r4): object+564 counted down when r4 != 0 (sub_8243DB78), up when 0 (sub_8243DB50), then
// logged by the game): its caller, the direction and the value before, as [deny] lines (cheap: no game log needed).
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_deny, false, "Game/Diagnostics",
                    "Diagnostic: the field's deny counter changes ([deny] lines: caller, direction, value)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fnxf_xiii_diag_deny_delay_ms, 0, "Game/Diagnostics",
                     "Diagnostic (the load glitch): the event's end count-up of the deny counter (from 82442B0C) waits "
                     "this many ms first (widens the gap after the fade's count-down)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" REX_FUNC(sub_82462538) {
  static const int32_t delay_ms = REXCVAR_GET(fnxf_xiii_diag_deny_delay_ms);
  if (delay_ms > 0 && !ctx.r4.u32 && uint32_t(ctx.lr) == 0x82442B0Cu)
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_deny) || REXCVAR_GET(fnxf_xiii_diag_game_log);
  if (on)
    REXLOG_INFO("[deny] {} by {:08X} (object {:08X}, was {})", ctx.r4.u32 ? "down" : "up", uint32_t(ctx.lr),
                ctx.r3.u32, Load32(base, ctx.r3.u32 + 564));
  __imp__sub_82462538(ctx, base);
}
