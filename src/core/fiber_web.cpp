/**
 * @file        rex/core/fiber_web.cpp
 * @brief       Web (Emscripten) backend for rex::thread::Fiber: NOT SUPPORTED.
 *
 * Decision D6 (docs/port/PHASE1_PLAN.md): no fibers and no Asyncify on web.
 * There is no ucontext in Emscripten, and Emscripten fibers would need
 * -sASYNCIFY across ~19.5k generated functions. Fibers only back
 * KeSetCurrentStackPointers, which Halo 3 is not expected to call. Every entry
 * point logs and aborts so a call is loud instead of silently wrong.
 *
 * Listed in docs/port/STUBS.md (owner WS-C; removal gate: proof that the guest
 * never calls KeSetCurrentStackPointers, or a real backend).
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

Fiber* Fiber::ConvertCurrentThread() { FiberUnsupported("ConvertCurrentThread"); }

Fiber* Fiber::Create(size_t, void (*)(void*), void*) { FiberUnsupported("Create"); }

void Fiber::SwitchTo(Fiber*) { FiberUnsupported("SwitchTo"); }

void Fiber::Destroy() { FiberUnsupported("Destroy"); }

}  // namespace rex::thread

#endif  // REX_PLATFORM_WEB
