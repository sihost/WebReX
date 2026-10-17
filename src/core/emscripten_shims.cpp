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
 *
 * The GNU pthread extension shims (pthread_setname_np, pthread_{get,set}affinity_np)
 * were removed for gate 2: threading_web.cpp replaced threading_posix.cpp on web
 * and nothing else in the web link uses them.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WEB

#include <cstdio>

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

#endif  // REX_PLATFORM_WEB
