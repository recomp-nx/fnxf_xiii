/**
 * @file    gpu/backend/render_thread.cpp
 */
#include "renut_engine/gpu/render/draw.h"  // fnxf_xiii (2026-11-05): rtw ([rtwork])
#include "renut_engine/gpu/backend/render_thread.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <string>
#include <rex/frame_latency.h>
#include <vector>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/fnxf_xiii_renut_tuning.h"
#if defined(__SWITCH__)
#include "profiler/switch_cpu_profiler.h"  // fnxf_xiii (2026-10-24): MarkSlowFrameCompanion
#endif

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-25): sdk/src/system/xobject.cpp

// fnxf_xiii (2026-10-20, from sa2-nx): see g_done_waiters and Run.
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_done_notify_waiters, true, "Game/Enhancements",
                    "Renut's render thread signals its work done (frames, fences, drains) only when a thread waits for "
                    "it: a signal is a kernel call on the Switch even with nobody waiting, and it made one after every "
                    "batch of work. Off: after every batch, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_prefetch_next, true, "Game/Enhancements",
                    "Renut's render thread asks for the next draw's capture (written on the game thread's core) to be "
                    "brought into its cache while it does the draw before it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fnxf_xiii_renut_wake_batch, 16, "Game/Enhancements",
                     "Renut's render thread, when it has caught up and sleeps, is woken by the game's draws only once "
                     "this many are queued (each wake is a kernel call on both threads; the fourth console run woke it "
                     "for almost every draw). Every other item (frames, fences, resources), every wait for it and a "
                     "2 ms timer still wake it at once. 1 or less: every draw wakes it, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_renut_batch_clock, true, "Game/Enhancements",
                    "Renut's render thread reads the clock once per batch of work instead of once per item (about "
                    "1,800 items a frame in City Escape); its busy time per frame is the same sum")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace renut::gpu {
bool StepDeferredMemExportsIdle();  // fnxf_xiii (2026-10-28): render/draw.cpp
void SettleExportsForFence();  // fnxf_xiii (2026-11-16): render/draw.cpp (fnxf_xiii_fence_waits_exports)
}

