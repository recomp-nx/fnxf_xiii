// fnxf_xiii (2026-10-23): the game's matrix multiply (sub_82226FB0, 615 call sites) and bounding box transform
// (sub_822271E0, 54) as native code (fnxf_xiii_native_vmath.h). In the v84 profile the first was the second hottest guest
// function. Proven on ARM, two ways: (1) statically, the Switch objects of the originals and of these compute every
// stored value as the same expression (16 and 28 float operations, the same operand order in each); (2) under
// qemu-aarch64, 300,000 trials each, every output byte, r3..r11 and the FPCR the same over ordinary and special inputs
// (denormals, infinities, NaNs with payloads, several NaNs together, overflow, cancellation, outputs over the inputs,
// unaligned pointers), flush to zero on and off before. The test caught each deliberately wrong variant (one operand
// order flipped, multiply-then-add for the fused one).
// fnxf_xiii_native_vmath off: the game's own functions, as before. AArch64 (the Switch) only: elsewhere the originals.
#include "fnxf_xiii_native_vmath.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(fnxf_xiii_native_vmath, true, "fnxf_xiii",
                    "The game's matrix multiply and bounding box transform (its two hottest vector functions) as "
                    "native code instead of the recompiled ones")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__aarch64__)

REX_EXTERN(__imp__sub_82226FB0);

namespace {
std::atomic<bool> g_logged{false};
inline void LogOnce() {
  if (!g_logged.load(std::memory_order_relaxed) && !g_logged.exchange(true))
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[vmath] native: matrix multiply, box transform (fnxf_xiii_native_vmath)");
}
}  // namespace
REX_EXTERN(__imp__sub_822271E0);

extern "C" REX_FUNC(sub_82226FB0) {
  if (!REXCVAR_GET(fnxf_xiii_native_vmath)) {
    __imp__sub_82226FB0(ctx, base);
    return;
  }
  LogOnce();
  fnxf_xiii::native_vmath::MatrixMultiply(ctx, base);
}

extern "C" REX_FUNC(sub_822271E0) {
  if (!REXCVAR_GET(fnxf_xiii_native_vmath)) {
    __imp__sub_822271E0(ctx, base);
    return;
  }
  LogOnce();
  fnxf_xiii::native_vmath::TransformBox(ctx, base);
}

#endif  // __aarch64__
