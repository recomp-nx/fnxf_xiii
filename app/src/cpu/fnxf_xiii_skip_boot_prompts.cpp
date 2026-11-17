// fnxf_xiii (2026-10-25, fnxf_xiii_skip_boot_prompts): the two prompts after the publisher logo answered at once.
//
// Both are in the game's start-up flow, sub_82200F80:
//   "Press any button to continue." (layout "userSelect", text $xsignio_00): the game reads one keystroke from any
//     controller, XInputGetKeystroke(XUSER_INDEX_ANY = 255, XINPUT_FLAG_GAMEPAD, &keystroke) through sub_82984A18:
//     once (return address 0x82201200), then after Sleep(500) (sub_82984938, return 0x82201208) again (0x82201218),
//     then every millisecond (0x82201238) until one comes; the keystroke's UserIndex becomes the player
//     (frame object +2332). Here those calls get an A press of player 1 (user 0) at once, and the 500 ms sleep is
//     skipped. The prompt itself is never opened (2026-10-25: its closing played the confirm sound): the message is
//     made (sub_824CC608) and later destroyed (sub_824C98E0) as before, but its opening (sub_824C76D8, return
//     0x822011E4; sub_824C74D0, 0x822011F0), its closing (sub_824C98B0, 0x8220124C) and the waits for its closing
//     animation (sub_824C6570, 0x82201254 and 0x8220126C: 0 = done) are skipped.
//   "Select the storage device you would like to use for saving and loading data." (dialog "chooseDeviceFirst", text
//     $trc_mes_05 with an OK button $xdiskchg_ok): sub_8245FFE0(name, text), return address 0x822013B0; 0 or more is
//     OK (the flow then checks the device, which the SDK's device selector has already given). Here it returns 0
//     without showing the dialog.
// Only these call sites change: the same functions called from anywhere else run as before.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <initializer_list>
#include <cstdint>

REXCVAR_DEFINE_BOOL(fnxf_xiii_skip_boot_prompts, true, "Game/Enhancements",
                    "Skip \"Press any button to continue\" (player 1 is taken) and the storage device notice after "
                    "the publisher logo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, fnxf_xiii_hide_device_select);  // fnxf_xiii_switch_texts.cpp
REX_EXTERN(__imp__sub_82984A18);  // XInputGetKeystroke wrapper
REX_EXTERN(__imp__sub_82984938);  // Sleep wrapper
REX_EXTERN(__imp__sub_8245FFE0);  // the game's message dialog (name, text)
REX_EXTERN(__imp__sub_824C76D8);  // a message: open
REX_EXTERN(__imp__sub_824C74D0);  // a message: its mode
REX_EXTERN(__imp__sub_824C98B0);  // a message: close
REX_EXTERN(__imp__sub_824C6570);  // a message: still animating

namespace {
bool On() {
  static const bool on = REXCVAR_GET(fnxf_xiii_skip_boot_prompts);
  return on;
}
void Store16(uint8_t *base, uint32_t ea, uint16_t v) { *reinterpret_cast<uint16_t *>(base + ea) = __builtin_bswap16(v); }
}  // namespace

extern "C" REX_FUNC(sub_82984A18) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (On() && (lr == 0x82201200u || lr == 0x82201218u || lr == 0x82201238u) && ctx.r5.u32) {
    const uint32_t keystroke = ctx.r5.u32;  // XINPUT_KEYSTROKE: VirtualKey, Unicode, Flags (u16), UserIndex, HidCode
    Store16(base, keystroke + 0, 0x5800);   // VK_PAD_A
    Store16(base, keystroke + 2, 0);
    Store16(base, keystroke + 4, 0x0001);   // XINPUT_KEYSTROKE_KEYDOWN
    base[keystroke + 6] = 0;                // player 1
    base[keystroke + 7] = 0;
    ctx.r3.u64 = 0;                         // ERROR_SUCCESS
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_INFO("[boot] \"Press any button\" answered: player 1 (fnxf_xiii_skip_boot_prompts)");
    return;
  }
  __imp__sub_82984A18(ctx, base);
}

