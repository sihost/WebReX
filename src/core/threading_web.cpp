/**
 * @file        core/threading_web.cpp
 * @brief       Web (Emscripten pthreads, wasm64) threading backend.
 *
 * Replaces threading_posix.cpp when REX_PLATFORM_WEB (src/core/CMakeLists.txt prefers
 * <unit>_web.cpp). Design notes: docs/port/threading.md in the project repo.
 *
 *  - One process-wide dispatcher mutex guards the state of every sync object and every
 *    thread's wait/suspend/APC state, like the NT dispatcher lock. A blocked thread sleeps on
 *    its own condition variable; objects keep the list of threads blocked on them and notify
 *    them when their state changes. WaitAll is atomic, wake-ups are never lost, nothing polls.
 *  - The web has no signals (no SIGRTMIN, pthread_kill, pthread_sigqueue, SIGEV_THREAD_ID).
 *    Suspend, terminate and user-callback (APC) delivery are cooperative:
 *      * a thread blocked in any wait or sleep is woken and parks there while suspended, or
 *        exits when terminated;
 *      * a running thread notices the request at rex::thread::SafePoint() / MaybeYield();
 *      * user callbacks run only in alertable waits/sleeps (as on native POSIX).
 *    A thread that spins in guest code without waiting can not be suspended or terminated.
 *  - Waitable timers are driven by one dedicated timer thread, started on first use.
 *  - Priority and affinity are recorded and reported, but have no effect (browsers schedule
 *    Workers themselves).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/fiber.h>
#include <rex/thread/safe_point.h>
#include <rex/thread/thread_report.h>

static_assert(REX_PLATFORM_WEB, "This file is web-only (Emscripten)");

#include <emscripten/threading.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(thread_exit_unwind, true, "Kernel",
                    "Web: Thread::Exit/Terminate unwind the thread's C++ frames back to its start "
                    "routine while the pthread is alive. false = the old pthread_exit, whose "
                    "destructors ran on a torn-down pthread (tid 0) and could leave the global "
                    "critical region locked for good (diagnostics/A-B only)");

namespace rex::thread {

namespace detail {
std::atomic<uint32_t> g_safe_point_requests{0};
}  // namespace detail

namespace {

using Clock = std::chrono::steady_clock;

// Timeouts longer than this are treated as infinite (avoids time_point overflow).
constexpr auto kMaxFiniteTimeout = std::chrono::hours(24 * 365 * 50);

// The dispatcher lock. Constant-initialized, so it is usable during static init.
std::mutex g_dispatcher;

std::atomic<uint32_t> g_next_system_id{1};

class WebObject;
class WebThread;

// What a thread is doing, for DescribeThreadsForDiagnostics only (written by the thread itself,
// read racily by the report).
struct ThreadDiagnostics {
  std::atomic<int> tid{0};                        // pthread tid (gettid), 0 until started
  std::atomic<const char*> state{"created"};     // static strings
  std::atomic<const void*> wait_object{nullptr};  // first object of the wait
  std::atomic<const char*> wait_kind{nullptr};
  std::atomic<uint32_t> wait_count{0};
  std::atomic<bool> wait_timed{false};
  std::atomic<int64_t> state_since_us{0};  // steady clock
};

// Per-thread scheduling state. Shared (shared_ptr) between the thread itself and its Thread
// object, so either may go away first. Every field is guarded by g_dispatcher unless noted.
struct ThreadControl {
  ThreadDiagnostics diag;  // not guarded (atomics)
  std::condition_variable cv;  // only its own thread waits on it (with g_dispatcher)
  uint32_t system_id = 0;      // immutable after creation
  pthread_t pthread{};
  bool started = false;
  bool finished = false;
  int exit_code = 0;
  uint32_t suspend_count = 0;
  bool terminate_requested = false;
  int terminate_exit_code = 0;
  bool parked = false;              // parked at a safe point because it is suspended
  bool safe_point_flagged = false;  // counted in detail::g_safe_point_requests
  const WebObject* pulse_token = nullptr;  // set by Event::Pulse for a blocked waiter
  std::deque<std::function<void()>> user_callbacks;
  WebThread* object = nullptr;  // the Thread object, if still alive
  std::string name;
  int32_t priority = 0;
  uint64_t affinity_mask = 0;
  bool exit_unwinds = false;  // runs under ThreadMain (own thread only): Exit unwinds to it
};

thread_local std::shared_ptr<ThreadControl> tls_control_ref;
thread_local ThreadControl* tls_control = nullptr;
// A fiber (fiber_web.cpp) runs on its own pthread but is the same rex::thread thread: waits,
// suspend/terminate requests and Exit act on the logical thread's control block. Only the raw
// pointer is carried; the owning reference stays with the thread's own pthread.
REX_FIBER_INHERITED_TLS(tls_control);

// Every control block ever created (kept alive; a few dozen per process), for the report.
std::mutex g_registry_mutex;
std::vector<std::shared_ptr<ThreadControl>>* g_registry = nullptr;

void RegisterControl(const std::shared_ptr<ThreadControl>& control) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (!g_registry) {
    g_registry = new std::vector<std::shared_ptr<ThreadControl>>();
  }
  g_registry->push_back(control);
}

int64_t SteadyMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void SetDiagState(ThreadControl* c, const char* state) {
  c->diag.state_since_us.store(SteadyMicros(), std::memory_order_relaxed);
  c->diag.state.store(state, std::memory_order_relaxed);
}

uint64_t AllProcessorsMask() {
  uint32_t n = std::clamp<uint32_t>(logical_processor_count(), 1u, 64u);
  return n >= 64 ? ~uint64_t(0) : ((uint64_t(1) << n) - 1);
}

void ApplyNativeThreadName(const std::string& name) {
  // Only used by the browser devtools / Emscripten profiler.
  emscripten_set_thread_name(pthread_self(), name.c_str());
}

// Returns the calling thread's control block, creating one for threads that were not started
// by Thread::Create (the main thread, std::thread workers, the timer thread).
ThreadControl* CurrentControl() {
  if (tls_control) {
    return tls_control;
  }
  auto control = std::make_shared<ThreadControl>();
  control->system_id = g_next_system_id.fetch_add(1, std::memory_order_relaxed);
  control->pthread = pthread_self();
  control->started = true;
  control->affinity_mask = AllProcessorsMask();
  control->diag.tid.store(gettid(), std::memory_order_relaxed);
  SetDiagState(control.get(), "running");
  RegisterControl(control);
  tls_control_ref = std::move(control);
  tls_control = tls_control_ref.get();
  return tls_control;
}

void UpdateSafePointFlagLocked(ThreadControl* c) {
  const bool need = c->started && !c->finished && !c->parked &&
                    (c->suspend_count > 0 || c->terminate_requested);
  if (need != c->safe_point_flagged) {
    c->safe_point_flagged = need;
    if (need) {
      detail::g_safe_point_requests.fetch_add(1, std::memory_order_relaxed);
    } else {
      detail::g_safe_point_requests.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

enum class SafePointAction { kContinue, kTerminate };

// Parks the calling thread while it is suspended. Returns kTerminate if the thread must exit.
SafePointAction SafePointLocked(ThreadControl* c, std::unique_lock<std::mutex>& lock) {
  while (!c->terminate_requested && c->suspend_count > 0) {
    if (!c->parked) {
      c->parked = true;
      UpdateSafePointFlagLocked(c);
      SetDiagState(c, "suspended");
    }
    c->cv.wait(lock);
  }
  if (c->parked) {
    c->parked = false;
    UpdateSafePointFlagLocked(c);
    SetDiagState(c, "running");
  }
  return c->terminate_requested ? SafePointAction::kTerminate : SafePointAction::kContinue;
}

[[noreturn]] void ExitCurrentThreadLocked(ThreadControl* c, std::unique_lock<std::mutex>& lock,
                                          int exit_code);

// Base for every waitable object. All members are guarded by g_dispatcher.
class WebObject {
 public:
  virtual ~WebObject() {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    // Destroying an object while a thread is blocked on it is a caller bug (as on native).
    assert_true(waiters_.empty());
  }

  // For the diagnostics report.
  virtual const char* KindName() const = 0;
  // True if a wait by `waiter` would be satisfied right now.
  virtual bool IsSignaledLocked(const ThreadControl* waiter) const = 0;
  // Applies the side effect of a satisfied wait (auto-reset, count--, take ownership).
  virtual void AcquireLocked(ThreadControl* waiter) = 0;
  // The "signal" half of SignalAndWait (Set / Release(1) / Release).
  virtual bool SignalLocked(ThreadControl* caller) = 0;

  void AddWaiterLocked(ThreadControl* t) { waiters_.push_back(t); }
  void RemoveWaiterLocked(ThreadControl* t) {
    auto it = std::find(waiters_.begin(), waiters_.end(), t);
    if (it != waiters_.end()) {
      waiters_.erase(it);
    }
  }
  void WakeWaitersLocked() {
    for (auto* t : waiters_) {
      t->cv.notify_all();
    }
  }

 protected:
  std::vector<ThreadControl*> waiters_;
};

WebObject* AsWebObject(WaitHandle* handle) {
  return handle ? dynamic_cast<WebObject*>(handle) : nullptr;
}

// Runs every queued user callback (FIFO). Returns true if at least one ran.
bool DrainUserCallbacks(ThreadControl* c) {
  bool ran = false;
  for (;;) {
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(g_dispatcher);
      if (c->user_callbacks.empty()) {
        break;
      }
      callback = std::move(c->user_callbacks.front());
      c->user_callbacks.pop_front();
    }
    ran = true;
    if (callback) {
      callback();
    }
  }
  return ran;
}

Clock::time_point DeadlineAfter(Clock::duration timeout) {
  if (timeout >= kMaxFiniteTimeout) {
    return Clock::time_point::max();
  }
  if (timeout <= Clock::duration::zero()) {
    return Clock::now();
  }
  return Clock::now() + timeout;
}

Clock::time_point DeadlineAfter(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max() || timeout >= kMaxFiniteTimeout) {
    return Clock::time_point::max();
  }
  return DeadlineAfter(std::chrono::duration_cast<Clock::duration>(timeout));
}

// The single wait loop behind Wait/SignalAndWait/WaitMultiple/AlertableSleep.
// count == 0 is a pure (alertable) sleep until the deadline.
std::pair<WaitResult, size_t> WaitCore(WebObject* const* objects, size_t count, bool wait_all,
                                       bool alertable, Clock::time_point deadline,
                                       WebObject* signal_first) {
  ThreadControl* self = CurrentControl();
  std::unique_lock<std::mutex> lock(g_dispatcher);

  if (signal_first && !signal_first->SignalLocked(self)) {
    return {WaitResult::kFailed, 0};
  }

  bool registered = false;
  auto unregister = [&] {
    if (registered) {
      for (size_t i = 0; i < count; ++i) {
        objects[i]->RemoveWaiterLocked(self);
      }
      registered = false;
    }
  };

  WaitResult result = WaitResult::kTimeout;
  size_t index = 0;
  bool run_callbacks = false;
  for (;;) {
    // 1. Suspend / terminate safe point (the web replacement for the RT-signal handler).
    if (SafePointLocked(self, lock) == SafePointAction::kTerminate) {
      unregister();
      ExitCurrentThreadLocked(self, lock, self->terminate_exit_code);
    }

    // 2. Object state. Like NT, an already-satisfiable wait wins over pending APCs.
    const WebObject* pulse = self->pulse_token;
    self->pulse_token = nullptr;
    auto signaled = [&](const WebObject* o) { return o == pulse || o->IsSignaledLocked(self); };
    if (count) {
      if (wait_all) {
        bool all = true;
        for (size_t i = 0; i < count && all; ++i) {
          all = signaled(objects[i]);
        }
        if (all) {
          for (size_t i = 0; i < count; ++i) {
            if (objects[i] != pulse) {
              objects[i]->AcquireLocked(self);
            }
          }
          result = WaitResult::kSuccess;
          index = 0;
          break;
        }
      } else {
        bool done = false;
        for (size_t i = 0; i < count; ++i) {
          if (signaled(objects[i])) {
            if (objects[i] != pulse) {
              objects[i]->AcquireLocked(self);
            }
            result = WaitResult::kSuccess;
            index = i;
            done = true;
            break;
          }
        }
        if (done) {
          break;
        }
      }
    }

    // 3. User callbacks (APCs) end an alertable wait.
    if (alertable && !self->user_callbacks.empty()) {
      result = WaitResult::kUserCallback;
      run_callbacks = true;
      break;
    }

    // 4. Timeout.
    if (deadline != Clock::time_point::max() && Clock::now() >= deadline) {
      result = WaitResult::kTimeout;
      break;
    }

    // 5. Block until an object, an APC, a suspend/resume/terminate request or the deadline.
    if (!registered) {
      for (size_t i = 0; i < count; ++i) {
        objects[i]->AddWaiterLocked(self);
      }
      registered = true;
      self->diag.wait_object.store(count ? objects[0] : nullptr, std::memory_order_relaxed);
      self->diag.wait_kind.store(count ? objects[0]->KindName() : nullptr,
                                 std::memory_order_relaxed);
      self->diag.wait_count.store(uint32_t(count), std::memory_order_relaxed);
      self->diag.wait_timed.store(deadline != Clock::time_point::max(), std::memory_order_relaxed);
      SetDiagState(self, !count ? "sleep(alertable)" : wait_all ? "wait-all" : "wait");
    }
    if (deadline == Clock::time_point::max()) {
      self->cv.wait(lock);
    } else {
      self->cv.wait_until(lock, deadline);
    }
  }
  if (registered) {
    SetDiagState(self, "running");
  }
  unregister();
  lock.unlock();

  if (run_callbacks) {
    DrainUserCallbacks(self);
  }
  return {result, index};
}

// ---------------------------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------------------------

class WebEvent final : public Event, public WebObject {
 public:
  WebEvent(bool manual_reset, bool initial_state)
      : manual_reset_(manual_reset), signaled_(initial_state) {}

  void Set() override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    SignalLocked(nullptr);
  }

  void Reset() override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    signaled_ = false;
  }

  // Win32 PulseEvent: release the threads blocked on the event right now (all of them for a
  // manual-reset event, one for an auto-reset event) and leave it non-signaled. Unlike the
  // POSIX backend (Set, sleep 10 us, Reset) no waiter can miss the pulse or see it twice.
  void Pulse() override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    for (auto* t : waiters_) {
      t->pulse_token = this;
      t->cv.notify_all();
      if (!manual_reset_) {
        break;
      }
    }
    signaled_ = false;
  }

  void* native_handle() const override { return const_cast<WebEvent*>(this); }

  const char* KindName() const override {
    return manual_reset_ ? "event(manual)" : "event(auto)";
  }
  bool IsSignaledLocked(const ThreadControl*) const override { return signaled_; }
  void AcquireLocked(ThreadControl*) override {
    if (!manual_reset_) {
      signaled_ = false;
    }
  }
  bool SignalLocked(ThreadControl*) override {
    signaled_ = true;
    WakeWaitersLocked();
    return true;
  }

 private:
  const bool manual_reset_;
  bool signaled_;
};

// ---------------------------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------------------------

class WebSemaphore final : public Semaphore, public WebObject {
 public:
  WebSemaphore(int initial_count, int maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    std::lock_guard<std::mutex> lock(g_dispatcher);
    return ReleaseLocked(release_count, out_previous_count);
  }

  void* native_handle() const override { return const_cast<WebSemaphore*>(this); }

  const char* KindName() const override { return "semaphore"; }
  bool IsSignaledLocked(const ThreadControl*) const override { return count_ > 0; }
  void AcquireLocked(ThreadControl*) override { --count_; }
  bool SignalLocked(ThreadControl*) override { return ReleaseLocked(1, nullptr); }

 private:
  bool ReleaseLocked(int release_count, int* out_previous_count) {
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    WakeWaitersLocked();
    return true;
  }

  int count_;
  const int maximum_count_;
};

// ---------------------------------------------------------------------------------------------
// Mutant (recursive, owned)
// ---------------------------------------------------------------------------------------------

class WebMutant final : public Mutant, public WebObject {
 public:
  explicit WebMutant(bool initial_owner) {
    if (initial_owner) {
      owner_ = CurrentControl();
      count_ = 1;
    }
  }

  bool Release() override {
    ThreadControl* self = CurrentControl();
    std::lock_guard<std::mutex> lock(g_dispatcher);
    return ReleaseLocked(self);
  }

  void* native_handle() const override { return const_cast<WebMutant*>(this); }

  const char* KindName() const override { return "mutant"; }
  bool IsSignaledLocked(const ThreadControl* waiter) const override {
    return count_ == 0 || owner_ == waiter;
  }
  void AcquireLocked(ThreadControl* waiter) override {
    owner_ = waiter;
    ++count_;
  }
  bool SignalLocked(ThreadControl* caller) override { return ReleaseLocked(caller); }

 private:
  bool ReleaseLocked(ThreadControl* caller) {
    if (owner_ != caller || count_ == 0) {
      return false;
    }
    if (--count_ == 0) {
      owner_ = nullptr;
      WakeWaitersLocked();
    }
    return true;
  }

  // Abandonment (owner exits while holding it) is not tracked, matching the POSIX backend.
  const ThreadControl* owner_ = nullptr;
  uint32_t count_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Waitable timer + the dedicated timer thread
// ---------------------------------------------------------------------------------------------

class WebTimer;

// Guarded by TimerThread::mutex_.
struct TimerState {
  WebTimer* owner = nullptr;
  uint64_t generation = 0;  // bumped by every Set*/Cancel; stale queue entries are skipped
  bool in_callback = false;
  std::function<void()> callback;
  Clock::duration period = Clock::duration::zero();
};

