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
#include <rex/memory/utils.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_INT32(kernel_call_watchdog_ms, 0, "Kernel",
                     "Diagnostics: if > 0, record each guest thread's last kernel export/hook "
                     "call and log all threads (with a guest stack walk) every N ms");
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
  // Natively an unmapped guest page faults the host, so only read readable pages.
  auto read32 = [mem](uint32_t address, uint32_t* out) {
    auto* heap = mem->LookupHeap(address);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(address, &protect) ||
        !(protect & memory::kMemoryProtectRead)) {
      return false;
    }
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
  if (period_ms <= 0 || g_kernel_call_trace) {
    return;
  }
  g_kernel_call_trace = true;
  REXKRNL_WARN("kcall-wd: kernel call trace on, watchdog every {} ms", period_ms);
  std::thread(WatchdogMain, period_ms).detach();
}

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
