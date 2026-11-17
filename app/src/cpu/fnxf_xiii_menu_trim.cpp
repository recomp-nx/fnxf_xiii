// fnxf_xiii (2026-11-11): the title menu without "Extras" (DODATKI in the Polish pack, $title_special).
//
// The title menu (sub_825908D0) makes its four entries (+0xCC New Game, +0xD0 Load, +0xD4 Settings, +0xD8 Extras) and
// hides Extras (its vfunc +0x18 with 1, as it hides Load without save data) when sub_821F2718 says the extras are
// not unlocked yet (a game with no progress: [system+0x90C] 0, or its state 4). sub_821F2718 is called from there
// only: it answers "not unlocked", so the menu is always the one of a new game: New Game, Load, Settings.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>

REXCVAR_DEFINE_BOOL(fnxf_xiii_title_menu_trim, true, "Game/Enhancements",
                    "The title menu shows no Extras (as before any progress)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_821F2718);

extern "C" REX_FUNC(sub_821F2718) {
  static const bool trim = REXCVAR_GET(fnxf_xiii_title_menu_trim);
  if (!trim) {
    __imp__sub_821F2718(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("[menus] title menu: Extras hidden (fnxf_xiii_title_menu_trim)");
}
