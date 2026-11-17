// Native replacement for the game's spin barrier (vfx-qix-effect workers).
//
// sub_8274AFC0(obj, n) arms the barrier: the first arriver (CAS obj+48 0 -> 1)
// stores the participant count n at obj+32 and returns 1, the others wait for
// obj+32 to become non-zero and return 0.
// sub_8274B028(obj, n) is the barrier itself: every participant decrements
// obj+32; the last one (old value 1) adds n-1 to obj+36, waits for it to reach
// 0 and clears obj+48; the others wait for obj+32 to reach 0, then decrement
// obj+36.
//
// On the 360 this busy-waits on 6 hardware threads. On the Switch the four
// workers share 3 cores, so the spinners burn whole time slices waiting for a
// worker that is not even scheduled (~250% CPU in the main menu, 14 fps), and
// every lwarx/stwcx. pair also takes the global critical-region mutex.
// Here the same counters are updated with host atomics (the lwarx/stwcx. loops
// elsewhere still work against them: they are a CAS on the same word) and the
// waits block in the kernel's address arbiter after a short spin.

#include <rex/hook.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#ifdef __SWITCH__
#include <switch.h>
#endif

REX_EXTERN(__imp__sub_8274AFC0);
REX_EXTERN(__imp__sub_8274B028);

#if defined(__aarch64__)
// cpu/fnxf_xiii_sync_diag.cpp (fnxf_xiii_diag_sync_waits, 2026-10-25): the waits below, timed when it is on.
bool FnxfXiiiSyncDiagOn();
void FnxfXiiiSyncDiagSpin(uint32_t lr, uint64_t ns);
#define FNXF_XIII_SPIN_DIAG 1
#endif

namespace {

using Word = std::atomic<uint32_t>;

#if FNXF_XIII_SPIN_DIAG
struct SpinTimer {
  uint32_t lr;
  int64_t t0;
  explicit SpinTimer(uint32_t l) : lr(l), t0(FnxfXiiiSyncDiagOn() ? Now() : 0) {}
  ~SpinTimer() {
    if (t0)
      FnxfXiiiSyncDiagSpin(lr, uint64_t(Now() - t0));
  }
  static int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};
#define SPIN_TIMED(ctx) SpinTimer spin_timer_(uint32_t((ctx).lr))
#else
#define SPIN_TIMED(ctx)
#endif

inline Word& At(uint8_t* base, uint32_t ea) {
  uint32_t off = 0;
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  if (ea >= 0xE0000000u) off = 0x1000u;
#endif
  return *reinterpret_cast<Word*>(base + ea + off);
}

inline uint32_t Load(Word& w) { return __builtin_bswap32(w.load(std::memory_order_acquire)); }

// Adds d to the big-endian word; returns the old value (host order).
inline uint32_t FetchAdd(Word& w, uint32_t d) {
  uint32_t raw = w.load(std::memory_order_relaxed);
  while (!w.compare_exchange_weak(raw, __builtin_bswap32(__builtin_bswap32(raw) + d),
                                  std::memory_order_seq_cst, std::memory_order_relaxed)) {
  }
  return __builtin_bswap32(raw);
}

inline void Wake(Word& w) {
#ifdef __SWITCH__
  svcSignalToAddress(&w, SignalType_Signal, 0, -1);
#else
  (void)w;
#endif
}

inline void Relax() {
#if defined(__aarch64__)
  asm volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

// Waits while keep(value) holds.
template <typename Keep>
inline void WaitWhile(Word& w, Keep keep) {
  for (int i = 0; i < 64; ++i) {
    if (!keep(Load(w))) return;
    Relax();
  }
  for (;;) {
    const uint32_t raw = w.load(std::memory_order_acquire);
    if (!keep(__builtin_bswap32(raw))) return;
#ifdef __SWITCH__
    // Returns at once if the word already changed, so no wake is lost. The
    // 1 ms timeout only matters if guest code we do not replace writes it.
    svcWaitForAddress(&w, ArbitrationType_WaitIfEqual, static_cast<int32_t>(raw), 1000000);
#else
    std::this_thread::yield();
#endif
  }
}

}  // namespace

// Arm: returns 1 to the first arriver.
REX_HOOK_RAW(sub_8274AFC0) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t n = ctx.r4.u32;
  SPIN_TIMED(ctx);
  Word& armed = At(base, obj + 48);
  Word& count = At(base, obj + 32);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  uint32_t expected = 0;
  if (armed.compare_exchange_strong(expected, __builtin_bswap32(1u), std::memory_order_seq_cst)) {
    count.store(__builtin_bswap32(n), std::memory_order_seq_cst);
    Wake(count);
    ctx.r3.u64 = 1;
  } else {
    WaitWhile(count, [](uint32_t v) { return v == 0; });
    ctx.r3.u64 = 0;
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

// Barrier.
REX_HOOK_RAW(sub_8274B028) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t n = ctx.r4.u32;
  SPIN_TIMED(ctx);
  Word& count = At(base, obj + 32);
  Word& leave = At(base, obj + 36);
  Word& armed = At(base, obj + 48);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (FetchAdd(count, 0xFFFFFFFFu) == 1) {
    // Last arriver: release the others, wait until they have all left.
    Wake(count);
    FetchAdd(leave, n - 1);
    WaitWhile(leave, [](uint32_t v) { return v != 0; });
    armed.store(0, std::memory_order_seq_cst);
  } else {
    WaitWhile(count, [](uint32_t v) { return v != 0; });
    if (FetchAdd(leave, 0xFFFFFFFFu) == 1) Wake(leave);
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
