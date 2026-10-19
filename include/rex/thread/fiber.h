/**
 * @file        rex/thread/fiber.h
 * @brief       Host OS fiber primitive for cooperative context switching
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <cstddef>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <ucontext.h>
#include <cstdint>
#include <vector>
#endif
#if REX_PLATFORM_WEB
#include <cstdint>
#include <type_traits>
#endif

namespace rex::thread {

#if REX_PLATFORM_WEB
/// Web only. A fiber runs on its own parked pthread (fiber_web.cpp), so thread_local state that
/// belongs to the logical (guest) thread has to follow every SwitchTo: the values of all
/// registered slots are read on the switching pthread and written on the resumed one. Register
/// with REX_FIBER_INHERITED_TLS(var) at namespace scope in the TU that defines `var` (pointer or
/// integer thread_locals). Natively fibers share the thread, so the macro expands to nothing.
bool RegisterFiberInheritedTls(uint64_t (*get)(), void (*set)(uint64_t));

namespace detail {
template <typename T>
uint64_t FiberTlsToBits(T value) {
  if constexpr (std::is_pointer_v<T>) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
  } else {
    return static_cast<uint64_t>(value);
  }
}
template <typename T>
void FiberTlsFromBits(T& slot, uint64_t bits) {
  if constexpr (std::is_pointer_v<T>) {
    slot = reinterpret_cast<T>(static_cast<uintptr_t>(bits));
  } else {
    slot = static_cast<T>(bits);
  }
}
}  // namespace detail

#define REX_FIBER_TLS_CAT2(a, b) a##b
#define REX_FIBER_TLS_CAT(a, b) REX_FIBER_TLS_CAT2(a, b)
#define REX_FIBER_INHERITED_TLS(var)                                                     \
  [[maybe_unused]] static const bool REX_FIBER_TLS_CAT(rex_fiber_tls_, __COUNTER__) =    \
      ::rex::thread::RegisterFiberInheritedTls(                                          \
          []() -> uint64_t { return ::rex::thread::detail::FiberTlsToBits(var); },       \
          [](uint64_t bits) { ::rex::thread::detail::FiberTlsFromBits(var, bits); })
#else
#define REX_FIBER_INHERITED_TLS(var) static_assert(true)
#endif

/// Host OS fiber primitive.
/// Each guest fiber gets one Fiber. Switching preserves the entire C++ call
/// stack, so mid-function resume works without any LR lookup.
struct Fiber {
  /// Convert the calling thread into a fiber.
  /// Must be called once on a thread before any SwitchTo.
  /// Returns a handle for the thread's current execution context.
  static Fiber* ConvertCurrentThread();

  /// Create a new fiber with its own host stack.
  /// entry(arg) is called when the fiber is first switched to.
  static Fiber* Create(size_t stack_size, void (*entry)(void*), void* arg);

  /// Suspend the current fiber and resume target.
  /// Returns when another fiber calls SwitchTo back to this one.
  static void SwitchTo(Fiber* target);

  /// Destroy this fiber. Must not be called while it is executing.
  void Destroy();

  /// Returns the fiber currently executing on this thread, or nullptr.
  static Fiber* Current() { return tls_current_; }

 private:
  static thread_local Fiber* tls_current_;

#if REX_PLATFORM_WIN32
  void* handle_ = nullptr;
  bool is_thread_fiber_ = false;
#elif REX_PLATFORM_LINUX
  ucontext_t context_{};
  std::vector<uint8_t> stack_;
  void (*entry_)(void*) = nullptr;
  void* arg_ = nullptr;
  bool is_thread_fiber_ = false;

  static void Trampoline();
#elif REX_PLATFORM_WEB
  // fiber_web.cpp: a thread fiber stands for the converted pthread itself; a created fiber owns
  // a pthread (started on the first switch) that is parked whenever the fiber is not running.
  struct WebState;
  WebState* web_ = nullptr;
  bool is_thread_fiber_ = false;
#endif
};

}  // namespace rex::thread
