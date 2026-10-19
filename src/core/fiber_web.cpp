/**
 * @file        rex/core/fiber_web.cpp
 * @brief       Web (Emscripten) backend for rex::thread::Fiber: one parked pthread per fiber.
 *
 * Wasm has no way to switch stacks without Asyncify (decision D6: no -sASYNCIFY over ~19.5k
 * generated functions), and there is no ucontext in Emscripten. A fiber is therefore a pthread
 * with its own host stack that only runs while the fiber is the current one:
 *
 *   - ConvertCurrentThread: the handle denotes the calling pthread (the "thread fiber").
 *   - Create: records entry/arg/stack size; the pthread is created on the first SwitchTo, so
 *     fibers that are never entered cost nothing.
 *   - SwitchTo(target): the running pthread hands the logical thread's thread_local state to the
 *     target (every slot registered with REX_FIBER_INHERITED_TLS: current XThread, ThreadState,
 *     PPCContext, memory base, rex::thread control block, thread id, soft FPSCR, ...), wakes it
 *     (a per-fiber futex word), and parks on its own futex word until somebody switches back. So
 *     exactly one pthread of a logical thread runs at a time, and guest code, kernel calls,
 *     waits and Thread::GetCurrentThread() on a fiber see the same thread as before the switch.
 *   - An exception that escapes a fiber's entry (including the private unwind that
 *     Thread::Exit/Terminate throws, threading_web.cpp) is handed to the thread fiber of the
 *     logical thread and rethrown there, so the thread unwinds back to its ThreadMain as if the
 *     fiber stack were part of it. An entry that returns makes the thread exit (Windows:
 *     returning from a fiber routine exits the thread) via Thread::Exit(0) on the thread fiber.
 *   - Destroy of a parked fiber wakes it with a private exception that unwinds its stack (C++
 *     destructors run), then joins the pthread.
 *
 * Limits (docs/port/STUBS.md, threading.md): a host lock held across a switch is owned by a
 * different pthread on the other side (recursive mutexes, e.g. the global critical region,
 * must not be held across KeSetCurrentStackPointers); pthread_exit on a fiber pthread (only the
 * --thread_exit_unwind=false path) parks the logical thread forever; a fiber costs one
 * Worker. Each switch is two futex operations plus a Worker wake-up (tens of microseconds).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WEB

#include <rex/thread.h>
#include <rex/thread/fiber.h>

#include <emscripten/threading.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

namespace {

struct TlsSlot {
  uint64_t (*get)();
  void (*set)(uint64_t);
};

// Filled by static initializers only (REX_FIBER_INHERITED_TLS), read-only afterwards.
std::vector<TlsSlot>& Slots() {
  static std::vector<TlsSlot> slots;
  return slots;
}

// The fiber the logical thread was converted from (target of escaped exceptions). Carried
// across switches like the registered slots.
thread_local Fiber* tls_thread_fiber = nullptr;

// Thrown inside a parked fiber by Destroy; caught by its pthread's start routine.
struct FiberCancelUnwind {};

[[noreturn]] void FiberFatal(const char* what) {
  std::fprintf(stderr, "[fiber] %s\n", what);
  std::abort();
}

void WaitFlag(std::atomic<uint32_t>& flag) {
  // Level-triggered: a wake that arrives before the wait is not lost.
  while (flag.exchange(0, std::memory_order_acquire) == 0) {
    emscripten_futex_wait(&flag, 0, INFINITY);
  }
}

void RaiseFlag(std::atomic<uint32_t>& flag) {
  flag.store(1, std::memory_order_release);
  emscripten_futex_wake(&flag, 1);
}

}  // namespace

bool RegisterFiberInheritedTls(uint64_t (*get)(), void (*set)(uint64_t)) {
  Slots().push_back(TlsSlot{get, set});
  return true;
}

struct Fiber::WebState {
  std::atomic<uint32_t> wake{0};     // 1 = this fiber may run
  std::atomic<uint32_t> done{0};     // created fibers: the pthread left its entry
  std::vector<uint64_t> carry;       // slot values handed over by the switching pthread
  Fiber* home = nullptr;             // the switcher's thread fiber
  std::exception_ptr pending;        // rethrown in this fiber when it resumes
  bool exit_thread = false;          // Thread::Exit(0) when this (thread) fiber resumes
  bool cancel = false;               // Destroy: unwind this fiber's stack
  // Created fibers only.
  void (*entry)(void*) = nullptr;
  void* arg = nullptr;
  size_t stack_size = 0;
  bool started = false;
  pthread_t pthread{};

  // Runs on the switching pthread: hand the logical thread's state to `target` and let it run.
  static void Handoff(Fiber* target) {
    WebState* w = target->web_;
    const auto& slots = Slots();
    w->carry.resize(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
      w->carry[i] = slots[i].get();
    }
    w->home = tls_thread_fiber;
    if (!target->is_thread_fiber_ && !w->started) {
      w->started = true;
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, std::max<size_t>(w->stack_size, 64 * 1024));
      int err = pthread_create(&w->pthread, &attr, &WebState::ThreadMain, target);
      pthread_attr_destroy(&attr);
      if (err != 0) {
        FiberFatal("SwitchTo: pthread_create for the fiber failed (Worker pool / memory)");
      }
      return;
    }
    RaiseFlag(w->wake);
  }

  // Runs on the resumed pthread before any of its code: adopt the logical thread's state.
  static void Adopt(Fiber* self) {
    WebState* w = self->web_;
    const auto& slots = Slots();
    for (size_t i = 0; i < slots.size() && i < w->carry.size(); ++i) {
      slots[i].set(w->carry[i]);
    }
    tls_thread_fiber = w->home ? w->home : (self->is_thread_fiber_ ? self : nullptr);
    tls_current_ = self;
  }

  // Parks the calling pthread until `self` is switched to again, then acts on what was asked.
  static void Park(Fiber* self) {
    WebState* w = self->web_;
    WaitFlag(w->wake);
    Adopt(self);
    if (w->cancel) {
      throw FiberCancelUnwind{};
    }
    if (w->pending) {
      std::exception_ptr e = std::move(w->pending);
      w->pending = nullptr;
      std::rethrow_exception(e);
    }
    if (w->exit_thread) {
      w->exit_thread = false;
      Thread::Exit(0);
    }
  }

  static void* ThreadMain(void* param) {
    Fiber* self = static_cast<Fiber*>(param);
    WebState* w = self->web_;
    Adopt(self);
    std::exception_ptr escaped;
    bool returned = false;
    try {
      w->entry(w->arg);
      returned = true;
    } catch (const FiberCancelUnwind&) {
      // Destroy: the stack is unwound; nothing runs on behalf of the logical thread any more.
      w->done.store(1, std::memory_order_release);
      emscripten_futex_wake(&w->done, 1);
      return nullptr;
    } catch (...) {
      escaped = std::current_exception();
    }
    // The entry left (returned or threw): the logical thread continues on its thread fiber.
    Fiber* home = tls_thread_fiber;
    if (!home || home == self) {
      FiberFatal(returned ? "fiber entry returned and the thread has no thread fiber"
                          : "exception escaped a fiber and the thread has no thread fiber");
    }
    if (returned) {
      home->web_->exit_thread = true;
    } else {
      home->web_->pending = std::move(escaped);
    }
    tls_current_ = nullptr;
    w->done.store(1, std::memory_order_release);
    Handoff(home);
    return nullptr;
  }
};

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  f->web_ = new WebState();
  tls_current_ = f;
  tls_thread_fiber = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->web_ = new WebState();
  f->web_->entry = entry;
  f->web_->arg = arg;
  f->web_->stack_size = stack_size;
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  if (!target || target == from) {
    return;  // resuming the running context is a no-op, as with SwitchToFiber/swapcontext
  }
  if (!from) {
    FiberFatal("SwitchTo called on a thread that is not a fiber (ConvertCurrentThread first)");
  }
  if (target->web_->done.load(std::memory_order_acquire)) {
    FiberFatal("SwitchTo a fiber whose entry has already left");
  }
  WebState::Handoff(target);
  WebState::Park(from);
}

void Fiber::Destroy() {
  WebState* w = web_;
  if (is_thread_fiber_) {
    // XThread's destructor may run on another thread; only clear our own slots.
    if (tls_current_ == this) {
      tls_current_ = nullptr;
    }
    if (tls_thread_fiber == this) {
      tls_thread_fiber = nullptr;
    }
  } else {
    if (this == tls_current_) {
      FiberFatal("Destroy called on the running fiber");
    }
    if (w->started) {
      if (!w->done.load(std::memory_order_acquire)) {
        // Parked inside SwitchTo: unwind its stack on its own pthread, wait until it is gone.
        w->cancel = true;
        WebState::Handoff(this);
        while (!w->done.load(std::memory_order_acquire)) {
          emscripten_futex_wait(&w->done, 0, INFINITY);
        }
      }
      pthread_join(w->pthread, nullptr);
    }
  }
  delete w;
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_WEB
