/**
 * @file        rex/std_compat.h
 * @brief       Minimal polyfills for C++23/C++20 library features missing from
 *              the libc++ shipped with Emscripten (6.0.12).
 *
 * Only active when the standard library does not provide the feature
 * (feature-test macros), so MSVC STL / libstdc++ / newer libc++ builds are
 * unaffected. The polyfills are declared in namespace std on purpose so that
 * existing `std::move_only_function` / `std::chrono::clock_cast` /
 * `std::chrono::clock_time_conversion` spellings in the SDK keep compiling.
 *
 * - move_only_function<R(Args...)>: only the unqualified call signature
 *   (the only form the SDK uses). No small-buffer optimisation.
 * - clock_time_conversion / clock_cast: identity and user specialisations
 *   only (no implicit hops through system_clock/utc_clock).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <version>

#if !defined(__cpp_lib_move_only_function)
#define REX_STD_COMPAT_MOVE_ONLY_FUNCTION 1
namespace std {

template <class Signature>
class move_only_function;

template <class R, class... Args>
class move_only_function<R(Args...)> {
  struct Base {
    virtual ~Base() = default;
    virtual R Invoke(Args&&... args) = 0;
  };
  template <class F>
  struct Holder final : Base {
    template <class G>
    explicit Holder(G&& g) : f(std::forward<G>(g)) {}
    R Invoke(Args&&... args) override { return std::invoke(f, std::forward<Args>(args)...); }
    F f;
  };

  template <class F>
  static bool IsNullCallable(const F& f) {
    if constexpr (std::is_pointer_v<F> || std::is_member_pointer_v<F>) {
      return f == nullptr;
    } else if constexpr (std::is_same_v<F, std::function<R(Args...)>>) {
      return !static_cast<bool>(f);
    } else {
      return false;
    }
  }

  std::unique_ptr<Base> impl_;

 public:
  using result_type = R;

  move_only_function() noexcept = default;
  move_only_function(std::nullptr_t) noexcept {}
  move_only_function(move_only_function&&) noexcept = default;
  move_only_function(const move_only_function&) = delete;

  template <class F, class D = std::decay_t<F>,
            class = std::enable_if_t<!std::is_same_v<D, move_only_function> &&
                                     !std::is_same_v<D, std::nullptr_t> &&
                                     std::is_invocable_r_v<R, D&, Args...>>>
  move_only_function(F&& f) {
    if (!IsNullCallable(f)) {
      impl_ = std::make_unique<Holder<D>>(std::forward<F>(f));
    }
  }

  move_only_function& operator=(move_only_function&&) noexcept = default;
  move_only_function& operator=(const move_only_function&) = delete;
  move_only_function& operator=(std::nullptr_t) noexcept {
    impl_.reset();
    return *this;
  }
  template <class F, class D = std::decay_t<F>,
            class = std::enable_if_t<!std::is_same_v<D, move_only_function> &&
                                     !std::is_same_v<D, std::nullptr_t> &&
                                     std::is_invocable_r_v<R, D&, Args...>>>
  move_only_function& operator=(F&& f) {
    move_only_function(std::forward<F>(f)).swap(*this);
    return *this;
  }

  ~move_only_function() = default;

  void swap(move_only_function& other) noexcept { impl_.swap(other.impl_); }
  explicit operator bool() const noexcept { return impl_ != nullptr; }

  R operator()(Args... args) { return impl_->Invoke(std::forward<Args>(args)...); }

  friend bool operator==(const move_only_function& f, std::nullptr_t) noexcept { return !f; }
};

}  // namespace std
#endif  // !__cpp_lib_move_only_function

// libc++ (as of the Emscripten 6.0.12 sysroot) does not implement
// [time.clock.cast] at all; libstdc++ and MSVC STL do.
#if defined(_LIBCPP_VERSION) && !defined(REX_STD_COMPAT_HAS_CLOCK_CAST)
#define REX_STD_COMPAT_CLOCK_CAST 1
namespace std::chrono {

template <class DestClock, class SourceClock>
struct clock_time_conversion {};

template <class Clock>
struct clock_time_conversion<Clock, Clock> {
  template <class Duration>
  time_point<Clock, Duration> operator()(const time_point<Clock, Duration>& t) const {
    return t;
  }
};

template <class DestClock, class SourceClock, class Duration>
auto clock_cast(const time_point<SourceClock, Duration>& t)
    -> decltype(clock_time_conversion<DestClock, SourceClock>{}(t)) {
  return clock_time_conversion<DestClock, SourceClock>{}(t);
}

}  // namespace std::chrono
#endif
