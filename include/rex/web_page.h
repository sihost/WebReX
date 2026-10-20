/**
 * @file        rex/web_page.h
 * @brief       Browser page lifecycle as seen from any thread (hidden, frozen, discarded).
 *
 * src/ui/rex_web_pre.js listens on the browser main thread for visibilitychange, freeze,
 * resume, pagehide and pageshow and stores the state into a shared word read here without a
 * lock. It also tells the page about each transition (Module.rexOnLifecycle(state, info)) and,
 * on a load that follows a Memory Saver / tab discard (document.wasDiscarded), calls
 * Module.rexOnDiscarded() so the page can explain that the browser threw the session away.
 *
 * Natively the page is always "visible".
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>

namespace rex::web {

enum PageStateBits : uint32_t {
  kPageHidden = 1u << 0,  // document.visibilityState == "hidden"
  kPageFrozen = 1u << 1,  // between 'freeze' and 'resume' (no task runs on the main thread)
};

#if REX_PLATFORM_WEB
/// Publishes the state word to the page script (call once from any thread; the web entry
/// point does). Until then the state reads as visible.
void InstallPageLifecycle();
/// PageStateBits of the page, lock-free.
uint32_t GetPageState();
/// Number of visible -> hidden transitions so far.
uint32_t GetPageHiddenCount();
/// Browser animation frames (requestAnimationFrame callbacks of the WebGPU presenter on the
/// device thread). They are the only precise periodic wake-up a page gets: timed waits
/// (Atomics.wait with a timeout, setTimeout) in Workers follow the Windows timer resolution and
/// often take 15.6 ms instead of 1 ms. NotifyAnimationFrame wakes WaitForAnimationFrame callers.
void NotifyAnimationFrame();
uint32_t GetAnimationFrameCount();
/// Blocks until the count is no longer `seen` or `timeout_ms` passed; true if it changed.
bool WaitForAnimationFrame(uint32_t seen, double timeout_ms);
#else
inline void InstallPageLifecycle() {}
inline uint32_t GetPageState() {
  return 0;
}
inline uint32_t GetPageHiddenCount() {
  return 0;
}
inline void NotifyAnimationFrame() {}
inline uint32_t GetAnimationFrameCount() {
  return 0;
}
inline bool WaitForAnimationFrame(uint32_t, double) {
  return false;
}
#endif

inline bool IsPageHidden() {
  return (GetPageState() & (kPageHidden | kPageFrozen)) != 0;
}

}  // namespace rex::web
