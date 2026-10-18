/**
 * @file        rex/crash_report.h
 * @brief       Structured crash report for the web page (Module.rexOnFatal).
 *
 * A trap or abort in a browser tab used to end with "Aborted(...)" in the console and a dead
 * canvas. On the web, every fatal path now ends in one report delivered on the browser main
 * thread to `Module.rexOnFatal(report)` (or, when the page has no such hook, to its
 * `Module.onAbort(text)` and console.error):
 *
 *   { kind, message, note, thread: {tid, guest_id}, threads, stack, log: [..last lines..],
 *     cvars: {name: value, ..modified..}, build: {sdk, compiled}, page: {url, userAgent, ..},
 *     text }   // text = the whole report as copyable plain text
 *
 * Sources: std::abort() on any thread (FatalError, rex_assert_fail, REX_FATAL*, unhandled guest
 * exceptions in seh_rethrow, std::terminate) via the worker's Module.onAbort, wasm traps
 * (unreachable, out-of-bounds, null call_indirect, stack overflow) via the worker's 'error'
 * event, and a window 'error' fallback when a worker died without reaching either. The worker
 * side lives in src/ui/rex_web_pre.js (linked with --pre-js by rexglue_configure_target); it
 * calls the rex_web_crash_collect export, which reads a lock-free ring of the last log lines
 * (no lock a crashed thread could hold) and posts the JSON to the main thread.
 *
 * --web_crash_test=fatal|abort|trap|guest [--web_crash_test_delay_s=N] crashes a helper thread
 * on purpose to try the page's crash UX.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

namespace rex::crash {

#if REX_PLATFORM_WEB
/// Adds the log ring sink (call after InitLogging) and arms --web_crash_test. The web entry
/// point (windowed_app_main_web.cpp) calls it; safe to call more than once.
void InstallCrashReporting();
#else
inline void InstallCrashReporting() {}
#endif

}  // namespace rex::crash

#if REX_PLATFORM_WEB
extern "C" {
/// Records the reason of an imminent abort (kind: "fatal", "assert", "guest-exception") so the
/// crash report carries it as `note`. First call wins. Declared weak in rex/assert.h so code that
/// does not link rexcore still builds.
void rex_web_note_fatal(const char* kind, const char* message);
}
#endif