class TimerThread {
 public:
  // Started on first use, never stopped (like the native TimerQueue thread).
  static TimerThread& Get() {
    static TimerThread* instance = new TimerThread();
    return *instance;
  }

  bool OnTimerThread() const { return pthread_equal(pthread_self(), thread_) != 0; }

  // Removes every queued entry of `state`. mutex_ must be held.
  void UnscheduleLocked(const TimerState* state) {
    for (auto it = queue_.begin(); it != queue_.end();) {
      it = it->second.state.get() == state ? queue_.erase(it) : std::next(it);
    }
  }

  void ScheduleLocked(const std::shared_ptr<TimerState>& state, Clock::time_point due) {
    queue_.emplace(due, Entry{state, state->generation});
    cv_.notify_all();
  }

  // Disarms `state`: no callback for it runs after this returns, unless called from that very
  // callback (then it only prevents the next one). mutex_ must be held via `lock`.
  void CancelLocked(TimerState* state, std::unique_lock<std::mutex>& lock) {
    ++state->generation;
    UnscheduleLocked(state);
    if (!OnTimerThread()) {
      idle_cv_.wait(lock, [state] { return !state->in_callback; });
    }
  }

  std::mutex mutex_;

 private:
  struct Entry {
    std::shared_ptr<TimerState> state;
    uint64_t generation;
  };

