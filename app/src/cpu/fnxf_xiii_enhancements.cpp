// fnxf_xiii: the enhancement switches reNut's renderer reads (fnxf_xiii's framework, cut down: this port has none of
// fnxf_xiii's CPU enhancements yet, only the renderer's own keys, which each have their own cvar).
#include "cpu/fnxf_xiii_enhancements.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread/mutex.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-25): sdk/src/system/xobject.cpp

// fnxf_xiii (2026-10-22): the SDK's Switch profiler reads this flag by name (switch_perf.cpp), and only a title that
// defines it can set it from fnxf_xiii.toml; without it, only the stacks_profile.flag file in log/ turned sampling on.
REXCVAR_DEFINE_BOOL(fnxf_xiii_show_compile_notice, true, "fnxf_xiii",
                    "Switch: \"Compiling shaders...\" in small type at the top left while a shader the game waits for "
                    "is compiled, and for a moment after");
// fnxf_xiii (2026-10-23): the achievement popup (renut_engine/gpu/backend/achievement_popup.cpp).
REXCVAR_DEFINE_BOOL(fnxf_xiii_achievement_popup, true, "fnxf_xiii",
                    "Switch: \"Achievement unlocked\" with the achievement's name at the top left of the picture when "
                    "the game unlocks one (in the game's language: English, German, French, Spanish or Italian)");
REXCVAR_DEFINE_DOUBLE(fnxf_xiii_achievement_popup_seconds, 5.0, "fnxf_xiii",
                      "Switch: how long the achievement popup stays up, in seconds")
    .range(1.0, 30.0);
REXCVAR_DEFINE_BOOL(fnxf_xiii_achievement_popup_test, false, "fnxf_xiii",
                    "Switch: a test popup shortly after start (the first achievement the game has, or a sample in the "
                    "game's language), to check the popup and its letters. Nothing is unlocked");
REXCVAR_DEFINE_INT32(fnxf_xiii_guest_atomic_lock, 0, "Game/Enhancements",
                     "The game's interrupt-disabled regions around its atomic lwarx/stwcx. pairs (mfmsr / mtmsrd): "
                     "0 = no lock (the pair is a host compare-and-swap, atomic on its own; the vfx-qix-effect workers "
                     "spent most of their time queued on the lock), 1 = a lock of their own, 2 = the SDK's global "
                     "critical region, shared with the kernel's object table, audio and file system (as before, Xenia's), "
                     "3 = not each other, but the global critical region's holder keeps them out (2's exclusion "
                     "without its cost: 0 and 1 made the loads after a skipped cutscene take a minute)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(switch_profile_stacks, false, "fnxf_xiii",
                    "Switch: the profiler samples every thread's stack each millisecond (log/rex_profile.log "
                    "shows where each thread spends its time or waits). Slows the game; for diagnostics");
REXCVAR_DEFINE_BOOL(switch_profile_report, false, "fnxf_xiii",
                    "Switch: every 10 s, each thread's CPU time, the faults and the system call counters into "
                    "log/rex_profile.log (on with switch_profile_stacks too). The overlay's FPS works without it");
REXCVAR_DEFINE_BOOL(fnxf_xiii_cpu_enhancements, true, "Game/Enhancements",
                    "Master switch of the CPU enhancements (each also has its own key). false = all of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_noncpu_enhancements, true, "Game/Enhancements",
                    "Master switch of the non-CPU enhancements: GPU, driver and storage side (each also has its own "
                    "key). false = all of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_cpu_verify, false, "Game/Enhancements",
                    "Verify configuration: every enhancement that can checks itself against the original path on "
                    "every use. Slower; for testing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fnxf_xiii {

bool EnhancementActive(EnhancementGroup group, bool own_key) {
  return own_key && (group == EnhancementGroup::kCpu ? REXCVAR_GET(fnxf_xiii_cpu_enhancements)
                                                     : REXCVAR_GET(fnxf_xiii_noncpu_enhancements));
}

void ApplyMasterSwitches() {}

void LogEnhancements() {
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[enhancements] cpu {} non-cpu {} verify {}", REXCVAR_GET(fnxf_xiii_cpu_enhancements),
              REXCVAR_GET(fnxf_xiii_noncpu_enhancements), REXCVAR_GET(fnxf_xiii_cpu_verify));
  // fnxf_xiii (2026-10-22): before the module launches (OnPostSetup), so no region is open when it changes.
  rex::thread::guest_atomic_lock_mode = REXCVAR_GET(fnxf_xiii_guest_atomic_lock);
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[enhancements] guest atomic regions: {}",
              rex::thread::guest_atomic_lock_mode == 0   ? "no lock (fnxf_xiii_guest_atomic_lock 0)"
              : rex::thread::guest_atomic_lock_mode == 1 ? "own lock (fnxf_xiii_guest_atomic_lock 1)"
              : rex::thread::guest_atomic_lock_mode == 2 ? "global critical region (fnxf_xiii_guest_atomic_lock 2)"
                                                         : "kept out by the global critical region's holder (fnxf_xiii_guest_atomic_lock 3)");
}

void CodegenChecksInit() {}
void FormsSelfCheck() {}

}  // namespace fnxf_xiii
