/**
 * @file        system/memory_fold_web.cpp
 * @brief       Out-of-line part of the guest address fold (rare 0x7F path, dirty map)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     See include/rex/ppc/fold_web.h. Compiled on every platform (it is small and
 *              platform independent) so the native unit tests exercise the exact code the
 *              web build runs; only REX_PLATFORM_WEB builds route guest accesses through it.
 */

#include <algorithm>

#include <rex/ppc/fold_web.h>
#include <rex/system/mmio_handler.h>

namespace rex::fold {

alignas(64) uint8_t g_phys_dirty[kDirtyPageCount];

namespace {

inline runtime::MMIOHandler* Handler() {
  return runtime::MMIOHandler::global_handler();
}

// Dword read from a registered MMIO window. Returns false if the address is not in one.
inline bool MmioLoad(uint32_t a, uint32_t* out) {
  auto* h = Handler();
  return h && h->CheckLoad(a, out);
}

inline bool MmioStore(uint32_t a, uint32_t v) {
  auto* h = Handler();
  return h && h->CheckStore(a, v);
}

template <typename T>
inline T AliasLoad(const uint8_t* base, uint32_t a) {
  return *reinterpret_cast<const volatile T*>(base + Fold(a));
}

template <typename T>
inline void AliasStore(uint8_t* base, uint32_t a, T v) {
  const uint64_t o = Fold(a);
  *reinterpret_cast<volatile T*>(base + o) = v;
  MarkDirtyHostOffset(o);
}

}  // namespace

uint8_t RareLoad8(const uint8_t* base, uint32_t a) {
  uint32_t v;
  if (MmioLoad(a, &v)) {
    return static_cast<uint8_t>(v);
  }
  return AliasLoad<uint8_t>(base, a);
}

uint16_t RareLoad16(const uint8_t* base, uint32_t a) {
  uint32_t v;
  if (MmioLoad(a, &v)) {
    return static_cast<uint16_t>(v);
  }
  return __builtin_bswap16(AliasLoad<uint16_t>(base, a));
}

uint32_t RareLoad32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  if (MmioLoad(a, &v)) {
    return v;
  }
  return __builtin_bswap32(AliasLoad<uint32_t>(base, a));
}

uint64_t RareLoad64(const uint8_t* base, uint32_t a) {
  uint32_t hi;
  if (MmioLoad(a, &hi)) {
    uint32_t lo = 0;
    MmioLoad(a + 4, &lo);
    return (uint64_t(hi) << 32) | lo;
  }
  return __builtin_bswap64(AliasLoad<uint64_t>(base, a));
}

void RareStore8(uint8_t* base, uint32_t a, uint8_t v) {
  if (MmioStore(a, v)) {
    return;
  }
  AliasStore<uint8_t>(base, a, v);
}

void RareStore16(uint8_t* base, uint32_t a, uint16_t v) {
  if (MmioStore(a, v)) {
    return;
  }
  AliasStore<uint16_t>(base, a, __builtin_bswap16(v));
}

void RareStore32(uint8_t* base, uint32_t a, uint32_t v) {
  if (MmioStore(a, v)) {
    return;
  }
  AliasStore<uint32_t>(base, a, __builtin_bswap32(v));
}

void RareStore64(uint8_t* base, uint32_t a, uint64_t v) {
  if (MmioStore(a, uint32_t(v >> 32))) {
    MmioStore(a + 4, uint32_t(v));
    return;
  }
  AliasStore<uint64_t>(base, a, __builtin_bswap64(v));
}

void MarkPhysicalRangeDirty(uint32_t physical_address, uint32_t length) noexcept {
  if (!length) {
    return;
  }
  const uint64_t first = uint64_t(physical_address) >> kDirtyPageShift;
  if (first >= kDirtyPageCount) {
    return;
  }
  const uint64_t last = std::min<uint64_t>((uint64_t(physical_address) + length - 1) >> kDirtyPageShift,
                                           kDirtyPageCount - 1);
  for (uint64_t i = first; i <= last; ++i) {
    g_phys_dirty[i] = 1;
  }
}