extern "C" REX_FUNC(sub_82984938) {
  if (On() && uint32_t(ctx.lr) == 0x82201208u) {  // the 500 ms before "Press any button" reads again
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82984938(ctx, base);
}

extern "C" REX_FUNC(sub_8245FFE0) {
  if (On() && uint32_t(ctx.lr) == 0x822013B0u) {  // "chooseDeviceFirst"
    ctx.r3.u64 = 0;                                // OK
    REXLOG_INFO("[boot] storage device notice skipped (fnxf_xiii_skip_boot_prompts)");
    return;
  }
  // fnxf_xiii (2026-10-25, fnxf_xiii_hide_device_select): the same notice in the device change flow, shown when Y in the
  // save menu clears the device (fnxf_xiii_switch_texts.cpp): answered OK without being shown.
  static const bool hide_select = REXCVAR_GET(fnxf_xiii_hide_device_select);
  if (hide_select && uint32_t(ctx.lr) == 0x8246170Cu) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8245FFE0(ctx, base);
}

// fnxf_xiii (2026-10-25): "Press any button" never opened, so neither its window nor its closing sound (see the top).
// fnxf_xiii (2026-10-31): the same for the "insert disc N" message of a disc swap (sub_82440110, after XamSwapDisc:
// made at 0x8244030C, opened 0x8244031C, its mode 0x82440328, the wait for the swap's event 0x82440334, closed
// 0x82440340, the waits for its closing animation 0x82440348 and 0x82440360, destroyed 0x82440370). Every disc is
// the game folder, so the swap is done at once (fnxf_xiii_disc_swap.cpp); the message was gone before it showed, but
// its sound played. Always (not a prompt the cvar is for): the message is made and destroyed as before, never opened.
#define FNXF_XIII_SKIP_AT(fn, disc_sites, ...)                                         \
  extern "C" REX_FUNC(fn) {                                                           \
    const uint32_t lr = uint32_t(ctx.lr);                                             \
    for (const uint32_t site : {__VA_ARGS__})                                         \
      if (On() && lr == site) {                                                       \
        ctx.r3.u64 = 0;                                                               \
        return;                                                                       \
      }                                                                               \
    for (const uint32_t site : std::initializer_list<uint32_t> disc_sites)            \
      if (lr == site) {                                                               \
        ctx.r3.u64 = 0;                                                               \
        return;                                                                       \
      }                                                                               \
    __imp__##fn(ctx, base);                                                           \
  }
#define FNXF_XIII_DISC(...) {__VA_ARGS__}
FNXF_XIII_SKIP_AT(sub_824C76D8, FNXF_XIII_DISC(0x8244031Cu), 0x822011E4u)
FNXF_XIII_SKIP_AT(sub_824C74D0, FNXF_XIII_DISC(0x82440328u), 0x822011F0u)
FNXF_XIII_SKIP_AT(sub_824C98B0, FNXF_XIII_DISC(0x82440340u), 0x8220124Cu)
FNXF_XIII_SKIP_AT(sub_824C6570, FNXF_XIII_DISC(0x82440348u, 0x82440360u), 0x82201254u, 0x8220126Cu)
#undef FNXF_XIII_DISC
#undef FNXF_XIII_SKIP_AT

// fnxf_xiii (2026-10-25, fnxf_xiii_diag_boot_sounds): which code plays the sound still heard where "Press any button"
// was. The game's UI sounds go through GuiManager::playSE (sub_824D7228: this, type 0-102) and the layouts' own sound
// events through LayoutProxy::playSe (sub_825FD140: this, a 16-byte key, called through a vtable). The first 64 of
// each are logged with their callers (the return address and two more from the guest stack's back chain).
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_boot_sounds, false, "Game/Diagnostics",
                    "Log the game's UI sound calls (type or key, and their callers), the first 64 of each kind")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824D7228);  // GuiManager::playSE(this, type)
REX_EXTERN(__imp__sub_824CC608);  // a message: made
REX_EXTERN(__imp__sub_825FD140);  // LayoutProxy::playSe(this, key)

namespace {
bool SoundDiagOn() {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_boot_sounds);
  return on;
}
uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
// The return addresses above the current call: the caller's caller saved its LR at -8 of the frame its back chain
// points to (the generated prologues' "stw r12,-8(r1)").
void Callers(const uint8_t *base, uint32_t sp, uint32_t out[2]) {
  out[0] = out[1] = 0;
  uint32_t frame = sp;
  for (int i = 0; i < 2; ++i) {
    if (!frame || frame >= 0xF0000000u)
      return;
    frame = Load32(base, frame);
    if (!frame || frame >= 0xF0000000u)
      return;
    out[i] = Load32(base, frame - 8);
  }
}
}  // namespace

// fnxf_xiii (2026-10-25): the sound still heard (fnxf_xiii_043.log): GuiManager::playSE type 47 from 82574A10 < 824CBA74 <
// 824CC648, i.e. made by the message's own creation (sub_824CC608, called for "Press any button" from 0x822011D4).
// That creation runs with UI sounds muted on its thread; the message is still made (and later destroyed) as before.
thread_local bool t_mute_ui_sounds = false;

extern "C" REX_FUNC(sub_824CC608) {
  // and the disc swap's "insert disc N" message, made at 0x8244030C (always: see FNXF_XIII_SKIP_AT)
  if ((On() && uint32_t(ctx.lr) == 0x822011D4u) || uint32_t(ctx.lr) == 0x8244030Cu) {
    t_mute_ui_sounds = true;
    __imp__sub_824CC608(ctx, base);
    t_mute_ui_sounds = false;
    return;
  }
  __imp__sub_824CC608(ctx, base);
}

extern "C" REX_FUNC(sub_824D7228) {
  if (t_mute_ui_sounds)
    return;  // GuiManager::playSE returns nothing
  if (SoundDiagOn()) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 64) {
      uint32_t up[2];
      Callers(base, ctx.r1.u32, up);
      REXLOG_INFO("[sound] GuiManager::playSE type {} from {:08X} < {:08X} < {:08X}", ctx.r4.u32, uint32_t(ctx.lr),
                  up[0], up[1]);
    }
  }
  __imp__sub_824D7228(ctx, base);
}

extern "C" REX_FUNC(sub_825FD140) {
  if (SoundDiagOn()) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 64) {
      uint32_t up[2];
      Callers(base, ctx.r1.u32, up);
      char key[17] = {};
      if (ctx.r4.u32)
        for (int i = 0; i < 16; ++i) {
          const char c = char(base[ctx.r4.u32 + i]);
          key[i] = (c >= 32 && c < 127) ? c : (c ? '.' : '\0');
          if (!c)
            break;
        }
      const uint32_t k = ctx.r4.u32;
      REXLOG_INFO("[sound] LayoutProxy::playSe \"{}\" ({:08X} {:08X} {:08X} {:08X}) from {:08X} < {:08X} < {:08X}", key,
                  k ? Load32(base, k) : 0u, k ? Load32(base, k + 4) : 0u, k ? Load32(base, k + 8) : 0u,
                  k ? Load32(base, k + 12) : 0u, uint32_t(ctx.lr), up[0], up[1]);
    }
  }
  __imp__sub_825FD140(ctx, base);
}
