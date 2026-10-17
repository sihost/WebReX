/**
 * @file        tests/web/threading_web_test.cpp
 * @brief       Node-runnable tests for the web threading backend (src/core/threading_web.cpp).
 *
 * Build + run: tests/web/run_threading_web_test.bat (em++ -m64 -pthread, emsdk node).
 * Covers: wait/timeout, WaitAny/WaitAll, semaphores, mutants, SignalAndWait, Pulse,
 * alertable waits and APC (user callback) delivery, cooperative suspend at safe points and in
 * waits, create-suspended threads, cooperative terminate, Thread::Exit, waitable timers
 * (once, repeating, cancel, manual reset) and thread naming (the SetThreadName path).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/thread.h>
#include <rex/thread/safe_point.h>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Mutant;
using rex::thread::Semaphore;
using rex::thread::Thread;
using rex::thread::Timer;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;
using Clock = std::chrono::steady_clock;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                            \
  } while (0)

long long ElapsedMs(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::unique_ptr<Thread> Spawn(std::function<void()> fn, bool suspended = false) {
  Thread::CreationParameters params;
  params.stack_size = 16 * 1024 * 1024;  // what XThread asks for
  params.create_suspended = suspended;
  auto t = Thread::Create(params, std::move(fn));
  CHECK(t != nullptr);
  return t;
}

bool Join(Thread* t, std::chrono::milliseconds timeout = 2000ms) {
  return rex::thread::Wait(t, false, timeout) == WaitResult::kSuccess;
}

// Spins until pred() or timeout; returns pred().
bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 2000ms) {
  auto start = Clock::now();
  while (!pred()) {
    if (Clock::now() - start > timeout) return false;
    rex::thread::Sleep(1ms);
  }
  return true;
}

void TestEventWaitTimeout() {
  auto ev = Event::CreateAutoResetEvent(false);
  CHECK(rex::thread::Wait(ev.get(), false, 0ms) == WaitResult::kTimeout);
  auto start = Clock::now();
  CHECK(rex::thread::Wait(ev.get(), false, 50ms) == WaitResult::kTimeout);
  auto ms = ElapsedMs(start);
  CHECK(ms >= 49 && ms < 1000);

  // Set from another thread wakes a blocked waiter; auto-reset is consumed by one wait.
  std::atomic<bool> woke{false};
  auto t = Spawn([&] {
    woke = rex::thread::Wait(ev.get(), false) == WaitResult::kSuccess;
  });
  rex::thread::Sleep(20ms);
  CHECK(!woke);
  ev->Set();
  CHECK(Join(t.get()));
  CHECK(woke);
  CHECK(rex::thread::Wait(ev.get(), false, 0ms) == WaitResult::kTimeout);

  // Manual reset stays signaled until Reset.
  auto mev = Event::CreateManualResetEvent(true);
  CHECK(rex::thread::Wait(mev.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(rex::thread::Wait(mev.get(), false, 0ms) == WaitResult::kSuccess);
  mev->Reset();
  CHECK(rex::thread::Wait(mev.get(), false, 0ms) == WaitResult::kTimeout);
}

void TestWaitMultiple() {
  auto a = Event::CreateManualResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* hs[] = {a.get(), b.get()};
  auto r = rex::thread::WaitAny(hs, 2, false, 10ms);
  CHECK(r.first == WaitResult::kTimeout);
  b->Set();
  r = rex::thread::WaitAny(hs, 2, false, 0ms);
  CHECK(r.first == WaitResult::kSuccess && r.second == 1);
  // WaitAll must not consume b unless a is also signaled (atomic acquire).
  b->Set();
  CHECK(rex::thread::WaitAll(hs, 2, false, 10ms) == WaitResult::kTimeout);
  CHECK(rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kSuccess);  // still there
  b->Set();
  std::atomic<int> got{-1};
  auto t = Spawn([&] { got = int(rex::thread::WaitAll(hs, 2, false, 2000ms)); });
  rex::thread::Sleep(20ms);
  CHECK(got == -1);
  a->Set();
  CHECK(Join(t.get()));
  CHECK(got == int(WaitResult::kSuccess));
  CHECK(rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kTimeout);  // consumed
  CHECK(rex::thread::Wait(a.get(), false, 0ms) == WaitResult::kSuccess);  // manual
  CHECK(rex::thread::WaitAny(static_cast<WaitHandle**>(nullptr), 0, false, 0ms).first ==
        WaitResult::kFailed);
}

void TestSemaphoreMutant() {
  CHECK(Semaphore::Create(2, 1) == nullptr);
  auto s = Semaphore::Create(1, 3);
  int prev = -1;
  CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kTimeout);
  CHECK(s->Release(3, &prev) && prev == 0);
  CHECK(!s->Release(1, &prev));  // over maximum
  CHECK(!s->Release(0, &prev));
  for (int i = 0; i < 3; ++i) CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kTimeout);

  auto m = Mutant::Create(true);
  CHECK(rex::thread::Wait(m.get(), false, 0ms) == WaitResult::kSuccess);  // recursive
  std::atomic<int> other{-1};
  auto t = Spawn([&] {
    other = int(rex::thread::Wait(m.get(), false, 30ms));
    // Not the owner -> Release fails.
    if (m->Release()) other = 100;
  });
  CHECK(Join(t.get()));
  CHECK(other == int(WaitResult::kTimeout));
  CHECK(m->Release());
  CHECK(m->Release());
  CHECK(!m->Release());  // count is zero now
  auto t2 = Spawn([&] {
    other = int(rex::thread::Wait(m.get(), false, 500ms));
    m->Release();
  });
  CHECK(Join(t2.get()));
  CHECK(other == int(WaitResult::kSuccess));
}

void TestSignalAndWaitAndPulse() {
  auto ping = Event::CreateAutoResetEvent(false);
  auto pong = Event::CreateAutoResetEvent(false);
  std::atomic<int> rounds{0};
  auto t = Spawn([&] {
    for (int i = 0; i < 100; ++i) {
      if (rex::thread::Wait(ping.get(), false, 2000ms) != WaitResult::kSuccess) return;
      ++rounds;
      pong->Set();
    }
  });
  int ok = 0;
  for (int i = 0; i < 100; ++i) {
    if (rex::thread::SignalAndWait(ping.get(), pong.get(), false, 2000ms) == WaitResult::kSuccess) ++ok;
  }
  CHECK(Join(t.get()));
  CHECK(ok == 100 && rounds == 100);

  // Pulse on a manual-reset event releases every current waiter and leaves it non-signaled.
  auto ev = Event::CreateManualResetEvent(false);
  std::atomic<int> released{0};
  std::atomic<int> entered{0};
  std::vector<std::unique_ptr<Thread>> ts;
  for (int i = 0; i < 3; ++i) {
    ts.push_back(Spawn([&] {
      ++entered;
      if (rex::thread::Wait(ev.get(), false, 2000ms) == WaitResult::kSuccess) ++released;
    }));
  }
  CHECK(WaitFor([&] { return entered == 3; }));
  rex::thread::Sleep(30ms);  // from `entered` into the wait
  ev->Pulse();
  for (auto& x : ts) CHECK(Join(x.get()));
  CHECK(released == 3);
  CHECK(rex::thread::Wait(ev.get(), false, 0ms) == WaitResult::kTimeout);

  // Pulse on an auto-reset event releases exactly one waiter.
  auto aev = Event::CreateAutoResetEvent(false);
  released = 0;
  entered = 0;
  ts.clear();
  for (int i = 0; i < 2; ++i) {
    ts.push_back(Spawn([&] {
      ++entered;
      if (rex::thread::Wait(aev.get(), false, 500ms) == WaitResult::kSuccess) ++released;
    }));
  }
  CHECK(WaitFor([&] { return entered == 2; }));
  rex::thread::Sleep(30ms);
  aev->Pulse();
  for (auto& x : ts) CHECK(Join(x.get()));
  CHECK(released == 1);
}

void TestAlertableApc() {
  auto never = Event::CreateManualResetEvent(false);
  std::atomic<uint32_t> apc_thread{0};
  std::atomic<uint32_t> waiter_thread{0};
  std::atomic<int> result{-1};
  std::atomic<bool> entered{false};
  std::atomic<bool> nonalertable_done{false};
  std::atomic<int> apc_runs{0};
  std::atomic<int> runs_after_nonalertable{-1};
  auto t = Spawn([&] {
    waiter_thread = rex::thread::current_thread_system_id();
    entered = true;
    // A non-alertable wait must not run (or be ended by) queued callbacks.
    auto start = Clock::now();
    nonalertable_done = rex::thread::Wait(never.get(), false, 400ms) == WaitResult::kTimeout &&
                        ElapsedMs(start) >= 399;
    runs_after_nonalertable = apc_runs.load();
    result = int(rex::thread::Wait(never.get(), true));  // infinite, alertable
  });
  CHECK(WaitFor([&] { return entered.load(); }));
  t->QueueUserCallback([&] {
    apc_thread = rex::thread::current_thread_system_id();
    ++apc_runs;
  });
  t->QueueUserCallback([&] { ++apc_runs; });
  rex::thread::Sleep(20ms);
  CHECK(apc_runs == 0);  // still inside the non-alertable wait
  CHECK(Join(t.get()));
  CHECK(nonalertable_done);
  CHECK(runs_after_nonalertable == 0);
  CHECK(result == int(WaitResult::kUserCallback));
  CHECK(apc_runs == 2);  // both drained, FIFO, on the target thread
  CHECK(apc_thread == waiter_thread && apc_thread != 0);

  // AlertableSleep returns kAlerted early when a callback is queued.
  std::atomic<int> sleep_result{-1};
  std::atomic<long long> slept_ms{0};
  auto t2 = Spawn([&] {
    auto start = Clock::now();
    sleep_result = int(rex::thread::AlertableSleep(5000ms));
    slept_ms = ElapsedMs(start);
  });
  rex::thread::Sleep(30ms);
  t2->QueueUserCallback([] {});
  CHECK(Join(t2.get()));
  CHECK(sleep_result == int(rex::thread::SleepResult::kAlerted));
  CHECK(slept_ms < 2000);
  CHECK(rex::thread::AlertableSleep(10ms) == rex::thread::SleepResult::kSuccess);
}

void TestSuspendSafePoint() {
  std::atomic<uint64_t> counter{0};
  std::atomic<bool> stop{false};
  auto t = Spawn([&] {
    while (!stop) {
      ++counter;
      rex::thread::SafePoint();  // what a kernel export / loop back-edge would call
    }
  });
  CHECK(WaitFor([&] { return counter > 1000; }));
  uint32_t prev = 99;
  CHECK(t->Suspend(&prev) && prev == 0);
  CHECK(t->Suspend(&prev) && prev == 1);
  rex::thread::Sleep(30ms);  // let it reach the safe point
  uint64_t c1 = counter;
  rex::thread::Sleep(50ms);
  uint64_t c2 = counter;
  CHECK(c1 == c2);  // parked
  CHECK(t->Resume(&prev) && prev == 2);
  rex::thread::Sleep(30ms);
  CHECK(counter == c2);  // still suspended (count 1)
  CHECK(t->Resume(&prev) && prev == 1);
  CHECK(!t->Resume(&prev) && prev == 0);
  CHECK(WaitFor([&] { return counter > c2 + 1000; }));
  stop = true;
  CHECK(Join(t.get()));
  CHECK(rex::thread::detail::g_safe_point_requests.load() == 0);
}

void TestSuspendInWait() {
  auto ev = Event::CreateManualResetEvent(false);
  std::atomic<bool> woke{false};
  auto t = Spawn([&] {
    rex::thread::Wait(ev.get(), false);
    woke = true;
  });
  rex::thread::Sleep(20ms);
  CHECK(t->Suspend());
  ev->Set();
  rex::thread::Sleep(50ms);
  CHECK(!woke);  // suspended inside the wait: does not return while suspended
  CHECK(t->Resume());
  CHECK(Join(t.get()));
  CHECK(woke);

  // Sleep is a safe point too.
  std::atomic<int> ticks{0};
  std::atomic<bool> stop{false};
  auto t2 = Spawn([&] {
    while (!stop) {
      ++ticks;
      rex::thread::Sleep(1ms);
    }
  });
  CHECK(WaitFor([&] { return ticks > 5; }));
  CHECK(t2->Suspend());
  rex::thread::Sleep(20ms);
  int k = ticks;
  rex::thread::Sleep(40ms);
  CHECK(ticks == k);
  CHECK(t2->Resume());
  CHECK(WaitFor([&] { return ticks > k + 5; }));
  stop = true;
  CHECK(Join(t2.get()));
}

void TestCreateSuspendedAndSelfSuspend() {
  std::atomic<bool> ran{false};
  auto t = Spawn([&] { ran = true; }, /*suspended=*/true);
  rex::thread::Sleep(40ms);
  CHECK(!ran);
  uint32_t prev = 0;
  CHECK(t->Resume(&prev) && prev == 1);
  CHECK(Join(t.get()));
  CHECK(ran);

  // Self-suspend (CommandProcessor::Pause style): parks inside Suspend() until resumed.
  std::atomic<int> phase{0};
  auto t2 = Spawn([&] {
    phase = 1;
    Thread::GetCurrentThread()->Suspend();
    phase = 2;
  });
  CHECK(WaitFor([&] { return phase == 1; }));
  rex::thread::Sleep(30ms);
  CHECK(phase == 1);
  CHECK(t2->Resume());
  CHECK(Join(t2.get()));
  CHECK(phase == 2);
}