namespace renut::gpu::render_thread {

std::atomic<bool> g_idle_work{false};  // fnxf_xiii (2026-10-28): RequestIdleWork

namespace {

using Clock = std::chrono::steady_clock;

struct Item {
  std::function<void()> work;
  bool locked = true;  // run with State().mutex held
  const void *prefetch[2] = {};  // fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_prefetch_next): fetched while the item before runs
};

// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_prefetch_next): the next item's data into the cache (its first lines).
inline void PrefetchItem(const Item &item) {
  if (const auto *a = static_cast<const char *>(item.prefetch[0])) {
    __builtin_prefetch(a);
    __builtin_prefetch(a + 64);
  }
  if (const auto *b = static_cast<const char *>(item.prefetch[1])) {
    __builtin_prefetch(b);
    __builtin_prefetch(b + 64);
    __builtin_prefetch(b + 128);
    __builtin_prefetch(b + 192);
  }
}

std::mutex g_mutex;
std::condition_variable g_work_cv;  // work queued, or stopping
// fnxf_xiii (fnxf_xiii_renut_queue_notify_idle): the render thread is in g_work_cv.wait (under g_mutex). A push notifies only
// then: on the Switch every notify is a kernel call, about 1,500 a frame from the game threads, most of them while the
// render thread is busy and not waiting. The render thread reads the queue under g_mutex before it sleeps, so a push
// it has not seen is never missed.
bool g_render_waiting = false;
std::condition_variable g_done_cv;  // work done: frames, fences, drains; or stopped
// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_done_notify_waiters): threads in g_done_cv's waits (under g_mutex). A done signal goes
// only when there is one: a waiter checks its condition under g_mutex before it counts itself and sleeps, and the
// state it waits for changes under g_mutex too, so no signal it needs is skipped.
int g_done_waiters = 0;
bool DoneNotifyAll() {
  static const bool on = REXCVAR_GET(fnxf_xiii_renut_done_notify_waiters);
  return !on;
}
// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_wake_batch): the draws queued before a sleeping render thread is woken (1: each).
size_t WakeBatch() {
  static const size_t n = size_t(std::max(1, int(REXCVAR_GET(fnxf_xiii_renut_wake_batch))));
  return n;
}
std::vector<Item> g_queue;
std::once_flag g_start_once;
std::thread::id g_thread_id;
bool g_running = false;   // the thread takes work
bool g_stopping = false;  // Stop: finish what is queued, then end
uint64_t g_items_queued = 0;
uint64_t g_items_done = 0;
uint64_t g_frames_queued = 0;
uint64_t g_frames_done = 0;
uint64_t g_fences_done = 0;  // fence completions written, plus callbacks reached
uint64_t g_callbacks_reached = 0;
uint64_t g_callbacks_run = 0;
// Fence completions waiting for their callbacks, in order.
struct PendingFence {
  u32 completion_va;
  u32 fence;
  uint64_t after_callback;
};
std::vector<PendingFence> g_pending_fences;

// Requires g_mutex.
void WriteFence(u32 completion_va, u32 fence) {
  if (auto *word = GuestPtr<be_u32>(completion_va))
    *word = fence;
  ++g_fences_done;
}

// Render thread only: time spent running work since the last frame ended, and
// when the running item started; of that time, how much it waited (WaitScope).
Clock::duration g_busy{};
Clock::time_point g_item_start{};
Clock::duration g_waited{};
thread_local bool t_render_thread = false;

uint64_t g_item_serial = 0;  // fnxf_xiii (2026-11-14, fnxf_xiii_repeat_draw_fast): ItemSerial

void Execute(Item &item) {
  ++g_item_serial;
  if (item.locked) {
    // fnxf_xiii (2026-11-05, [rtwork]): the wait for State().mutex (a game thread's hook holding it) timed.
    std::unique_lock lock(State().mutex, std::defer_lock);
    if (rtw::On()) {
      const rtw::Scope wait(rtw::kLockWait);
      lock.lock();
    } else {
      lock.lock();
    }
    item.work();
  } else {
    item.work();
  }
}

void Run() {
  {
    // fnxf_xiii: the thread may be a plain pthread on the Switch (Start), so it records its own id.
    std::lock_guard lock(g_mutex);
    g_thread_id = std::this_thread::get_id();
  }
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"renut render");
#endif
  fnxf_xiii::renut_gpu::OnRenderThreadStart();  // fnxf_xiii: its core and priority on the Switch
#if defined(__SWITCH__)
  // fnxf_xiii (2026-10-24): sampled through the game thread's slow frames too (fnxf_xiii_cpu_profiler,
  // fnxf_xiii_slow_frame_profile_ms): what the render thread did while the game waited for its fences.
  os::switch_cpu_profiler::MarkSlowFrameCompanion("render");
#endif
  t_render_thread = true;
  std::vector<Item> batch;
  for (;;) {
    {
      std::unique_lock lock(g_mutex);
      // fnxf_xiii (2026-10-28): idle work (RequestIdleWork) while nothing is queued, a part at a time, without g_mutex.
      while (g_queue.empty() && !g_stopping && g_idle_work.load(std::memory_order_relaxed)) {
        lock.unlock();
        const bool more = StepDeferredMemExportsIdle();
        if (!more)
          g_idle_work.store(false, std::memory_order_relaxed);
        lock.lock();
      }
      g_render_waiting = true;
      if (WakeBatch() > 1) {
        // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_wake_batch): draws queued without a wake are taken within 2 ms at the latest.
        while (g_queue.empty() && !g_stopping)
          g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
      } else {
        g_work_cv.wait(lock, [] { return !g_queue.empty() || g_stopping; });
      }
      g_render_waiting = false;
      if (g_queue.empty())
        break;
      batch.swap(g_queue);
    }
    // fnxf_xiii (2026-10-19, render-thread proposal 19): one clock read per item (the end of one is the start of the
    // next). Its other half, one State().mutex hold across consecutive locked items, hung the emulator run at the
    // first loading screen (a game thread's hook waits on the lock while the render thread's next item waits on it):
    // each item takes the lock on its own, as before.
    Clock::time_point start = Clock::now();
    static const bool batch_clock = REXCVAR_GET(fnxf_xiii_renut_batch_clock);
    if (batch_clock) {
      // fnxf_xiii (2026-10-20, from sa2-nx): the batch's start for every item (QueueFrame's item counts its frame up to itself from
      // there and leaves the rest of the batch to the next frame), one clock read at the end.
      g_item_start = start;
      static const bool prefetch = REXCVAR_GET(fnxf_xiii_renut_prefetch_next);
      const size_t n = batch.size();
      for (size_t i = 0; i < n; ++i) {
        if (prefetch && i + 1 < n)
          PrefetchItem(batch[i + 1]);
        Execute(batch[i]);
      }
      g_busy += Clock::now() - start;
    } else {
      for (Item &item : batch) {
        g_item_start = start;
        Execute(item);
        const Clock::time_point end = Clock::now();
        g_busy += end - start;
        start = end;
      }
    }
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_items_done += batch.size();
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
    batch.clear();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_done_cv.notify_all();
}

void Start() {
  std::call_once(g_start_once, [] {
    InstallLateInput();  // fnxf_xiii (2026-10-19, fnxf_xiii_late_input): the delay function (it reads the key per frame)
    std::lock_guard lock(g_mutex);
    g_running = true;
    // Never joined or destroyed: the renderer's objects are left at exit too
    // (Video::BeginShutdown), and a joinable std::thread destroyed at exit
    // would terminate the process.
    // fnxf_xiii: a 1 MB stack on the Switch (the conversions and the driver run deep on this thread), never joined
    // (fnxf_xiii_renut_tuning.h); Run records its id.
    fnxf_xiii::renut_gpu::StartLongLivedThread(Run, 1024 * 1024);
    if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Render thread started");
  });
}

// fnxf_xiii (2026-10-20, from sa2-nx, fnxf_xiii_renut_wake_batch): before a thread waits for the render thread, the draws queued without a
// wake are handed over. Requires g_mutex.
void WakeForWaitLocked() {
  if (g_render_waiting && !g_queue.empty())
    g_work_cv.notify_one();
}

void Push(std::function<void()> work, bool locked, const void *prefetch0 = nullptr,
          const void *prefetch1 = nullptr, bool draw = false) {
  Start();
  {
    std::unique_lock lock(g_mutex);
    // (Work queued by the render thread's own work runs after it.)
    if (g_running && !g_stopping) {
      g_queue.push_back({std::move(work), locked, {prefetch0, prefetch1}});
      ++g_items_queued;
      // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_wake_batch): a draw wakes a sleeping render thread once enough are queued.
      const bool wake = (g_render_waiting && (!draw || g_queue.size() >= WakeBatch())) ||
                        !fnxf_xiii::renut_gpu::QueueNotifyIdleOn();
      lock.unlock();
      if (wake)
        g_work_cv.notify_one();
      return;
    }
  }
  // Shutting down: nothing takes work any more, so run it here.
  Item item{std::move(work), locked, {}};
  Execute(item);
}

} // namespace

void Queue(std::function<void()> work) {
  Push(std::move(work), true);
}

void QueueUnlocked(std::function<void()> work) {
  Push(std::move(work), false);
}

void QueuePrefetched(const void *prefetch0, const void *prefetch1, std::function<void()> work) {
  Push(std::move(work), true, prefetch0, prefetch1, true);  // fnxf_xiii (from sa2-nx): the game's draws (fnxf_xiii_renut_wake_batch)
}

void QueueFenceCompletion(u32 completion_va, u32 fence, uint64_t after_callback) {
  QueueUnlocked([completion_va, fence, after_callback] {
    ::renut::gpu::SettleExportsForFence();  // fnxf_xiii (2026-11-16, fnxf_xiii_fence_waits_exports)
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      // Fences complete in order: behind one still waiting, this one waits too.
      if (g_pending_fences.empty() && after_callback <= g_callbacks_run)
        WriteFence(completion_va, fence);
      else
        g_pending_fences.push_back({completion_va, fence, after_callback});
      wake = g_done_waiters > 0 || DoneNotifyAll();  // fnxf_xiii (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

void QueueCallbackMarker(uint64_t seq) {
  QueueUnlocked([seq] {
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_callbacks_reached = std::max(g_callbacks_reached, seq);
      ++g_fences_done;  // wakes WaitForFenceProgress: there is a callback to run
      wake = g_done_waiters > 0 || DoneNotifyAll();  // fnxf_xiii (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

uint64_t CallbacksReached() {
  std::lock_guard lock(g_mutex);
  return g_callbacks_reached;
}

void CallbacksRun(uint64_t seq) {
  bool wake;
  {
    std::lock_guard lock(g_mutex);
    g_callbacks_run = std::max(g_callbacks_run, seq);
    size_t written = 0;
    for (; written < g_pending_fences.size(); ++written) {
      const PendingFence &p = g_pending_fences[written];
      if (p.after_callback > g_callbacks_run)
        break;
      WriteFence(p.completion_va, p.fence);
    }
    g_pending_fences.erase(g_pending_fences.begin(), g_pending_fences.begin() + written);
    wake = g_done_waiters > 0 || DoneNotifyAll();  // fnxf_xiii (from sa2-nx)
  }
  if (wake)
    g_done_cv.notify_all();
}

// fnxf_xiii (2026-10-24, the slow frames' fence waits): the render thread's side of a long fence wait, for the log.
std::string DescribeForFenceWait(FenceWaitState *state) {
  std::lock_guard lock(g_mutex);
  if (state)
    *state = {g_render_waiting, g_queue.size(), g_pending_fences.size()};
  return fmt::format("render thread {}, {} items queued ({} queued and {} done in all), frames {} queued / {} done, "
                     "fences done {}, callbacks reached {} / run {}, {} fence completions waiting for a callback{}",
                     g_render_waiting ? "idle (waiting for work)" : "busy", g_queue.size(), g_items_queued,
                     g_items_done, g_frames_queued, g_frames_done, g_fences_done, g_callbacks_reached,
                     g_callbacks_run, g_pending_fences.size(),
                     g_pending_fences.empty()
                         ? std::string()
                         : fmt::format(" (the first {:#x} after callback {})", g_pending_fences.front().fence,
                                       g_pending_fences.front().after_callback));
}

bool WaitForFenceProgress(std::chrono::milliseconds timeout) {
  std::unique_lock lock(g_mutex);
  if (!g_running || g_stopping)
    return false;
  const uint64_t seen = g_fences_done;
  WakeForWaitLocked();  // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_wake_batch)
  ++g_done_waiters;  // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_done_notify_waiters)
  g_done_cv.wait_for(lock, timeout,
                     [seen] { return g_fences_done != seen || !g_running || g_stopping; });
  --g_done_waiters;
  return g_running && !g_stopping;
}

// fnxf_xiii (2026-10-19, input latency, fnxf_xiii_late_input): the game reads its controller at the start of a frame, and
// when the render thread is the slower of the two its frame then waits: the Swap below waits for the frame before to be
// done. The slack of a frame is that wait plus the delay its read already had: how much later the read could have been
// without the frame reaching the render thread any later. The next frame's first read is delayed by the least slack of
// the last kSlackFrames frames less kLateInputMarginUs (rex::latency::SetFrameReadDelayFunction): a frame whose game
// work runs long eats into the margin, and a run of them shrinks the delay; with the game thread or the frame cap the
// limit there is no wait, and the delay goes to 0.
namespace {
constexpr uint32_t kSlackFrames = 16;
constexpr uint64_t kLateInputMarginUs = 2000;
// fnxf_xiii (2026-10-19, latency batch 3): the most the read is delayed: 80 % of the game's frame interval (averaged over
// the frames at QueueFrame), at most kLateInputCeilingUs. A fixed 30 ms held it back where the GPU is the limit: the
// console's 18 fps scenes (package 35) delayed 30.0 ms with more slack than that.
constexpr uint64_t kLateInputCeilingUs = 60000;
std::atomic<uint64_t> g_frame_interval_us{0};
uint64_t g_last_queue_us = 0;
uint64_t g_slack_us[kSlackFrames] = {};
uint32_t g_slack_next = 0;
uint32_t g_slack_count = 0;
std::atomic<uint32_t> g_applied_delay_us{0};
std::atomic<uint64_t> g_delay_total_us{0}, g_delay_frames{0};

// fnxf_xiii (2026-10-20, package 36s): fnxf_xiii_late_input_gpu (36l-36r), a second slack from the GPU queue's wait, is gone:
// on the console it gained nothing at the 30 fps cap and, in scenes just able to hold 30, its delay ratcheted up to
// 29 ms and cost about 6 fps (36q). docs/deko3d-renderer.md section 25.

uint32_t LateInputDelay() {
  if (!fnxf_xiii::renut_gpu::LateInputOn() || g_slack_count < kSlackFrames) {
    g_applied_delay_us.store(0, std::memory_order_relaxed);
    return 0;
  }
  uint64_t least = UINT64_MAX;
  for (uint64_t s : g_slack_us)
    least = std::min(least, s);
  const uint64_t interval = g_frame_interval_us.load(std::memory_order_relaxed);
  const uint64_t ceiling = std::min(kLateInputCeilingUs, interval * 4 / 5);
  const uint64_t delay = least > kLateInputMarginUs ? std::min(least - kLateInputMarginUs, ceiling) : 0;
  g_applied_delay_us.store(uint32_t(delay), std::memory_order_relaxed);
  g_delay_total_us.fetch_add(delay, std::memory_order_relaxed);
  g_delay_frames.fetch_add(1, std::memory_order_relaxed);
  return uint32_t(delay);
}
}  // namespace

void InstallLateInput() {
  rex::latency::SetFrameReadDelayFunction(&LateInputDelay);
}

std::string LateInputReport() {
  const uint64_t frames = g_delay_frames.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_delay_total_us.exchange(0, std::memory_order_relaxed);
  return frames ? fmt::format("late input: the first controller read of a frame delayed {:.1f} ms on average ({} "
                              "frames)",
                              double(total) / 1000.0 / double(frames), frames)
                : std::string("late input: no delay");
}

uint64_t QueueFrame(std::function<void()> work) {
  {
    std::lock_guard lock(g_mutex);
    ++g_frames_queued;
  }
  QueueUnlocked([work = std::move(work)] {
    work();
    // The frame's work includes this item's (the present); Run adds the whole
    // item afterwards, so what is left of it counts to the next frame.
    const Clock::duration so_far = Clock::now() - g_item_start;
    const double busy_ms =
        std::chrono::duration<double, std::milli>(g_busy + so_far - g_waited).count();
    g_busy = -so_far;
    g_waited = {};
    frame_stats::OnRenderThreadFrame(busy_ms);
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      ++g_frames_done;
      wake = g_done_waiters > 0 || DoneNotifyAll();  // fnxf_xiii (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
  const auto waiting = std::chrono::steady_clock::now();
  std::unique_lock lock(g_mutex);
  WakeForWaitLocked();  // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_wake_batch)
  ++g_done_waiters;  // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_done_notify_waiters)
  g_done_cv.wait(lock, [] {
    return !g_running || g_stopping || g_frames_queued - g_frames_done <= kMaxFramesAhead;
  });
  --g_done_waiters;
  const uint64_t done = g_frames_done;
  lock.unlock();
  const auto waited = std::chrono::steady_clock::now() - waiting;
  frame_stats::OnGameThreadWait(std::chrono::duration<double, std::milli>(waited).count());
  // fnxf_xiii (2026-10-19, fnxf_xiii_late_input): this frame's slack (the game thread only: one Swap at a time).
  {
    // The game's frame interval (the game thread only), averaged over about 16 frames.
    const uint64_t at = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (g_last_queue_us && at > g_last_queue_us) {
      const uint64_t gap = std::min<uint64_t>(at - g_last_queue_us, 1'000'000);
      const uint64_t avg = g_frame_interval_us.load(std::memory_order_relaxed);
      g_frame_interval_us.store(avg ? avg + (int64_t(gap) - int64_t(avg)) / 16 : gap, std::memory_order_relaxed);
    }
    g_last_queue_us = at;
  }
  g_slack_us[g_slack_next] =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count()) +
      g_applied_delay_us.exchange(0, std::memory_order_relaxed);
  g_slack_next = (g_slack_next + 1) % kSlackFrames;
  g_slack_count = std::min(g_slack_count + 1, kSlackFrames);
  return done;
}

bool IsCurrent() { return t_render_thread; }
uint64_t ItemSerial() { return g_item_serial; }

uint64_t FramesDone() {
  std::lock_guard lock(g_mutex);
  return g_frames_done;
}

void Drain() {
  std::unique_lock lock(g_mutex);
  if (!g_running || std::this_thread::get_id() == g_thread_id)
    return;
  const uint64_t target = g_items_queued;
  WakeForWaitLocked();  // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_wake_batch)
  ++g_done_waiters;  // fnxf_xiii (from sa2-nx) (fnxf_xiii_renut_done_notify_waiters)
  g_done_cv.wait(lock, [target] { return !g_running || g_items_done >= target; });
  --g_done_waiters;
}

double BusyMsThisFrame() {
  return std::chrono::duration<double, std::milli>(g_busy + (Clock::now() - g_item_start) - g_waited).count();
}

void RequestIdleWork() {
  g_idle_work.store(true, std::memory_order_relaxed);
}

double WaitedMsThisFrame() {
  return std::chrono::duration<double, std::milli>(g_waited).count();
}

WaitScope::WaitScope() : start_(Clock::now()), counted_(t_render_thread) {}

WaitScope::~WaitScope() {
  if (counted_)
    g_waited += Clock::now() - start_;
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_running || g_stopping)
      return;
    g_stopping = true;
  }
  g_work_cv.notify_all();
  g_done_cv.notify_all();
}

} // namespace renut::gpu::render_thread
