/**
 * @file        ppc/detail/fpscr.h
 * @brief       Platform-specific FPSCR constants and intrinsics
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <simde/x86/sse.h>

// SSE3 constants are missing from simde
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif

namespace rex::ppc::detail {

// simde does not handle denormal flags, so we need to implement per-arch.
#if defined(__x86_64__) || defined(_M_X64)

struct FPSCRPlatform {
  static constexpr size_t RoundShift = 13;
  static constexpr size_t RoundMaskVal = SIMDE_MM_ROUND_MASK;
  static constexpr size_t FlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
  // Exception mask bits (1 = exception masked/disabled)
  static constexpr uint32_t ExceptionMask = (1 << 7) |   // IM - Invalid operation
                                            (1 << 8) |   // DM - Denormal operand
                                            (1 << 9) |   // ZM - Zero divide
                                            (1 << 10) |  // OM - Overflow
                                            (1 << 11) |  // UM - Underflow
                                            (1 << 12);   // PM - Precision (Inexact)
  static constexpr size_t GuestToHost[] = {SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO,
                                           SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN};

  static inline uint32_t getcsr() noexcept { return simde_mm_getcsr(); }

  static inline void setcsr(uint32_t csr) noexcept { simde_mm_setcsr(csr); }

  static inline void InitHostExceptions(uint32_t& csr) noexcept {
    csr |= ExceptionMask;  // Set mask bits to disable exceptions
  }
};

#elif defined(__aarch64__) || defined(_M_ARM64)

struct FPSCRPlatform {
  // RMode
  static constexpr size_t RoundShift = 22;
  static constexpr size_t RoundMaskVal = 3 << RoundShift;
  // FZ and FZ16
  static constexpr size_t FlushMask = (1 << 19) | (1 << 24);
  // Nearest, Zero, -Infinity, -Infinity
  static constexpr size_t GuestToHost[] = {0 << RoundShift, 3 << RoundShift, 1 << RoundShift,
                                           2 << RoundShift};
  // Exception enable bits (0 = exception disabled, ARM defaults to 0)
  static constexpr uint32_t ExceptionMask = (1 << 8) |   // IOE - Invalid Operation
                                            (1 << 9) |   // DZE - Division by Zero
                                            (1 << 10) |  // OFE - Overflow
                                            (1 << 11) |  // UFE - Underflow
                                            (1 << 12) |  // IXE - Inexact
                                            (1 << 15);   // IDE - Input Denormal

  static inline uint32_t getcsr() noexcept {
    uint64_t csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return csr;
  }

  static inline void setcsr(uint32_t csr) noexcept {
    __asm__ __volatile__("msr fpcr, %0" : : "r"(csr));
  }

  static inline void InitHostExceptions(uint32_t& csr) noexcept {
    csr &= ~ExceptionMask;  // Clear enable bits to disable exceptions
  }
};

#elif defined(__wasm__)

// WebAssembly has no floating-point control register: rounding is always
// round-to-nearest-even, denormals are never flushed and there are no sticky
// exception flags or traps. The "host csr" is therefore a pure software value
// (laid out like x86 MXCSR so the shared FPSCRRegister bit logic is unchanged)
// that round-trips mtfsf/mffs but has NO effect on arithmetic. Lost semantics
// are documented in docs/port/toolchain.md ("FPSCR on wasm").
struct FPSCRPlatform {
  static constexpr size_t RoundShift = 13;
  static constexpr size_t RoundMaskVal = 3u << RoundShift;
  static constexpr size_t FlushMask = (1u << 15) | (1u << 6);  // FZ | DAZ (ignored)
  static constexpr uint32_t ExceptionMask = 0x3Fu << 7;        // all masked (no traps exist)
  // Nearest, Zero, +Inf, -Inf (MXCSR RC encoding)
  static constexpr size_t GuestToHost[] = {0u << RoundShift, 3u << RoundShift, 2u << RoundShift,
                                           1u << RoundShift};
  static constexpr uint32_t kDefaultCsr = 0x1F80;  // MXCSR reset value: RNE, all masked

  static inline thread_local uint32_t soft_csr_ = kDefaultCsr;

  static inline uint32_t getcsr() noexcept { return soft_csr_; }

  static inline void setcsr(uint32_t csr) noexcept { soft_csr_ = csr; }

  static inline void InitHostExceptions(uint32_t& csr) noexcept { csr |= ExceptionMask; }
};

#else
#error "Missing implementation for FPSCR."
#endif

}  // namespace rex::ppc::detail