  TimerThread() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int result = pthread_create(&thread_, &attr, &TimerThread::Main, this);
    pthread_attr_destroy(&attr);
    if (result != 0) {
      std::fprintf(stderr, "rex::thread: failed to start the web timer thread (%d)\n", result);
      std::abort();
    }
  }

  static void* Main(void* param) {
    set_current_thread_name("rex::thread::WebTimer");
    static_cast<TimerThread*>(param)->Run();
    return nullptr;
  }

  void Run();

  std::condition_variable cv_;       // wakes the timer thread (new entry)
  std::condition_variable idle_cv_;  // a callback finished (for CancelLocked)
  std::multimap<Clock::time_point, Entry> queue_;
  pthread_t thread_{};
};

class WebTimer final : public Timer, public WebObject {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit WebTimer(bool manual_reset)
      : manual_reset_(manual_reset), state_(std::make_shared<TimerState>()) {
    state_->owner = this;
  }

  ~WebTimer() override {
    if (!ever_armed_.load(std::memory_order_acquire)) {
      return;
    }
    auto& tt = TimerThread::Get();
    std::unique_lock<std::mutex> lock(tt.mutex_);
    tt.CancelLocked(state_.get(), lock);
    state_->owner = nullptr;
  }

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  }
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    Arm(due_time, Clock::duration::zero(), std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    Arm(due_time, std::chrono::duration_cast<Clock::duration>(period), std::move(opt_callback));
    return true;
  }

  // Win32 CancelWaitableTimer: disarm, leave the signaled state alone.
  bool Cancel() override {
    if (!ever_armed_.load(std::memory_order_acquire)) {
      return true;
    }
    auto& tt = TimerThread::Get();
    std::unique_lock<std::mutex> lock(tt.mutex_);
    tt.CancelLocked(state_.get(), lock);
    return true;
  }

  void* native_handle() const override { return const_cast<WebTimer*>(this); }

  const char* KindName() const override { return "timer"; }
  bool IsSignaledLocked(const ThreadControl*) const override { return signaled_; }
  void AcquireLocked(ThreadControl*) override {
    if (!manual_reset_) {
      signaled_ = false;
    }
  }
  bool SignalLocked(ThreadControl*) override {
    signaled_ = true;
    WakeWaitersLocked();
    return true;
  }

  // Called on the timer thread (TimerThread::mutex_ not held).
  void Fire() {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    SignalLocked(nullptr);
  }

 private:
  void Arm(Clock::time_point due, Clock::duration period, std::function<void()> callback) {
    ever_armed_.store(true, std::memory_order_release);
    auto& tt = TimerThread::Get();
    std::unique_lock<std::mutex> lock(tt.mutex_);  // lock order: timer mutex -> dispatcher
    tt.CancelLocked(state_.get(), lock);
    {
      // Reset before queueing so an already-due timer can not be lost.
      std::lock_guard<std::mutex> dispatcher_lock(g_dispatcher);
      signaled_ = false;
    }
    state_->callback = std::move(callback);
    state_->period = period;
    tt.ScheduleLocked(state_, due);
  }

  const bool manual_reset_;
  bool signaled_ = false;  // guarded by g_dispatcher
  std::atomic<bool> ever_armed_{false};
  std::shared_ptr<TimerState> state_;
};

