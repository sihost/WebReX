/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <cstring>

#include <rex/memory/host_write_web.h>
#include <rex/ppc/function.h>

namespace rex::kernel::crt {

// Web: these host writes replace guest stores, so they mark the GPU dirty map themselves
// (after the data, see rex/ppc/fold_web.h RawStoreW). Natively NoteHostWrite is a no-op.
static void* Noted(void* dst, size_t n) {
  memory::NoteHostWrite(dst, n);
  return dst;
}

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

static void* native_memcpy(void* dst, const void* src, size_t n) {
  return Noted(std::memcpy(dst, src, n), n);
}

static void* native_memmove(void* dst, const void* src, size_t n) {
  return Noted(std::memmove(dst, src, n), n);
}

static void* native_memset(void* dst, int val, size_t n) {
  return Noted(std::memset(dst, val, n), n);
}

static void* native_memchr(const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

static void* native_XMemCpy(void* dst, const void* src, size_t n) {
  return Noted(std::memcpy(dst, src, n), n);
}

static void* native_XMemSet(void* dst, int val, size_t n) {
  return Noted(std::memset(dst, val, n), n);
}

static void* native_XMemSet128(void* dst, int val, size_t n) {
  return Noted(std::memset(dst, val, n), n);
}

static void* native_memset_vmx(void* dst, int val, size_t n) {
  return Noted(std::memset(dst, val, n), n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  Noted(std::memcpy(dst, src, count), count);
  return 0;
}

static int native_memmove_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  Noted(std::memmove(dst, src, count), count);
  return 0;
}

}  // namespace rex::kernel::crt

REXCRT_EXPORT(rexcrt_memcpy, rex::kernel::crt::native_memcpy)
REXCRT_EXPORT(rexcrt_memmove, rex::kernel::crt::native_memmove)
REXCRT_EXPORT(rexcrt_memset, rex::kernel::crt::native_memset)
REXCRT_EXPORT(rexcrt_memchr, rex::kernel::crt::native_memchr)
REXCRT_EXPORT(rexcrt_XMemCpy, rex::kernel::crt::native_XMemCpy)
REXCRT_EXPORT(rexcrt_XMemSet, rex::kernel::crt::native_XMemSet)
REXCRT_EXPORT(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
REXCRT_EXPORT(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
REXCRT_EXPORT(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REXCRT_EXPORT(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
