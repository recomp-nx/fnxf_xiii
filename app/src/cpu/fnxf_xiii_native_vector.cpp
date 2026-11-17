// fnxf_xiii (2026-11-05): sub_823AD1A8 (std::vector<unsigned char>::_M_fill_insert) natively, a new buffer through the
// game's own allocator (fnxf_xiii_native_vector.h). fnxf_xiii_native_vector off: the game's own function, as before.
#include "fnxf_xiii_native_vector.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(fnxf_xiii_native_vector, true, "fnxf_xiii",
                    "Diagnostics: the game's byte-vector fill-insert (sub_823AD1A8, the load's hot loop) done with "
                    "memset/memmove on guest memory instead of byte by byte; off: the game's own function")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_823AD1A8);
extern "C" REX_FUNC(sub_823AD1A8) {
  if (REXCVAR_GET(fnxf_xiii_native_vector) &&
      fnxf_xiii::native_vector::CallFillInsert(ctx, base, [](uint32_t a) { return rex::runtime::ResolveIndirectFunction(a); }))
    return;
  __imp__sub_823AD1A8(ctx, base);
}
