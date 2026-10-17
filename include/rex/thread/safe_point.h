/**
 * @file        rex/thread/safe_point.h
 * @brief       Cooperative suspend/terminate safe point.
 *
 * Native builds deliver Thread::Suspend()/Terminate() asynchronously (RT signals on POSIX,
 * SuspendThread/TerminateThread on Windows), so SafePoint() compiles to nothing there.
 *
 * The web build (Emscripten pthreads) has no signals. A suspend or terminate request is
 * noticed by the target thread:
 *   - immediately, if it is blocked in any rex::thread wait or sleep (it is woken, then parks
 *     inside the wait until resumed, or exits);
 *   - at its next call to SafePoint() or MaybeYield() otherwise.
 * A thread that runs guest code without waiting, sleeping, yielding or reaching a safe point
 * can NOT be suspended or terminated on the web. See docs/port/threading.md (project repo).
 *
 * The fast path is one relaxed load of a process-wide counter that is non-zero only while some
 * running thread has a pending request, so SafePoint() is cheap enough for kernel export entry
 * points and loop back-edges.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/platform.h>

namespace rex::thread {

#if REX_PLATFORM_WEB

namespace detail {
// Number of started, running (not parked, not finished) threads that have a pending suspend or
// terminate request. Maintained by threading_web.cpp under its dispatcher lock.
extern std::atomic<uint32_t> g_safe_point_requests;
// Parks the calling thread while it is suspended; exits it if termination was requested.
void SafePointSlow();
}  // namespace detail

inline void SafePoint() {
  if (detail::g_safe_point_requests.load(std::memory_order_relaxed) != 0) [[unlikely]] {
    detail::SafePointSlow();
  }
}

#else

inline void SafePoint() {}

#endif  // REX_PLATFORM_WEB

}  // namespace rex::thread