void TimerThread::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (queue_.empty()) {
      cv_.wait(lock);
      continue;
    }
    auto it = queue_.begin();
    if (it->first > Clock::now()) {
      cv_.wait_until(lock, it->first);
      continue;
    }
    const Clock::time_point due = it->first;
    std::shared_ptr<TimerState> state = std::move(it->second.state);
    const uint64_t generation = it->second.generation;
    queue_.erase(it);
    if (generation != state->generation || !state->owner) {
      continue;  // cancelled or re-armed since it was queued
    }

    state->in_callback = true;
    WebTimer* owner = state->owner;
    std::function<void()> callback = state->callback;
    lock.unlock();
    // `owner` stays alive: its destructor / Cancel wait for in_callback to clear.
    owner->Fire();
    if (callback) {
      callback();  // like the POSIX backend: runs on the timer thread
    }
    lock.lock();
    state->in_callback = false;

    if (state->period > Clock::duration::zero() && state->generation == generation &&
        state->owner) {
      // Missed periods are coalesced into one (no callback flood after a stall).
      ScheduleLocked(state, std::max(due + state->period, Clock::now()));
    }
    idle_cv_.notify_all();
  }
}

// ---------------------------------------------------------------------------------------------
// Thread
// ---------------------------------------------------------------------------------------------

