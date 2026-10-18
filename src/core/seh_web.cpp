/**
 * @file        core/seh_web.cpp
 * @brief       Web (Emscripten) SEH backend: no hardware exceptions (PHASE1_PLAN D7).
 *
 * Replaces seh_posix.cpp when REX_PLATFORM_WEB. Wasm has no SIGSEGV/SIGBUS/SIGFPE/SIGILL:
 * folded guest accesses can not leave linear memory, divisions are guarded by the codegen, and
 * the remaining traps (null call_indirect, unreachable, host bugs) are fatal exactly like a
 * native crash. Halo 3 does not catch access violations (docs/port/symbol_recon.md 3b).
 * SEH_TRY/SEH_CATCH stay C++ try/catch (-fwasm-exceptions) for software-raised exceptions;
 * no signal handlers are installed. RaiseException(0x406D1388) (SetThreadName) is handled
 * by RtlRaiseException_entry and returns normally.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/crash_report.h>
#include <rex/platform.h>
#include <rex/ppc/detail/seh.h>

static_assert(REX_PLATFORM_WEB, "This file is web-only (Emscripten)");

#include <cstdio>
#include <cstdlib>

namespace rex::ppc::detail {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(uint32_t /*code*/, void* /*exception_pointers*/) {
  return 0;  // nothing raises host-level structured exceptions on the web
}

[[noreturn]] void seh_rethrow() {
  // Native POSIX re-raises the fault signal and dies; there is no fault to re-raise here.
  char note[160];
  std::snprintf(note, sizeof(note), "unhandled guest exception code=0x%08X info=%llx,%llx",
                tls_seh_state.code, static_cast<unsigned long long>(tls_seh_state.info[0]),
                static_cast<unsigned long long>(tls_seh_state.info[1]));
  std::fprintf(stderr, "[rex::seh/web] %s\n", note);
  rex_web_note_fatal("guest-exception", note);
  std::abort();
}

void seh_initialize() {
  g_seh_initialized.store(true, std::memory_order_relaxed);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::ppc::detail
