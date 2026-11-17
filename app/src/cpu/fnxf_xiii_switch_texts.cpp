// fnxf_xiii (2026-10-25, fnxf_xiii_switch_device_name): the save menu names the storage device "Nintendo Switch".
//
// The save screen's device label (top right) is the game's own text, chosen by the device's type in sub_82587420:
// type 1 (hard drive) "$xsave_20", type 2 (memory unit) "$xsave_22" (the key strings at 0x820987D8 / 0x820987E4), looked
// up into a text object by sub_824E0438(text object, key, 0). The SDK's device is the hard drive, so the label is
// $xsave_20: "Xbox 360 Hard Drive" / "Xbox 360 Festplatte" / "Disque dur Xbox 360" / "Disco duro Xbox 360" / "Disco
// rigido Xbox 360" (txtres_us / gr / fr / sp / it.ztr, the line ending "\0\x01"). After that lookup the text the object
// points to (+4) is written over with "Nintendo Switch" and the same ending: every language's original is at least as
// long (19-21 characters against 15), so it is changed in place. The lookup is told apart by the key's address: only
// that site passes it.
//
// fnxf_xiii (2026-10-25, fnxf_xiii_hide_device_select): no "(Y) Select device" in the save menu, and Y does nothing there.
// The menu's button line is built by sub_82587420 from three texts appended into one buffer by sub_824E8270(buffer,
// key), which puts a separator before every text but the first: "$xsave_16" ("(Y) Select device", key at 0x820987CC,
// return address 0x82588CC0), then "$pause_04" ("(A) OK", only when +268 of the menu is above 0), then
// "$m_res_fl_wmb06" ("(B) Close"). The first append is skipped, so the line starts with OK or Close.
// The Y button: the device change flow, sub_82461588, is reached through the storage manager's vtables (sub_82461AE0:
// r6 1, r7 0; AF0: r6 0, r7 0; B00: r7 1, which returns 0 at once) from the game's job runner (sub_8243C1E8), so the
// save menu does not appear among its callers. fnxf_xiii_046.log (fnxf_xiii_diag_device_select): one call at start-up (r7 1
// -> 0) and the Y press in the save menu (this 6EC5A8F0, r4 BA540980, r5 0x493B0, r6 1, r7 0 -> -1, from the job
// runner); nothing when the menu opens. Load Game runs the same flow with the same arguments (returning from it at
// once, with 0 or with -1, sent Load Game straight back to the main menu), so the flow runs as it is. What Y adds is
// inside it: with no device set for it ([r24] 0, which Y clears) the flow shows "Select the storage device..."
// ($trc_mes_05 with OK: sub_8245FFE0, return address 0x8246170C), waits 500 ms (sub_8245FB70, 0x82461724), then opens
// the device selector, which the SDK answers at once with the one device. The notice is answered OK without being
// shown (fnxf_xiii_skip_boot_prompts.cpp, which owns the sub_8245FFE0 hook) and the wait skipped: Y gives the same device
// back at once and the menu stays.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(fnxf_xiii_switch_device_name, true, "Game/Enhancements",
                    "The save menu's storage device label reads \"Nintendo Switch\" instead of \"Xbox 360 Hard Drive\"")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824E0438);  // the game's text lookup: (text object, key, flags)

namespace {
constexpr uint32_t kKeyHardDrive = 0x820987D8u;  // "$xsave_20"
constexpr char kName[] = "Nintendo Switch";
}  // namespace

extern "C" REX_FUNC(sub_824E0438) {
  const uint32_t object = ctx.r3.u32, key = ctx.r4.u32;
  __imp__sub_824E0438(ctx, base);
  if (key != kKeyHardDrive || !object)
    return;
  static const bool on = REXCVAR_GET(fnxf_xiii_switch_device_name);
  if (!on)
    return;
  const uint32_t text = __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + object + 4));
  if (!text)
    return;
  char *p = reinterpret_cast<char *>(base + text);
  const size_t length = std::strlen(p);
  constexpr size_t kLength = sizeof(kName) - 1;
  if (length < kLength || std::memcmp(p, kName, kLength) == 0)
    return;  // too short to hold it (no language's is), or already changed (a shared text)
  const bool marker = p[length + 1] == '\x01';  // the line's ending after its NUL
  std::memcpy(p, kName, kLength);
  p[kLength] = '\0';
  if (marker)
    p[kLength + 1] = '\x01';
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("[texts] save menu device label: \"Nintendo Switch\" (fnxf_xiii_switch_device_name)");
}

REXCVAR_DEFINE_BOOL(fnxf_xiii_hide_device_select, true, "Game/Enhancements",
                    "The save menu shows no \"(Y) Select device\" and ignores Y there (one storage device on the Switch)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824E8270);  // append a text (by key) to a button line buffer
REX_EXTERN(__imp__sub_82461588);  // the storage device change flow
REX_EXTERN(__imp__sub_8245FB70);  // a wait (ms, in 10 ms sleeps)

namespace {
bool HideDeviceSelect() {
  static const bool on = REXCVAR_GET(fnxf_xiii_hide_device_select);
  return on;
}
}  // namespace

extern "C" REX_FUNC(sub_8245FB70) {
  if (HideDeviceSelect() && uint32_t(ctx.lr) == 0x82461724u)
    return;  // the 500 ms after the (skipped) "Select the storage device" notice
  __imp__sub_8245FB70(ctx, base);
}

extern "C" REX_FUNC(sub_824E8270) {
  if (HideDeviceSelect() && uint32_t(ctx.lr) == 0x82588CC0u && ctx.r4.u32 == 0x820987CCu)
    return;  // "$xsave_16": (Y) Select device
  __imp__sub_824E8270(ctx, base);
}

REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_device_select, false, "Game/Diagnostics",
                    "Log the save menu's storage device change calls (arguments, callers, result), the first 32")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" REX_FUNC(sub_82461588) {
  static const bool diag = REXCVAR_GET(fnxf_xiii_diag_device_select);
  if (!diag) {
    __imp__sub_82461588(ctx, base);
    return;
  }
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  // Two callers up from the guest stack's back chain (each saved its LR at -8 of the frame above).
  uint32_t up[2] = {0, 0}, frame = ctx.r1.u32;
  for (int i = 0; i < 2 && frame && frame < 0xF0000000u; ++i) {
    frame = __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + frame));
    if (!frame || frame >= 0xF0000000u)
      break;
    up[i] = __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + frame - 8));
  }
  __imp__sub_82461588(ctx, base);
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 32)
    REXLOG_INFO("[device] change flow ({:08X}, {:08X}, {:08X}, r6 {}, r7 {}) -> {} from {:08X} < {:08X} < {:08X}", r3, r4,
                r5, r6, r7, ctx.r3.u32, lr, up[0], up[1]);
}