void TestTerminateAndExit() {
  // Blocked in a wait -> terminated promptly; its handle becomes signaled.
  auto never = Event::CreateManualResetEvent(false);
  std::atomic<bool> after{false};
  auto t = Spawn([&] {
    rex::thread::Wait(never.get(), false);
    after = true;
  });
  rex::thread::Sleep(20ms);
  CHECK(rex::thread::Wait(t.get(), false, 0ms) == WaitResult::kTimeout);
  t->Terminate(7);
  CHECK(Join(t.get()));
  CHECK(!after);

  // Suspend, then terminate (KernelState::TerminateTitle order) of a spinning thread.
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> spins{0};
  auto t2 = Spawn([&] {
    while (!stop) {
      ++spins;
      rex::thread::SafePoint();
    }
  });
  CHECK(WaitFor([&] { return spins > 100; }));
  t2->Suspend();
  t2->Terminate(0);
  CHECK(Join(t2.get()));

  // Thread::Exit from inside.
  std::atomic<bool> after_exit{false};
  auto t3 = Spawn([&] {
    Thread::Exit(3);
    after_exit = true;
  });
  CHECK(Join(t3.get()));
  CHECK(!after_exit);
  CHECK(!t3->Suspend());  // finished threads can not be suspended
  CHECK(rex::thread::detail::g_safe_point_requests.load() == 0);
}

