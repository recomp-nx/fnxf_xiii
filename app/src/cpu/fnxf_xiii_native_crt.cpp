// fnxf_xiii (2026-10-23): the game's C runtime memory and string functions as the host C library's (fnxf_xiii_native_crt.h).
//
// Found and proven on the PC (the harness in the notes: each recompiled function against each C library function over
// 400 random inputs, r3 and a whole 1 MB arena compared; then these overrides themselves against the originals, with
// overlapping ranges and every byte value):
//   sub_829715A0  memcpy   (926 call sites)       sub_82971500  memset   (620)
//   sub_82971A28  memcpy   (57)                   sub_82972C60  memmove  (45)
//   sub_82984B88  memcpy   (4)                    sub_82984C38  memcpy   (3)
//   sub_82984CE8  memcpy   (6)                    sub_829708B8  strncpy  (39)
//   sub_829700B0  strchr   (21)                   sub_82970CC0  strstr   (47)
// sub_82984CE8 is the 360's large-copy dispatcher (XMemCpy's pattern): under 256 bytes sub_829715A0, else by the
// destination page's protection (MmQueryAddressProtect: write-combined) sub_82984B88 or sub_82984C38, all three copies;
// on the Switch guest memory is plain RAM, the choice was only the 360's speed. strchr is the game's, not C's: it
// compares the whole 32-bit c (fnxf_xiii_native_crt.h CallStrchr). The PC check of the overrides: 3000 trials each, with
// overlaps (forward copies go to the original), exactly adjacent ranges, n = 0, pointers above 0x80000000, any c.
// Every call site was checked: none reads a volatile register (r0, r4-r12, ctr, cr0/1/5-7) after the call, which the
// native versions leave as they were (the PowerPC ABI's rule, here checked rather than assumed). Guest memory on the
// Switch is one plain block (REX_PHYS_HOST_OFFSET 0); its only unmapped pages are the GPU and XMA register windows,
// which the game reaches with no copies (0 MMIO faults in the logs), and first-touch commits retry any instruction.
// 2026-11-06: and the game's memmove sub_82729978 (its pieces as long as the overlap's distance), hooked in
// cpu/fnxf_xiii_mover_diag.cpp (CallMemmove), behind this same cvar.
// fnxf_xiii_native_crt off: the game's own functions, as before.
#include "fnxf_xiii_native_crt.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(fnxf_xiii_native_crt, true, "fnxf_xiii",
                    "The game's memcpy / memset / memmove / strncpy / strchr / strstr done by the host C library "
                    "instead of the game's own recompiled ones")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
std::atomic<bool> g_logged{false};
void LogOnce() {
  if (!g_logged.load(std::memory_order_relaxed) && !g_logged.exchange(true))
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[crt] native: memcpy x5, memmove, memset, strncpy, strchr, strstr (fnxf_xiii_native_crt)");
}
}  // namespace

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_load_watch): told of each native memcpy / memmove / memset of 4 KB or more
// (kind 'c' / 'm' / 'f', destination, source or fill value, bytes, the caller's return address). Null: nothing.
namespace fnxf_xiii::native_crt {
std::atomic<void (*)(char kind, uint32_t d, uint32_t s, uint32_t n, uint32_t lr)> g_big_write_observer{nullptr};
}

#define FNXF_XIII_NATIVE_CRT(addr, call)                                     \
  REX_EXTERN(__imp__sub_##addr);                                          \
  extern "C" REX_FUNC(sub_##addr) {                                       \
    if (REXCVAR_GET(fnxf_xiii_native_crt) && fnxf_xiii::native_crt::call(ctx, base)) { \
      LogOnce();                                                          \
      if (fnxf_xiii::native_crt::Kind(#call) != '?' && ctx.r5.u32 >= 4096) { \
        if (auto *o = fnxf_xiii::native_crt::g_big_write_observer.load(std::memory_order_relaxed)) \
          o(fnxf_xiii::native_crt::Kind(#call), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr)); \
      }                                                                   \
      return;                                                             \
    }                                                                     \
    __imp__sub_##addr(ctx, base);                                         \
  }

FNXF_XIII_NATIVE_CRT(829715A0, CallMemcpy)
FNXF_XIII_NATIVE_CRT(82971A28, CallMemcpy)
FNXF_XIII_NATIVE_CRT(82984B88, CallMemcpy)
FNXF_XIII_NATIVE_CRT(82984C38, CallMemcpy)
FNXF_XIII_NATIVE_CRT(82984CE8, CallMemcpy)
FNXF_XIII_NATIVE_CRT(82972C60, CallMemmove)
FNXF_XIII_NATIVE_CRT(82971500, CallMemset)
FNXF_XIII_NATIVE_CRT(829708B8, CallStrncpy)
FNXF_XIII_NATIVE_CRT(829700B0, CallStrchr)
FNXF_XIII_NATIVE_CRT(82970CC0, CallStrstr)