class WebThread final : public Thread, public WebObject {
 public:
  explicit WebThread(std::shared_ptr<ThreadControl> control) : control_(std::move(control)) {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    control_->object = this;
    name_ = control_->name;
  }

  ~WebThread() override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    if (control_->object == this) {
      control_->object = nullptr;
    }
  }

  void* native_handle() const override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    return reinterpret_cast<void*>(control_->pthread);
  }

  uint32_t system_id() const override { return control_->system_id; }

  void set_name(std::string name) override {
    Thread::set_name(name);
    bool is_current;
    {
      std::lock_guard<std::mutex> lock(g_dispatcher);
      control_->name = name;
      is_current = tls_control == control_.get();
    }
    // Another thread's devtools name is applied when it starts; later renames are cosmetic.
    if (is_current) {
      ApplyNativeThreadName(name);
    }
  }

  int32_t priority() override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    return control_->priority;
  }
  void set_priority(int32_t new_priority) override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    control_->priority = new_priority;  // no effect on the web
  }

  uint64_t affinity_mask() override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    return control_->affinity_mask;
  }
  void set_affinity_mask(uint64_t new_affinity_mask) override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    control_->affinity_mask = new_affinity_mask;  // no effect on the web
  }

  // Queued FIFO; runs when the thread is (or next becomes) blocked in an alertable wait/sleep.
  void QueueUserCallback(std::function<void()> callback) override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    control_->user_callbacks.push_back(std::move(callback));
    control_->cv.notify_all();
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) override {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = control_->suspend_count;
    }
    if (control_->suspend_count == 0) {
      return false;
    }
    if (--control_->suspend_count == 0) {
      control_->cv.notify_all();
    }
    UpdateSafePointFlagLocked(control_.get());
    return true;
  }

  // Cooperative: returns at once; the target stops at its next wait/sleep/safe point.
  // (Win32 SuspendThread is asynchronous for other threads too.) Self-suspend parks here.
  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) override {
    std::unique_lock<std::mutex> lock(g_dispatcher);
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = control_->suspend_count;
    }
    if (control_->finished) {
      return false;
    }
    ++control_->suspend_count;
    UpdateSafePointFlagLocked(control_.get());
    if (tls_control == control_.get()) {
      ThreadControl* self = control_.get();
      if (SafePointLocked(self, lock) == SafePointAction::kTerminate) {
        ExitCurrentThreadLocked(self, lock, self->terminate_exit_code);
      }
      return true;
    }
    control_->cv.notify_all();
    return true;
  }

  // Cooperative for other threads: the target exits at its next wait/sleep/safe point (also
  // when parked suspended), and the thread object becomes signaled when it has really exited.
  void Terminate(int exit_code) override {
    std::unique_lock<std::mutex> lock(g_dispatcher);
    if (control_->finished) {
      return;
    }
    if (tls_control == control_.get()) {
      ExitCurrentThreadLocked(control_.get(), lock, exit_code);
    }
    if (!control_->terminate_requested) {
      control_->terminate_requested = true;
      control_->terminate_exit_code = exit_code;
    }
    UpdateSafePointFlagLocked(control_.get());
    control_->cv.notify_all();
  }

  const char* KindName() const override { return "thread"; }
  bool IsSignaledLocked(const ThreadControl*) const override { return control_->finished; }
  void AcquireLocked(ThreadControl*) override {}
  bool SignalLocked(ThreadControl*) override { return true; }

  const std::shared_ptr<ThreadControl>& control() const { return control_; }

 private:
  std::shared_ptr<ThreadControl> control_;
};

