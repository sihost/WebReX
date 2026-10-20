/**
 * @file        core/page_lifecycle_web.cpp
 * @brief       See rex/web_page.h. The listeners are in src/ui/rex_web_pre.js.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/web_page.h>

static_assert(REX_PLATFORM_WEB, "This file is web-only (Emscripten)");

#include <emscripten.h>
#include <emscripten/threading.h>

#include <atomic>
#include <climits>
#include <cstdint>
#include <mutex>

namespace {

// [0] = PageStateBits, [1] = hidden transitions. Written by the browser main thread with
// Atomics.store (rex_web_pre.js), read here.
alignas(8) std::atomic<uint32_t> g_page_words[2] = {};
static_assert(sizeof(std::atomic<uint32_t>) == 4);
std::once_flag g_once;

// NotifyAnimationFrame count (futex word).
alignas(4) std::atomic<uint32_t> g_animation_frames{0};

}  // namespace

namespace rex::web {

void InstallPageLifecycle() {
  std::call_once(g_once, [] {
    MAIN_THREAD_ASYNC_EM_ASM(
        {
          if (Module["__rexSetPageStateAddr"]) Module["__rexSetPageStateAddr"]($0);
        },
        static_cast<double>(reinterpret_cast<uintptr_t>(&g_page_words[0])));
  });
}

uint32_t GetPageState() {
  return g_page_words[0].load(std::memory_order_acquire);
}

uint32_t GetPageHiddenCount() {
  return g_page_words[1].load(std::memory_order_relaxed);
}

void NotifyAnimationFrame() {
  g_animation_frames.fetch_add(1, std::memory_order_acq_rel);
  emscripten_futex_wake(&g_animation_frames, INT_MAX);
}

uint32_t GetAnimationFrameCount() {
  return g_animation_frames.load(std::memory_order_acquire);
}

bool WaitForAnimationFrame(uint32_t seen, double timeout_ms) {
  if (g_animation_frames.load(std::memory_order_acquire) != seen) {
    return true;
  }
  if (timeout_ms > 0.0) {
    emscripten_futex_wait(&g_animation_frames, seen, timeout_ms);
  }
  return g_animation_frames.load(std::memory_order_acquire) != seen;
}

}  // namespace rex::web