void TestTimers() {
  // One-shot synchronization timer.
  auto timer = Timer::CreateSynchronizationTimer();
  std::atomic<int> fired{0};
  auto start = Clock::now();
  CHECK(timer->SetOnceAfter(std::chrono::milliseconds(30), [&] { ++fired; }));
  CHECK(rex::thread::Wait(timer.get(), false, 2000ms) == WaitResult::kSuccess);
  auto ms = ElapsedMs(start);
  CHECK(ms >= 29 && ms < 1500);
  CHECK(WaitFor([&] { return fired == 1; }));
  CHECK(rex::thread::Wait(timer.get(), false, 0ms) == WaitResult::kTimeout);  // auto reset

  // Repeating timer + cancel.
  auto rep = Timer::CreateSynchronizationTimer();
  std::atomic<int> ticks{0};
  CHECK(rep->SetRepeatingAfter(std::chrono::milliseconds(5), 10ms, [&] { ++ticks; }));
  CHECK(WaitFor([&] { return ticks >= 5; }));
  CHECK(rep->Cancel());
  int after_cancel = ticks;
  rex::thread::Sleep(60ms);
  CHECK(ticks == after_cancel);  // no callback after Cancel() returns

  // Manual-reset timer stays signaled; re-arming resets it.
  auto man = Timer::CreateManualResetTimer();
  CHECK(man->SetOnceAfter(std::chrono::milliseconds(5)));
  CHECK(rex::thread::Wait(man.get(), false, 2000ms) == WaitResult::kSuccess);
  CHECK(rex::thread::Wait(man.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(man->SetOnceAfter(std::chrono::milliseconds(200)));
  CHECK(rex::thread::Wait(man.get(), false, 0ms) == WaitResult::kTimeout);
  CHECK(man->Cancel());

  // Destroying an armed timer is safe; its callback never runs afterwards.
  std::atomic<int> late{0};
  {
    auto dying = Timer::CreateSynchronizationTimer();
    dying->SetOnceAfter(std::chrono::milliseconds(20), [&] { ++late; });
  }
  rex::thread::Sleep(60ms);
  CHECK(late == 0);

  // A timer callback that queues an APC (what XTimer does) reaches an alertable waiter.
  auto never = Event::CreateManualResetEvent(false);
  std::atomic<int> wr{-1};
  std::atomic<bool> apc_ran{false};
  auto waiter = Spawn([&] { wr = int(rex::thread::Wait(never.get(), true, 3000ms)); });
  Thread* wt = waiter.get();
  auto apc_timer = Timer::CreateSynchronizationTimer();
  apc_timer->SetOnceAfter(std::chrono::milliseconds(20),
                          [&] { wt->QueueUserCallback([&] { apc_ran = true; }); });
  CHECK(Join(waiter.get(), 3000ms));
  CHECK(wr == int(WaitResult::kUserCallback) && apc_ran);
}

void TestNamesAndIds() {
  std::atomic<bool> ok{false};
  std::unique_ptr<Thread> t;
  auto ready = Event::CreateManualResetEvent(false);
  t = Spawn([&] {
    rex::thread::Wait(ready.get(), false);
    // The RtlRaiseException(0x406D1388) -> SetThreadName -> XThread::set_name path renames the
    // current thread; it must simply return.
    Thread::GetCurrentThread()->set_name("halo_render_thread_with_a_long_name");
    ok = Thread::GetCurrentThread() == t.get() &&
         Thread::GetCurrentThread()->name() == "halo_render_thread_with_a_long_name" &&
         Thread::GetCurrentThread()->system_id() == rex::thread::current_thread_system_id();
  });
  ready->Set();
  CHECK(Join(t.get()));
  CHECK(ok);
  // Main (not created by Thread::Create) gets a wrapper too.
  Thread* main_thread = Thread::GetCurrentThread();
  CHECK(main_thread != nullptr && main_thread == Thread::GetCurrentThread());
  CHECK(main_thread->system_id() == rex::thread::current_thread_system_id());
  main_thread->set_priority(rex::thread::ThreadPriority::kHighest);
  CHECK(main_thread->priority() == rex::thread::ThreadPriority::kHighest);
  main_thread->set_affinity_mask(0x3);
  CHECK(main_thread->affinity_mask() == 0x3);
  auto tls = rex::thread::AllocateTlsHandle();
  CHECK(rex::thread::SetTlsValue(tls, 42) && rex::thread::GetTlsValue(tls) == 42);
  CHECK(rex::thread::FreeTlsHandle(tls));
}

void TestManyThreads() {
  // More threads than a small pool, each with a 16 MiB stack, all blocked then released.
  constexpr int kThreads = 24;
  auto go = Event::CreateManualResetEvent(false);
  auto sem = Semaphore::Create(0, kThreads);
  std::vector<std::unique_ptr<Thread>> ts;
  for (int i = 0; i < kThreads; ++i) {
    ts.push_back(Spawn([&] {
      if (rex::thread::Wait(go.get(), false, 5000ms) == WaitResult::kSuccess) sem->Release(1, nullptr);
    }));
  }
  go->Set();
  int got = 0;
  for (int i = 0; i < kThreads; ++i) {
    if (rex::thread::Wait(sem.get(), false, 5000ms) == WaitResult::kSuccess) ++got;
  }
  CHECK(got == kThreads);
  for (auto& t : ts) CHECK(Join(t.get(), 5000ms));
}

struct TestCase {
  const char* name;
  void (*fn)();
};

}  // namespace

int main() {
  const TestCase tests[] = {
      {"event_wait_timeout", TestEventWaitTimeout},
      {"wait_multiple", TestWaitMultiple},
      {"semaphore_mutant", TestSemaphoreMutant},
      {"signal_and_wait_pulse", TestSignalAndWaitAndPulse},
      {"alertable_apc", TestAlertableApc},
      {"suspend_safe_point", TestSuspendSafePoint},
      {"suspend_in_wait", TestSuspendInWait},
      {"create_suspended_self_suspend", TestCreateSuspendedAndSelfSuspend},
      {"terminate_exit", TestTerminateAndExit},
      {"timers", TestTimers},
      {"names_ids_tls", TestNamesAndIds},
      {"many_threads_16mib", TestManyThreads},
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