void MarkFinishedLocked(ThreadControl* c, int exit_code) {
  SetDiagState(c, "finished");
  c->finished = true;
  c->exit_code = exit_code;
  UpdateSafePointFlagLocked(c);
  if (c->object) {
    c->object->WakeWaitersLocked();
  }
}

// Thrown by ExitCurrentThreadLocked on a thread started by ThreadMain and caught there.
struct ThreadExitUnwind {
  int exit_code;
};

void ExitCurrentThreadLocked(ThreadControl* c, std::unique_lock<std::mutex>& lock, int exit_code) {
  if (c->exit_unwinds && REXCVAR_GET(thread_exit_unwind)) {
    // Unwind the C++ frames (running their destructors) back to ThreadMain while this is still
    // a live pthread; ThreadMain marks the thread finished and returns.
    //
    // Not pthread_exit: Emscripten's pthread_exit first tears the pthread down (TLS dtors,
    // pthread_self() = NULL, so tid reads 0, and a detached thread's stack and TLS block are
    // queued to be freed on the main thread) and only then throws its JS 'unwind' exception.
    // With -fwasm-exceptions every C++ cleanup pad on the way catches that exception and runs
    // its destructors on the dead thread. XThread::Exit's object_ref then runs ~XThread
    // (UnregisterThread, SystemHeapFree, FreeStack), which take the global critical region
    // (a recursive mutex) with tid 0: musl sees a free mutex as "owned by me", only bumps
    // _m_count, and the next real owner inherits the count, so its last unlock never releases
    // the mutex. Every other thread then blocks on the global lock for good (gate 5: the
    // intermittent freeze after main_game_change; a guest thread exits during the map change,
    // and in the captured case the BINK thread inherited the lock and kept it while sleeping).
    lock.unlock();
    SetDiagState(c, "exiting");
    throw ThreadExitUnwind{exit_code};
  }
  MarkFinishedLocked(c, exit_code);
  lock.unlock();
  // Threads not started by ThreadMain (the main thread, std::thread): Emscripten unwinds the
  // Worker back to its event loop (a JS exception), see above.
  pthread_exit(reinterpret_cast<void*>(static_cast<intptr_t>(exit_code)));
}

