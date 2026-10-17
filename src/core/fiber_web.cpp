/**
 * @file        rex/core/fiber_web.cpp
 * @brief       Web (Emscripten) backend for rex::thread::Fiber: thread fibers only.
 *
 * Decision D6 (docs/port/PHASE1_PLAN.md): no fiber switching and no Asyncify on
 * web. There is no ucontext in Emscripten, and Emscripten fibers would need
 * -sASYNCIFY across ~19.5k generated functions. Switching fibers only backs
 * KeSetCurrentStackPointers, which Halo 3 does not import.
 *
 * What does work is ConvertCurrentThread: XThread::Execute converts every guest
 * thread before calling its entry so a later SwitchTo has a context to return
 * to. Converting needs no context capture here; the handle simply denotes the
 * running thread, and SwitchTo(that handle) from that thread is a no-op, as it
 * is with swapcontext/SwitchToFiber. Create and switching to any other fiber
 * log and abort so a call is loud instead of silently wrong.
 *
 * Listed in docs/port/STUBS.md (owner WS-C).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WEB

#include <rex/thread/fiber.h>

#include <cstdio>
#include <cstdlib>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

[[noreturn]] static void FiberUnsupported(const char* what) {
  std::fprintf(stderr, "[STUB] rex::thread::Fiber::%s called on web: fibers are not supported "
                       "(D6, docs/port/STUBS.md). Aborting.\n",
               what);
  std::abort();
}

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t, void (*)(void*), void*) { FiberUnsupported("Create"); }

void Fiber::SwitchTo(Fiber* target) {
  // Resuming the context that is already running is the only switch possible
  // without saved stacks; anything else needs a second host stack.
  if (target && target == tls_current_) {
    return;
  }
  FiberUnsupported("SwitchTo");
}

void Fiber::Destroy() {
  if (!is_thread_fiber_) {
    FiberUnsupported("Destroy");  // unreachable: Create never returns
  }
  // XThread's destructor may run on another thread; only clear our own slot.
  if (tls_current_ == this) {
    tls_current_ = nullptr;
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_WEB
