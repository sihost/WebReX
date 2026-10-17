/**
 * @file        rex/core/emscripten_shims.cpp
 * @brief       Web (Emscripten) link shims owned by WS-B (build). STUBS.
 *
 * Kept out of the *_web.cpp namespace (WS-C) on purpose; each entry is listed
 * in docs/port/STUBS.md and can be deleted as soon as the real web backend
 * provides the symbol.
 *
 * 1. rex::arch::ExceptionHandler: there are no signals on wasm (a trap is
 *    fatal, D7), so a host fault handler can never run. Install/Uninstall only
 *    log. MMIO is dispatched in software by the PPC_* macros instead.
 * 2. GNU pthread extensions used by threading_posix.cpp (bridge until WS-C's
 *    threading_web.cpp replaces it): Emscripten's libc declares
 *    pthread_setname_np / pthread_{get,set}affinity_np but does not define them.
 *    Weak, so a real libc definition wins.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WEB

#include <cerrno>
#include <cstdio>

#include <pthread.h>
#include <sched.h>

#include <rex/exception_handler.h>

namespace rex::arch {

void ExceptionHandler::Install(Handler fn, void* data) {
  (void)fn;
  (void)data;
  std::fprintf(stderr,
               "[STUB] rex::arch::ExceptionHandler::Install: no host fault handler on web "
               "(traps are fatal, D7; docs/port/STUBS.md)\n");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  (void)fn;
  (void)data;
}

}  // namespace rex::arch

extern "C" {

__attribute__((weak)) int pthread_setname_np(pthread_t, const char*) {
  return 0;  // thread names are cosmetic (debugger/profiler only)
}

__attribute__((weak)) int pthread_getaffinity_np(pthread_t, size_t, cpu_set_t*) {
  return ENOSYS;  // browsers expose no CPU affinity
}

__attribute__((weak)) int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t*) {
  return ENOSYS;
}

}  // extern "C"

#endif  // REX_PLATFORM_WEB
