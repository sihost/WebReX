/**
 * @file        tests/web/fiber_web_test.cpp
 * @brief       Tests for the web fiber backend (src/core/fiber_web.cpp: one parked pthread per
 *              fiber, futex hand-off, thread_local state carried across switches).
 *
 * Build + run: tests/web/run_fiber_web_test.bat [node|chrome] (em++ -m64 -pthread; the chrome
 * variant runs the same binary in headless Chrome through tests/web/run_in_chrome.mjs).
 * Covers: N fibers ping-pong with main (exactly one pthread of the logical thread runs at a
 * time), registered thread_locals following the fiber, nested creation/switching, Destroy of
 * parked / never-started / finished fibers (stack unwound, destructors run), exceptions caught
 * inside a fiber and escaping one (rethrown on the thread fiber), Thread::Exit and an entry
 * return on a fiber exiting the rex::thread thread, Terminate and Thread::GetCurrentThread
 * while the thread runs on a fiber, and the switch round-trip time.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <rex/thread.h>
#include <rex/thread/fiber.h>
#include <rex/thread/safe_point.h>

using namespace std::chrono_literals;
using rex::thread::Fiber;
using rex::thread::Thread;
using rex::thread::WaitResult;
using Clock = std::chrono::steady_clock;

thread_local int g_tls_carried = 0;
thread_local int g_tls_private = 0;
REX_FIBER_INHERITED_TLS(g_tls_carried);

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

long long ElapsedMs(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// Exactly one pthread of the logical thread may run between switches.
std::atomic<int> g_running{0};
void Enter() { CHECK(g_running.fetch_add(1) == 0); }
void Leave() { g_running.fetch_sub(1); }
void Switch(Fiber* to) {
  Leave();
  Fiber::SwitchTo(to);
  Enter();
}

Fiber* g_main = nullptr;
std::string g_trace;

// --- ping-pong -------------------------------------------------------------------------------

struct PingArg {
  int index;
  int count = 0;
  bool own_pthread = false;
  pthread_t main_pthread;
};

void PingEntry(void* p) {
  auto* a = static_cast<PingArg*>(p);
  Enter();
  a->own_pthread = !pthread_equal(pthread_self(), a->main_pthread);
  for (;;) {
    ++a->count;
    CHECK(Fiber::Current() != g_main);
    Switch(g_main);
  }
}

void TestPingPong() {
  g_main = Fiber::ConvertCurrentThread();
  Enter();
  constexpr int kFibers = 8, kRounds = 200;
  std::vector<PingArg> args(kFibers);
  std::vector<Fiber*> fibers;
  for (int i = 0; i < kFibers; ++i) {
    args[i].index = i;
    args[i].main_pthread = pthread_self();
    fibers.push_back(Fiber::Create(256 * 1024, PingEntry, &args[i]));
  }
  auto start = Clock::now();
  for (int r = 0; r < kRounds; ++r) {
    for (int i = 0; i < kFibers; ++i) {
      Switch(fibers[i]);
      CHECK(args[i].count == r + 1);
      CHECK(Fiber::Current() == g_main);
    }
  }
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
  std::printf("  %d round trips (main -> fiber -> main): %.1f us each\n", kFibers * kRounds,
              double(us) / (kFibers * kRounds));
  for (int i = 0; i < kFibers; ++i) {
    CHECK(args[i].own_pthread);
    fibers[i]->Destroy();  // parked inside SwitchTo: unwound
  }
  Leave();
  g_main->Destroy();
  g_main = nullptr;
}

// --- thread_local state follows the fiber ----------------------------------------------------

void TlsEntry(void*) {
  Enter();
  CHECK(g_tls_carried == 42);  // registered: carried from the switching thread
  CHECK(g_tls_private == 0);   // not registered: the fiber pthread's own
  g_tls_carried = 43;
  g_tls_private = 9;
  Switch(g_main);
  CHECK(g_tls_carried == 44);
  CHECK(g_tls_private == 9);
  Switch(g_main);
}

void TestTlsFollows() {
  g_main = Fiber::ConvertCurrentThread();
  Enter();
  g_tls_carried = 42;
  g_tls_private = 7;
  Fiber* f = Fiber::Create(128 * 1024, TlsEntry, nullptr);
  Switch(f);
  CHECK(g_tls_carried == 43);
  CHECK(g_tls_private == 7);
  g_tls_carried = 44;
  Switch(f);
  f->Destroy();
  Leave();
  g_main->Destroy();
}

// --- nested creation and switching -----------------------------------------------------------

Fiber* g_a = nullptr;
Fiber* g_b = nullptr;
Fiber* g_c = nullptr;

void CEntry(void*) {
  Enter();
  g_trace += "C";
  Switch(g_a);
  g_trace += "C";
  Switch(g_b);
  CHECK(false);  // never resumed again
}

void BEntry(void*) {
  Enter();
  g_trace += "B";
  g_c = Fiber::Create(128 * 1024, CEntry, nullptr);
  Switch(g_c);
  g_trace += "B";
  Switch(g_main);
  CHECK(false);
}

void AEntry(void*) {
  Enter();
  g_trace += "A";
  g_b = Fiber::Create(128 * 1024, BEntry, nullptr);
  Switch(g_b);
  g_trace += "A";
  Switch(g_main);
  CHECK(false);
}

void TestNested() {
  g_trace.clear();
  g_main = Fiber::ConvertCurrentThread();
  Enter();
  g_a = Fiber::Create(128 * 1024, AEntry, nullptr);
  Switch(g_a);  // A B C A -> main
  g_trace += "M";
  Switch(g_c);  // C B -> main
  g_trace += "M";
  CHECK(g_trace == "ABCAMCBM");
  g_a->Destroy();
  g_b->Destroy();
  g_c->Destroy();
  Leave();
  g_main->Destroy();
}

// --- Destroy ---------------------------------------------------------------------------------

std::atomic<int> g_guard_dtors{0};
struct Guard {
  ~Guard() { g_guard_dtors.fetch_add(1); }
};
bool g_after_switch = false;
bool g_never_entered = true;

void ParkedEntry(void*) {
  Enter();
  Guard outer;
  {
    Guard inner;
    Switch(g_main);
    g_after_switch = true;
  }
  Switch(g_main);
}

void NeverEntry(void*) { g_never_entered = false; }

void TestDestroy() {
  g_main = Fiber::ConvertCurrentThread();
  Enter();
  g_guard_dtors = 0;
  Fiber* parked = Fiber::Create(128 * 1024, ParkedEntry, nullptr);
  Switch(parked);
  CHECK(g_guard_dtors == 0);
  Leave();
  parked->Destroy();  // the destroyer does not run guest code while the fiber unwinds
  Enter();
  CHECK(g_guard_dtors == 2);
  CHECK(!g_after_switch);

  Fiber* never = Fiber::Create(128 * 1024, NeverEntry, nullptr);
  never->Destroy();
  CHECK(g_never_entered);
  Leave();
  g_main->Destroy();
}

// --- exceptions ------------------------------------------------------------------------------

bool g_caught_inside = false;

void ThrowingEntry(void*) {
  Enter();
  try {
    throw std::runtime_error("inside");
  } catch (const std::runtime_error& e) {
    g_caught_inside = std::string(e.what()) == "inside";
  }
  Switch(g_main);
  Guard g;
  Leave();
  throw std::runtime_error("escaped");
}

void TestExceptions() {
  g_main = Fiber::ConvertCurrentThread();
  Enter();
  g_guard_dtors = 0;
  Fiber* f = Fiber::Create(128 * 1024, ThrowingEntry, nullptr);
  Switch(f);
  CHECK(g_caught_inside);
  bool caught_escaped = false;
  try {
    Leave();
    Fiber::SwitchTo(f);  // the fiber's escaping exception is rethrown here
    Enter();
  } catch (const std::runtime_error& e) {
    Enter();
    caught_escaped = std::string(e.what()) == "escaped";
  }
  CHECK(caught_escaped);
  CHECK(g_guard_dtors == 1);  // the fiber's frames were unwound on its own pthread
  CHECK(Fiber::Current() == g_main);
  f->Destroy();  // finished: only joins
  Leave();
  g_main->Destroy();
}

// --- rex::thread threads on fibers -----------------------------------------------------------

std::unique_ptr<Thread> Spawn(std::function<void()> fn) {
  Thread::CreationParameters params;
  params.stack_size = 1024 * 1024;
  return Thread::Create(params, std::move(fn));
}

bool Join(Thread* t, std::chrono::milliseconds timeout = 3000ms) {
  return rex::thread::Wait(t, false, timeout) == WaitResult::kSuccess;
}

struct ThreadCase {
  Fiber* thread_fiber = nullptr;
  Fiber* fiber = nullptr;
  Thread* expected_thread = nullptr;
  std::atomic<bool> same_thread{false};
  std::atomic<bool> after_switch{false};
  std::atomic<bool> fiber_started{false};
  std::atomic<int> mode{0};  // 0 = Thread::Exit, 1 = return, 2 = spin until terminated
};

void ThreadCaseEntry(void* p) {
  auto* c = static_cast<ThreadCase*>(p);
  Guard g;
  c->same_thread = Thread::GetCurrentThread() == c->expected_thread &&
                   Thread::GetCurrentThread() != nullptr;
  rex::thread::Sleep(1ms);  // waits work on the fiber (same control block)
  c->fiber_started = true;
  if (c->mode == 0) {
    Thread::Exit(5);
  } else if (c->mode == 2) {
    for (;;) {
      rex::thread::SafePoint();
      rex::thread::MaybeYield();
    }
  }
  // mode 1: return -> the thread exits
}

void RunThreadCase(int mode, const char* label) {
  ThreadCase c;
  c.mode = mode;
  g_guard_dtors = 0;
  std::atomic<Thread*> self{nullptr};
  auto t = Spawn([&] {
    self = Thread::GetCurrentThread();
    c.expected_thread = Thread::GetCurrentThread();
    Guard g;
    c.thread_fiber = Fiber::ConvertCurrentThread();
    c.fiber = Fiber::Create(256 * 1024, ThreadCaseEntry, &c);
    Fiber::SwitchTo(c.fiber);
    c.after_switch = true;  // never: the thread exits from the fiber
  });
  if (mode == 2) {
    auto start = Clock::now();
    while (!c.fiber_started && ElapsedMs(start) < 3000) rex::thread::Sleep(1ms);
    t->Terminate(7);
  }
  bool joined = Join(t.get());
  CHECK(joined);
  CHECK(c.same_thread);
  CHECK(!c.after_switch);
  CHECK(g_guard_dtors == 2);  // the fiber's and the thread's frames were both unwound
  if (!joined || g_guard_dtors != 2) std::printf("  (%s)\n", label);
  if (joined) {
    c.fiber->Destroy();
    c.thread_fiber->Destroy();
  }
}

void TestThreadExitOnFiber() { RunThreadCase(0, "Thread::Exit"); }
void TestEntryReturnExitsThread() { RunThreadCase(1, "entry return"); }
void TestTerminateOnFiber() { RunThreadCase(2, "Terminate"); }

struct TestCase {
  const char* name;
  void (*fn)();
};

}  // namespace

int main() {
  const TestCase tests[] = {
      {"ping_pong_8_fibers", TestPingPong},
      {"tls_follows_fiber", TestTlsFollows},
      {"nested", TestNested},
      {"destroy", TestDestroy},
      {"exceptions", TestExceptions},
      {"thread_exit_on_fiber", TestThreadExitOnFiber},
      {"entry_return_exits_thread", TestEntryReturnExitsThread},
      {"terminate_on_fiber", TestTerminateOnFiber},
  };
  for (const auto& test : tests) {
    int before = g_failures;
    auto start = Clock::now();
    test.fn();
    std::printf("%s %-32s (%lld ms)\n", g_failures == before ? "PASS" : "FAIL", test.name,
                ElapsedMs(start));
  }
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