struct ThreadStartData {
  std::shared_ptr<ThreadControl> control;
  std::function<void()> start_routine;
};

void* ThreadMain(void* parameter) {
  std::function<void()> start_routine;
  {
    std::unique_ptr<ThreadStartData> data(static_cast<ThreadStartData*>(parameter));
    tls_control_ref = std::move(data->control);
    start_routine = std::move(data->start_routine);
  }
  ThreadControl* c = tls_control_ref.get();
  tls_control = c;
  c->diag.tid.store(gettid(), std::memory_order_relaxed);
  SetDiagState(c, "starting");
  c->exit_unwinds = true;

  int exit_code = 0;
  try {
    std::string name;
    {
      std::unique_lock<std::mutex> lock(g_dispatcher);
      c->pthread = pthread_self();
      c->started = true;
      UpdateSafePointFlagLocked(c);
      name = c->name;
      // create_suspended threads park here until the first Resume().
      if (SafePointLocked(c, lock) == SafePointAction::kTerminate) {
        ExitCurrentThreadLocked(c, lock, c->terminate_exit_code);
      }
      name = c->name;
    }
    ApplyNativeThreadName(name);

    SetDiagState(c, "running");
    start_routine();
  } catch (const ThreadExitUnwind& exit) {
    // Thread::Exit / Terminate: every frame above has been unwound on this live pthread.
    exit_code = exit.exit_code;
  }
  start_routine = nullptr;

  {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    MarkFinishedLocked(c, exit_code);
  }
  return reinterpret_cast<void*>(static_cast<intptr_t>(exit_code));
}

}  // namespace

// =============================================================================================
// Public API
// =============================================================================================

void detail::SafePointSlow() {
  ThreadControl* c = tls_control;
  if (!c) {
    return;  // never registered, so nobody can have targeted it
  }
  std::unique_lock<std::mutex> lock(g_dispatcher);
  if (SafePointLocked(c, lock) == SafePointAction::kTerminate) {
    ExitCurrentThreadLocked(c, lock, c->terminate_exit_code);
  }
}

void EnableAffinityConfiguration() {}

uint32_t current_thread_system_id() {
  return CurrentControl()->system_id;
}

void set_current_thread_name(const std::string_view name) {
  ThreadControl* c = CurrentControl();
  std::string n(name);
  {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    c->name = n;
  }
  ApplyNativeThreadName(n);
}

void MaybeYield() {
  SafePoint();
  sched_yield();
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

void SyncMemory() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

void Sleep(std::chrono::microseconds duration) {
  ThreadControl* self = CurrentControl();
  const auto deadline = DeadlineAfter(std::chrono::duration_cast<Clock::duration>(duration));
  std::unique_lock<std::mutex> lock(g_dispatcher);
  bool marked = false;
  for (;;) {
    if (SafePointLocked(self, lock) == SafePointAction::kTerminate) {
      ExitCurrentThreadLocked(self, lock, self->terminate_exit_code);
    }
    if (Clock::now() >= deadline) {
      if (marked) {
        SetDiagState(self, "running");
      }
      return;
    }
    if (!marked) {
      marked = true;
      SetDiagState(self, "sleep");
    }
    if (deadline == Clock::time_point::max()) {
      self->cv.wait(lock);
    } else {
      self->cv.wait_until(lock, deadline);
    }
  }
}

SleepResult AlertableSleep(std::chrono::microseconds duration) {
  auto deadline = DeadlineAfter(std::chrono::duration_cast<Clock::duration>(duration));
  auto result = WaitCore(nullptr, 0, false, true, deadline, nullptr);
  return result.first == WaitResult::kUserCallback ? SleepResult::kAlerted : SleepResult::kSuccess;
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  [[maybe_unused]] auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  WebObject* object = AsWebObject(wait_handle);
  if (!object) {
    return WaitResult::kFailed;
  }
  return WaitCore(&object, 1, false, is_alertable, DeadlineAfter(timeout), nullptr).first;
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  WebObject* to_signal = AsWebObject(wait_handle_to_signal);
  WebObject* to_wait = AsWebObject(wait_handle_to_wait_on);
  if (!to_signal || !to_wait) {
    return WaitResult::kFailed;
  }
  // Signal and wait are one atomic step under the dispatcher lock (like NT).
  return WaitCore(&to_wait, 1, false, is_alertable, DeadlineAfter(timeout), to_signal).first;
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  if (!wait_handles || wait_handle_count == 0) {
    return {WaitResult::kFailed, 0};
  }
  std::vector<WebObject*> objects(wait_handle_count);
  for (size_t i = 0; i < wait_handle_count; ++i) {
    objects[i] = AsWebObject(wait_handles[i]);
    if (!objects[i]) {
      return {WaitResult::kFailed, 0};
    }
  }
  return WaitCore(objects.data(), objects.size(), wait_all, is_alertable, DeadlineAfter(timeout),
                  nullptr);
}

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<WebEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<WebEvent>(false, initial_state);
}

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<WebSemaphore>(initial_count, maximum_count);
}

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<WebMutant>(initial_owner);
}

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<WebTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<WebTimer>(false);
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  auto control = std::make_shared<ThreadControl>();
  control->system_id = g_next_system_id.fetch_add(1, std::memory_order_relaxed);
  control->priority = params.initial_priority;
  control->affinity_mask = AllProcessorsMask();
  control->suspend_count = params.create_suspended ? 1 : 0;
  RegisterControl(control);

  auto thread = std::make_unique<WebThread>(control);

  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) {
    return nullptr;
  }
  // XThreads ask for 16 MiB (xthread.cpp). Emscripten allocates pthread stacks from the heap;
  // link with -sPTHREAD_POOL_SIZE large enough to avoid lazy Worker creation (toolchain.md).
  size_t stack_size = std::max<size_t>(params.stack_size, 64 * 1024);
  if (pthread_attr_setstacksize(&attr, stack_size) != 0 ||
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
    pthread_attr_destroy(&attr);
    return nullptr;
  }
  auto* start_data = new ThreadStartData{control, std::move(start_routine)};
  pthread_t pthread{};
  int result = pthread_create(&pthread, &attr, ThreadMain, start_data);
  pthread_attr_destroy(&attr);
  if (result != 0) {
    delete start_data;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    control->pthread = pthread;
  }
  return thread;
}

