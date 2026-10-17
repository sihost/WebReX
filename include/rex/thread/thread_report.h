/**
 * @file        rex/thread/thread_report.h
 * @brief       Diagnostics: what every thread of the web threading backend is doing.
 *
 * Used when the process is stuck (gate1.mjs --stall calls the rex_debug_dump_threads export
 * from the page): the report must not block, so it reads the per-thread state without the
 * dispatcher lock (racy by design; every field is an atomic).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/platform.h>

namespace rex::thread {

#if REX_PLATFORM_WEB
// One line per thread the web backend knows (rex::thread::Thread::Create threads and threads
// that called into it): pthread tid, system id, name, and what it is blocked in (wait on which
// objects since when, sleep, suspended) or "running". threading_web.cpp.
std::string DescribeThreadsForDiagnostics();
#endif

}  // namespace rex::thread