void MarkGuestRangeDirty(uint32_t guest_address, uint32_t length) noexcept {
  uint64_t a = guest_address;
  uint64_t remaining = length;
  while (remaining) {
    // Each top byte is folded with one delta, so split at 16 MiB boundaries.
    const uint64_t segment_end = (a | 0xFFFFFFull) + 1;
    const uint64_t n = std::min<uint64_t>(remaining, segment_end - a);
    const uint64_t o = Fold(uint32_t(a));
    if (IsPhysicalOffset(o)) {
      MarkPhysicalRangeDirty(uint32_t(o - kPhysOffset), uint32_t(n));
    }
    a += n;
    remaining -= n;
    if (a > 0xFFFFFFFFull) {
      break;
    }
  }
}

namespace {
// 8 dirty bytes at an 8-aligned page index (the map is 64-byte aligned). Racy plain read like
// the byte reads: a page can only become dirty concurrently, never clean.
[[gnu::always_inline]] inline uint64_t DirtyWord(uint32_t i) {
  uint64_t word;
  __builtin_memcpy(&word, &g_phys_dirty[i], sizeof(word));
  return word;
}
}  // namespace

bool AnyDirtyPages(uint32_t first_page, uint32_t last_page) noexcept {
  if (first_page >= kDirtyPageCount) {
    return false;
  }
  last_page = std::min<uint32_t>(last_page, uint32_t(kDirtyPageCount - 1));
  uint32_t i = first_page;
  for (; i <= last_page && (i & 7); ++i) {
    if (g_phys_dirty[i]) {
      return true;
    }
  }
  for (; last_page >= 7 && i <= last_page - 7; i += 8) {
    if (DirtyWord(i)) {
      return true;
    }
  }
  for (; i <= last_page; ++i) {
    if (g_phys_dirty[i]) {
      return true;
    }
  }
  return false;
}

uint32_t ConsumeDirtyPages(uint32_t first_page, uint32_t last_page, DirtyRunFn fn,
                           void* context) noexcept {
  if (first_page >= kDirtyPageCount) {
    return 0;
  }
  last_page = std::min<uint32_t>(last_page, uint32_t(kDirtyPageCount - 1));
  uint32_t total = 0;
  uint32_t run_first = UINT32_MAX;
  for (uint32_t i = first_page; i <= last_page; ++i) {
    // Skip 8 clean pages at once (textures and buffers span up to thousands of pages).
    while (!(i & 7) && last_page >= 7 && i <= last_page - 7 && !DirtyWord(i)) {
      if (run_first != UINT32_MAX) {
        if (fn) {
          fn(context, run_first, i - run_first);
        }
        run_first = UINT32_MAX;
      }
      i += 8;
    }
    if (i > last_page) {
      break;
    }
    // Cheap plain read first; only exchange pages that look dirty.
    bool dirty = false;
    if (g_phys_dirty[i]) {
      dirty = __atomic_exchange_n(&g_phys_dirty[i], uint8_t(0), __ATOMIC_ACQ_REL) != 0;
    }
    if (dirty) {
      ++total;
      if (run_first == UINT32_MAX) {
        run_first = i;
      }
    } else if (run_first != UINT32_MAX) {
      if (fn) {
        fn(context, run_first, i - run_first);
      }
      run_first = UINT32_MAX;
    }
  }
  if (run_first != UINT32_MAX && fn) {
    fn(context, run_first, last_page + 1 - run_first);
  }
  return total;
}

void ClearAllDirty() noexcept {
  for (size_t i = 0; i < kDirtyPageCount; ++i) {
    __atomic_store_n(&g_phys_dirty[i], uint8_t(0), __ATOMIC_RELAXED);
  }
}

}  // namespace rex::fold