Thread* Thread::GetCurrentThread() {
  ThreadControl* c = CurrentControl();
  {
    std::lock_guard<std::mutex> lock(g_dispatcher);
    if (c->object) {
      return c->object;
    }
  }
  // Threads not created by Thread::Create (main thread etc.), or whose Thread object is gone.
  // Like the POSIX backend this wrapper is intentionally leaked.
  if (tls_control_ref.get() != c) {
    // A fiber pthread (fiber_web.cpp) carries only the raw control pointer: find its owner.
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (const auto& control : *g_registry) {
      if (control.get() == c) {
        return new WebThread(control);
      }
    }
  }
  return new WebThread(tls_control_ref);
}

void Thread::Exit(int exit_code) {
  ThreadControl* c = CurrentControl();
  std::unique_lock<std::mutex> lock(g_dispatcher);
  ExitCurrentThreadLocked(c, lock, exit_code);
}

std::string DescribeThreadsForDiagnostics() {
  std::vector<std::shared_ptr<ThreadControl>> controls;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (g_registry) {
      controls = *g_registry;
    }
  }
  // Names are std::strings guarded by the dispatcher: copy them if it is free (it is only ever
  // held briefly), otherwise read them anyway (they are set once at thread start).
  bool dispatcher_locked = false;
  for (int i = 0; i < 1000 && !dispatcher_locked; ++i) {
    dispatcher_locked = g_dispatcher.try_lock();
    if (!dispatcher_locked) {
      sched_yield();
    }
  }
  std::vector<std::string> names;
  names.reserve(controls.size());
  for (const auto& c : controls) {
    names.push_back(c->name);
  }
  if (dispatcher_locked) {
    g_dispatcher.unlock();
  }
  const int64_t now = SteadyMicros();
  std::string out;
  char line[512];
  std::snprintf(line, sizeof(line), "threads: %zu known to rex::thread (dispatcher %s)\n",
                controls.size(), dispatcher_locked ? "free" : "BUSY - names read racily");
  out += line;
  for (size_t i = 0; i < controls.size(); ++i) {
    const ThreadDiagnostics& d = controls[i]->diag;
    const char* state = d.state.load(std::memory_order_relaxed);
    const double since_s = double(now - d.state_since_us.load(std::memory_order_relaxed)) / 1e6;
    int n = std::snprintf(line, sizeof(line), "  tid %d sys %u '%s': %s for %.1f s",
                          d.tid.load(std::memory_order_relaxed), controls[i]->system_id,
                          names[i].c_str(), state ? state : "?", since_s);
    if (n > 0 && size_t(n) < sizeof(line) && state && std::strncmp(state, "wait", 4) == 0) {
      std::snprintf(line + n, sizeof(line) - size_t(n), " on %u object(s), first %s %p%s",
                    d.wait_count.load(std::memory_order_relaxed),
                    d.wait_kind.load(std::memory_order_relaxed)
                        ? d.wait_kind.load(std::memory_order_relaxed)
                        : "?",
                    d.wait_object.load(std::memory_order_relaxed),
                    d.wait_timed.load(std::memory_order_relaxed) ? " (timed)" : "");
    }
    out += line;
    out += '\n';
  }
  return out;
}

}  // namespace rex::thread
