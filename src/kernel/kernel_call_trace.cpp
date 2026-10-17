/**
 * @file        kernel/kernel_call_trace.cpp
 * @brief       Diagnostics: per-guest-thread record of the last kernel export /
 *              PPC_HOOK call and a watchdog that logs all of them periodically.
 *
 * Off by default. `--kernel_call_watchdog_ms=N` (N > 0) turns on recording in
 * every PPC_HOOK thunk and logs, every N ms, each guest thread's call count,
 * whether it is currently inside the call (blocked in a wait, for example), the
 * export name, its guest return address and a short guest back-chain walk. Used
 * to find where a guest thread stalls and to compare call sequences with native.
 * `--kernel_call_trace` records without the watchdog thread (no periodic logging).
 *
 * Web: the `rex_debug_dump_threads` export (called from the page, e.g. by
 * server/tools/gate1.mjs --stall) prints the owner of the global critical region
 * (pthread tid), every rex::thread thread's state (rex/thread/thread_report.h) and
 * the recorded kernel calls, straight to console.log from the calling (browser main)
 * thread without taking any lock a stuck thread could hold.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/memory/utils.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread/mutex.h>

#if REX_PLATFORM_WEB
#include <emscripten/console.h>
#include <emscripten/emscripten.h>
#include <pthread.h>
#include <unistd.h>

#include <rex/thread/thread_report.h>
#endif

REXCVAR_DEFINE_INT32(kernel_call_watchdog_ms, 0, "Kernel",
                     "Diagnostics: if > 0, record each guest thread's last kernel export/hook "
                     "call and log all threads (with a guest stack walk) every N ms");
REXCVAR_DEFINE_BOOL(kernel_call_trace, false, "Kernel",
                    "Diagnostics: record each guest thread's last kernel export/hook call (as "
                    "kernel_call_watchdog_ms) without the periodic watchdog log; read on demand "
                    "(web: the rex_debug_dump_threads export)");
REXCVAR_DEFINE_STRING(guest_trace_trigger, "", "Kernel",
                      "Diagnostics (recompiled code built with -DREX_GUEST_TRACE=1): guest "
                      "function whose entry starts recording guest function entries on that "
                      "thread, e.g. rex_hs_runtime_update");
REXCVAR_DEFINE_INT32(guest_trace_count, 200000, "Kernel",
                     "Diagnostics: guest function entries to record after the trigger, then log");
REXCVAR_DEFINE_INT32(guest_trace_skip, 0, "Kernel",
                     "Diagnostics: trigger hits to skip before recording");

namespace rex::detail {

bool g_kernel_call_trace = false;

namespace {

struct Record {
  std::atomic<const char*> name{nullptr};
  std::atomic<uint64_t> count{0};
  std::atomic<uint32_t> lr{0}, r1{0}, r3{0}, r4{0}, r5{0};
  std::atomic<bool> inside{false};
  uint32_t thread_id = 0;
  int tid = 0;  // web: pthread tid (matches rex_debug_dump_threads' thread list)
  std::string thread_name;
  uint64_t last_logged_count = 0;  // watchdog thread only
};

std::mutex g_records_mutex;
std::vector<Record*> g_records;  // never freed: threads may still write after exit
thread_local Record* t_record = nullptr;

bool IsStackAddress(uint32_t a) { return a >= 0x70000000u && a < 0x80000000u; }

std::string WalkGuestStack(uint32_t sp) {
  std::string out;
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory() || !IsStackAddress(sp)) {
    return out;
  }
  auto* mem = ks->memory();
  // Natively an unmapped guest page faults the host, so only read readable pages. On the web
  // nothing faults (the stack range is in linear memory), and QueryProtect takes the global
  // critical region, which a stuck thread may hold (rex_debug_dump_threads must not block).
  auto read32 = [mem](uint32_t address, uint32_t* out) {
#if !REX_PLATFORM_WEB
    auto* heap = mem->LookupHeap(address);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(address, &protect) ||
        !(protect & memory::kMemoryProtectRead)) {
      return false;
    }
#endif
    *out = memory::load_and_swap<uint32_t>(mem->TranslateVirtual(address));
    return true;
  };
  for (int depth = 0; depth < 10; ++depth) {
    uint32_t bc = 0, lr = 0;
    if (!read32(sp, &bc) || !IsStackAddress(bc) || bc <= sp || bc - sp > 0x100000u ||
        !read32(bc - 8, &lr)) {
      break;
    }
    out += fmt::format(" {:08X}", lr);
    sp = bc;
  }
  return out;
}

void WatchdogMain(int period_ms) {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
    std::vector<Record*> records;
    {
      std::lock_guard<std::mutex> lock(g_records_mutex);
      records = g_records;
    }
    REXKRNL_WARN("kcall-wd: {} guest threads", records.size());
    for (Record* r : records) {
      const uint64_t count = r->count.load(std::memory_order_relaxed);
      const char* name = r->name.load(std::memory_order_relaxed);
      const bool inside = r->inside.load(std::memory_order_acquire);
      const uint32_t r1 = r->r1.load(std::memory_order_relaxed);
      REXKRNL_WARN(
          "kcall-wd: thid {} '{}' calls={} (+{}) {} {} lr={:08X} r3={:08X} r4={:08X} r5={:08X} "
          "sp={:08X} stack:{}",
          r->thread_id, r->thread_name, count, count - r->last_logged_count,
          inside ? "IN" : "after", name ? name : "?", r->lr.load(std::memory_order_relaxed),
          r->r3.load(std::memory_order_relaxed), r->r4.load(std::memory_order_relaxed),
          r->r5.load(std::memory_order_relaxed), r1, WalkGuestStack(r1));
      r->last_logged_count = count;
    }
  }
}

}  // namespace

void KernelCallTraceEnter(const char* name, const PPCContext& ctx) {
  Record* r = t_record;
  if (!r) {
    r = new Record();
#if REX_PLATFORM_WEB
    r->tid = gettid();
#endif
    if (rex::system::XThread::IsInThread()) {
      auto* thread = rex::system::XThread::GetCurrentThread();
      r->thread_id = thread->thread_id();
      r->thread_name = thread->name();
    }
    {
      std::lock_guard<std::mutex> lock(g_records_mutex);
      g_records.push_back(r);
    }
    t_record = r;
  }
  r->name.store(name, std::memory_order_relaxed);
  r->lr.store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  r->r1.store(ctx.r1.u32, std::memory_order_relaxed);
  r->r3.store(ctx.r3.u32, std::memory_order_relaxed);
  r->r4.store(ctx.r4.u32, std::memory_order_relaxed);
  r->r5.store(ctx.r5.u32, std::memory_order_relaxed);
  r->count.fetch_add(1, std::memory_order_relaxed);
  r->inside.store(true, std::memory_order_release);
}

void KernelCallTraceExit() {
  if (t_record) {
    t_record->inside.store(false, std::memory_order_release);
  }
}

void StartKernelCallTrace() {
  const int period_ms = REXCVAR_GET(kernel_call_watchdog_ms);
  if ((period_ms <= 0 && !REXCVAR_GET(kernel_call_trace)) || g_kernel_call_trace) {
    return;
  }
  g_kernel_call_trace = true;
  if (period_ms <= 0) {
    REXKRNL_WARN("kcall-wd: kernel call trace on (no watchdog)");
    return;
  }
  REXKRNL_WARN("kcall-wd: kernel call trace on, watchdog every {} ms", period_ms);
  std::thread(WatchdogMain, period_ms).detach();
}

#if REX_PLATFORM_WEB
namespace {
// musl pthread_mutex_t: __u.__vi[1] = _m_lock (owner tid in the low 30 bits, bit 31 = waiters),
// __u.__i[5] = _m_count (recursion count - 1 for a recursive mutex).
std::string DescribeMutex(const char* what, std::recursive_mutex& mutex) {
  const volatile int* words = reinterpret_cast<const volatile int*>(mutex.native_handle());
  const int lock = words[1];
  const int count = words[5];
  return fmt::format("{}: _m_lock={:#010x} -> {} (owner tid {}{}), _m_count={}\n", what,
                     uint32_t(lock), lock & 0x3fffffff ? "HELD" : "free", lock & 0x3fffffff,
                     lock < 0 ? ", waiters" : "", count);
}
}  // namespace
#endif

}  // namespace rex::detail

#if REX_PLATFORM_WEB
// Diagnostics export (see the file comment). Each line goes to console.log separately.
extern "C" EMSCRIPTEN_KEEPALIVE void rex_debug_dump_threads() {
  using namespace rex::detail;
  std::string out = fmt::format("rex_debug_dump_threads (called on tid {})\n", gettid());
  out += DescribeMutex("global critical region", rex::thread::global_critical_region::mutex());
  out += rex::thread::DescribeThreadsForDiagnostics();
  if (!g_kernel_call_trace) {
    out += "kernel calls: not recorded (--kernel_call_trace or --kernel_call_watchdog_ms)\n";
  } else {
    std::vector<Record*> records;
    if (g_records_mutex.try_lock()) {
      records = g_records;
      g_records_mutex.unlock();
    }
    out += fmt::format("kernel calls: {} guest threads\n", records.size());
    for (Record* r : records) {
      const char* name = r->name.load(std::memory_order_relaxed);
      const uint32_t r1 = r->r1.load(std::memory_order_relaxed);
      out += fmt::format(
          "  tid {} thid {} '{}' calls={} {} {} lr={:08X} r3={:08X} r4={:08X} r5={:08X} "
          "sp={:08X} stack:{}\n",
          r->tid, r->thread_id, r->thread_name, r->count.load(std::memory_order_relaxed),
          r->inside.load(std::memory_order_acquire) ? "IN" : "after", name ? name : "?",
          r->lr.load(std::memory_order_relaxed), r->r3.load(std::memory_order_relaxed),
          r->r4.load(std::memory_order_relaxed), r->r5.load(std::memory_order_relaxed), r1,
          WalkGuestStack(r1));
    }
  }
  size_t start = 0;
  while (start < out.size()) {
    size_t end = out.find('\n', start);
    if (end == std::string::npos) {
      end = out.size();
    }
    emscripten_out(out.substr(start, end - start).c_str());
    start = end + 1;
  }
}
#endif

namespace rex::detail {

//=============================================================================
// Guest function entry trace (only with recompiled code built -DREX_GUEST_TRACE=1)
//=============================================================================

namespace {
struct GuestTraceState {
  int state = 0;  // 0 waiting for trigger, 1 recording, 2 done
  int triggers_seen = 0;
  std::vector<const char*> names;
};
thread_local GuestTraceState t_guest_trace;

void GuestTraceDump(const std::vector<const char*>& names) {
  const uint32_t thid = rex::system::XThread::IsInThread()
                            ? rex::system::XThread::GetCurrentThread()->thread_id()
                            : 0;
  REXKRNL_WARN("gtrace: thid {} begin, {} entries after trigger '{}'", thid, names.size(),
               REXCVAR_GET(guest_trace_trigger));
  std::string line;
  for (size_t i = 0; i < names.size(); ++i) {
    const char* n = names[i];
    if (std::strncmp(n, "__imp__", 7) == 0) n += 7;
    line += n;
    line += ' ';
    if ((i + 1) % 48 == 0 || i + 1 == names.size()) {
      REXKRNL_WARN("gtrace: {} {}", i / 48, line);
      line.clear();
    }
  }
  REXKRNL_WARN("gtrace: thid {} end", thid);
}
}  // namespace

bool GuestTraceIsTrigger(const char* name) {
  const std::string& trigger = REXCVAR_GET(guest_trace_trigger);
  if (trigger.empty()) return false;
  const char* n = std::strncmp(name, "__imp__", 7) == 0 ? name + 7 : name;
  return trigger == name || trigger == n;
}

void GuestTraceEnter(const char* name, bool is_trigger) {
  GuestTraceState& t = t_guest_trace;
  if (t.state == 2) [[likely]] {
    return;
  }
  if (t.state == 0) {
    if (!is_trigger) [[likely]] {
      return;
    }
    if (t.triggers_seen++ < REXCVAR_GET(guest_trace_skip)) {
      return;
    }
    t.state = 1;
    t.names.reserve(size_t(std::max(1, REXCVAR_GET(guest_trace_count))));
  }
  t.names.push_back(name);
  if (t.names.size() >= size_t(std::max(1, REXCVAR_GET(guest_trace_count)))) {
    t.state = 2;
    GuestTraceDump(t.names);
    std::vector<const char*>().swap(t.names);
  }
}

}  // namespace rex::detail
