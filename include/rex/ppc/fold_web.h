/**
 * @file        ppc/fold_web.h
 * @brief       Guest address folding for hosts without page aliasing (web / wasm64)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Design: halo3 web port docs/port/memory.md sections 3, 4, 6A, 7 (Layout B).
 *
 * Natively the SDK maps the guest address space with aliasing file views
 * (src/system/xmemory.cpp map_info): the 0x9xxxxxxx xex view aliases 0x8xxxxxxx and the
 * 0x7F/0xA0/0xC0/0xE0 views alias the 512 MiB physical block. wasm linear memory cannot
 * alias pages, so on REX_PLATFORM_WEB every guest address is folded onto one canonical
 * host offset inside a single 0xB0001000-byte guest window ("Layout B"):
 *
 *   guest top byte | host offset (relative to the window base)
 *   0x00 - 0x7E    | a
 *   0x7F           | kPhysOffset + (a & 0x00FFFFFF)        (rare path: MMIO windows first)
 *   0x80 - 0x8F    | a
 *   0x90 - 0x9F    | a - 0x10000000                         (alias of 0x8x)
 *   0xA0 - 0xBF    | kPhysOffset + (a & 0x1FFFFFFF)
 *   0xC0 - 0xDF    | kPhysOffset + (a & 0x1FFFFFFF)
 *   0xE0 - 0xFF    | kPhysOffset + (a & 0x1FFFFFFF) + 0x1000 (GetPhysicalAddress +4 KiB)
 *
 * host = base + a + kFold.d[a >> 24]. The table is constexpr so accesses through
 * compile-time-constant addresses fold completely.
 *
 * This header is platform independent on purpose: the native unit tests compile the same
 * table and runtime (src/system/memory_fold_web.cpp) and check them against map_info. Only
 * include/rex/ppc/memory.h and src/system/xmemory.cpp select it, under REX_PLATFORM_WEB.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::fold {

//=============================================================================
// Layout B constants
//=============================================================================

// Host offset of guest physical address 0 inside the guest window.
inline constexpr uint64_t kPhysOffset = 0x90000000ull;
// Size of the guest physical block (0xA0/0xC0/0xE0 views and the raw physical heap).
inline constexpr uint64_t kPhysSize = 0x20000000ull;
// Highest folded offset is Fold(0xFFFFFFFF) = 0xB0000FFF, so the window is 0xB0001000 bytes.
inline constexpr uint64_t kWindowSize = kPhysOffset + kPhysSize + 0x1000ull;
// Slack after the window for accesses that straddle its end (dcbz128 / 16-byte vectors).
inline constexpr uint64_t kWindowSlack = 0x10000ull;
// Alignment of the window base (the SDK itself only needs 32 bytes, see PPC_FUNC_PROLOGUE).
inline constexpr uint64_t kWindowAlignment = 0x10000ull;

// Dirty map: one byte per 4 KiB of physical memory, indexed by guest physical address >> 12.
// 0x20001 entries: physical 0..0x1FFFFFFF plus the page reached by the 0xE0 view's +0x1000.
inline constexpr uint32_t kDirtyPageShift = 12;
inline constexpr uint32_t kDirtyPageSize = 1u << kDirtyPageShift;
inline constexpr size_t kDirtyPageCount = size_t((kPhysSize + 0x1000ull) >> kDirtyPageShift);

// Top byte routed out of line (MMIO windows + GPU writeback / XPS alias of physical 0-16 MiB).
inline constexpr uint32_t kRareTopByte = 0x7Fu;

//=============================================================================
// Fold table
//=============================================================================

struct FoldTable {
  int64_t d[256];
  constexpr explicit FoldTable(uint64_t phys_offset) : d{} {
    const int64_t p = static_cast<int64_t>(phys_offset);
    for (int i = 0; i < 256; ++i) {
      int64_t v = 0;
      if (i == 0x7F) {
        v = p - 0x7F000000ll;  // 0x7F -> physical 0..16 MiB
      } else if (i >= 0x90 && i < 0xA0) {
        v = -0x10000000ll;  // 0x9x xex 4K view -> 0x8x
      } else if (i >= 0xA0 && i < 0xC0) {
        v = p - 0xA0000000ll;  // physical 64K view
      } else if (i >= 0xC0 && i < 0xE0) {
        v = p - 0xC0000000ll;  // physical 16M view
      } else if (i >= 0xE0) {
        v = p - 0xE0000000ll + 0x1000ll;  // physical 4K view (+4 KiB, xmemory.cpp map_info)
      }
      d[i] = v;
    }
  }
};

alignas(64) inline constexpr FoldTable kFold{kPhysOffset};

// Folds a guest address with an arbitrary table (tests use the native layout A table too).
constexpr uint64_t FoldWith(const FoldTable& table, uint32_t guest_address) noexcept {
  return uint64_t(guest_address) + uint64_t(table.d[guest_address >> 24]);
}

// Host offset (relative to the window base) of a guest virtual address, Layout B.
[[gnu::always_inline]] constexpr uint64_t Fold(uint32_t guest_address) noexcept {
  return uint64_t(guest_address) + uint64_t(kFold.d[guest_address >> 24]);
}

[[gnu::always_inline]] constexpr bool IsRare(uint32_t guest_address) noexcept {
  return (guest_address >> 24) == kRareTopByte;
}

// True if the folded host offset lies in the physical block.
[[gnu::always_inline]] constexpr bool IsPhysicalOffset(uint64_t host_offset) noexcept {
  return host_offset >= kPhysOffset;
}

//=============================================================================
// Dirty map (write-watch replacement, memory.md section 7)
//=============================================================================
// Set by every guest store into physical memory (store macros, PPC_RAW_ADDR_W) and by host
// writers through Memory::MarkPhysicalDirty. Cleared by Memory::ConsumePhysicalDirty (GPU).
// The CPU side uses plain byte stores: racy but benign (a page can only become dirty there;
// the consumer clears with an atomic exchange before it reads the data).
extern uint8_t g_phys_dirty[kDirtyPageCount];

[[gnu::always_inline]] inline void MarkDirtyHostOffset(uint64_t host_offset) noexcept {
  if (IsPhysicalOffset(host_offset)) {
    g_phys_dirty[(host_offset - kPhysOffset) >> kDirtyPageShift] = 1;
  }
}

// Marks [physical_address, physical_address + length) dirty (guest physical addresses,
// 0..0x20000FFF). Safe for any length; clamps to the map.
void MarkPhysicalRangeDirty(uint32_t physical_address, uint32_t length) noexcept;

// Marks the physical pages behind a guest *virtual* range dirty (splits at 16 MiB view
// boundaries; virtual-only parts are ignored).
void MarkGuestRangeDirty(uint32_t guest_address, uint32_t length) noexcept;

// Atomically clears the dirty bytes of physical page indices [first_page, last_page] and calls
// fn(context, run_first_page, run_page_count) for each maximal run that was dirty.
// Returns the number of dirty pages found.
using DirtyRunFn = void (*)(void* context, uint32_t first_page, uint32_t page_count);
// True if any page of [first_page, last_page] is dirty (lock-free, 8 pages per load). A page
// that becomes dirty concurrently may be missed, as if written just after the check.
bool AnyDirtyPages(uint32_t first_page, uint32_t last_page) noexcept;
uint32_t ConsumeDirtyPages(uint32_t first_page, uint32_t last_page, DirtyRunFn fn,
                           void* context) noexcept;

// Test/diagnostic helper: clears the whole map.
void ClearAllDirty() noexcept;

//=============================================================================
// Out-of-line rare path (top byte 0x7F)
//=============================================================================
// Registered MMIO window (MMIOHandler::CheckLoad/CheckStore) -> handler, with exactly the
// semantics of the native PPC_MM_* macros (8/16-bit: truncated register value, 64-bit:
// two dword accesses, high word first). Otherwise the physical alias (+ dirty mark on store).
// Values are host-endian (the byte swap is done here).

[[gnu::noinline, gnu::cold]] uint8_t RareLoad8(const uint8_t* base, uint32_t guest_address);
[[gnu::noinline, gnu::cold]] uint16_t RareLoad16(const uint8_t* base, uint32_t guest_address);
[[gnu::noinline, gnu::cold]] uint32_t RareLoad32(const uint8_t* base, uint32_t guest_address);
[[gnu::noinline, gnu::cold]] uint64_t RareLoad64(const uint8_t* base, uint32_t guest_address);
[[gnu::noinline, gnu::cold]] void RareStore8(uint8_t* base, uint32_t guest_address, uint8_t v);
[[gnu::noinline, gnu::cold]] void RareStore16(uint8_t* base, uint32_t guest_address, uint16_t v);
[[gnu::noinline, gnu::cold]] void RareStore32(uint8_t* base, uint32_t guest_address, uint32_t v);
[[gnu::noinline, gnu::cold]] void RareStore64(uint8_t* base, uint32_t guest_address, uint64_t v);

//=============================================================================
// Inline access helpers used by the web branch of include/rex/ppc/memory.h
//=============================================================================

[[gnu::always_inline]] inline uint8_t Load8(const uint8_t* base, uint32_t a) {
  if (__builtin_expect(IsRare(a), 0)) {
    return RareLoad8(base, a);
  }
  return *reinterpret_cast<const volatile uint8_t*>(base + Fold(a));
}

[[gnu::always_inline]] inline uint16_t Load16(const uint8_t* base, uint32_t a) {
  if (__builtin_expect(IsRare(a), 0)) {
    return RareLoad16(base, a);
  }
  return __builtin_bswap16(*reinterpret_cast<const volatile uint16_t*>(base + Fold(a)));
}

[[gnu::always_inline]] inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  if (__builtin_expect(IsRare(a), 0)) {
    return RareLoad32(base, a);
  }
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + Fold(a)));
}

[[gnu::always_inline]] inline uint64_t Load64(const uint8_t* base, uint32_t a) {
  if (__builtin_expect(IsRare(a), 0)) {
    return RareLoad64(base, a);
  }
  return __builtin_bswap64(*reinterpret_cast<const volatile uint64_t*>(base + Fold(a)));
}

[[gnu::always_inline]] inline void Store8(uint8_t* base, uint32_t a, uint8_t v) {
  if (__builtin_expect(IsRare(a), 0)) {
    RareStore8(base, a, v);
    return;
  }
  const uint64_t o = Fold(a);
  *reinterpret_cast<volatile uint8_t*>(base + o) = v;
  MarkDirtyHostOffset(o);  // after the data, so a consumer that clears first can't miss it
}

[[gnu::always_inline]] inline void Store16(uint8_t* base, uint32_t a, uint16_t v) {
  if (__builtin_expect(IsRare(a), 0)) {
    RareStore16(base, a, v);
    return;
  }
  const uint64_t o = Fold(a);
  *reinterpret_cast<volatile uint16_t*>(base + o) = __builtin_bswap16(v);
  MarkDirtyHostOffset(o);
}

[[gnu::always_inline]] inline void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  if (__builtin_expect(IsRare(a), 0)) {
    RareStore32(base, a, v);
    return;
  }
  const uint64_t o = Fold(a);
  *reinterpret_cast<volatile uint32_t*>(base + o) = __builtin_bswap32(v);
  MarkDirtyHostOffset(o);
}

[[gnu::always_inline]] inline void Store64(uint8_t* base, uint32_t a, uint64_t v) {
  if (__builtin_expect(IsRare(a), 0)) {
    RareStore64(base, a, v);
    return;
  }
  const uint64_t o = Fold(a);
  *reinterpret_cast<volatile uint64_t*>(base + o) = __builtin_bswap64(v);
  MarkDirtyHostOffset(o);
}

// Store-side raw address (stvx, stwcx./stdcx., dcbz/dcbz128): fold + mark the page dirty.
// Callers access at most 128 naturally aligned bytes, so one page mark suffices.
//
// Ordering: the mark must land AFTER the data (like Store8..64). The GPU thread consumes
// marks at any time, not only after the guest's ring-pointer write: it exchanges a page's
// mark to 0 and then copies the page (ConsumeDirtyPages + UploadRanges) while the guest may
// still be filling another part of the same page (dynamic vertex/index rings). With the mark
// first, "guest marks P -> GPU clears P and copies it -> guest data lands" leaves P changed
// and unmarked (a stale GPU copy). Mark-after is safe: a consumer that clears first either
// copies the new data or sees the later mark.
//
// PPC_RAW_ADDR_W therefore yields RawStoreW(...).ptr: the codegen always uses it inside the
// store's own full-expression (`simde_mm_store_si128((T*)PPC_RAW_ADDR_W(ea), v);`,
// `memset((void*)PPC_RAW_ADDR_W(ea), 0, 128);`, `eq = __sync_bool_compare_and_swap(
// reinterpret_cast<T*>(PPC_RAW_ADDR_W(ea)), ...);`), so the temporary's destructor (the
// mark) runs after the store. The data and mark stores may alias (g_phys_dirty is uint8_t
// and `base` is not restrict), so the compiler keeps them in program order; the signal
// fence makes that explicit at no runtime cost.
struct RawStoreW {
  uint8_t* ptr;
  uint64_t offset;
  [[gnu::always_inline]] RawStoreW(uint8_t* base, uint32_t a) noexcept
      : ptr(base + Fold(a)), offset(Fold(a)) {}
  [[gnu::always_inline]] ~RawStoreW() {
    __atomic_signal_fence(__ATOMIC_RELEASE);
    MarkDirtyHostOffset(offset);
  }
  RawStoreW(const RawStoreW&) = delete;
  RawStoreW& operator=(const RawStoreW&) = delete;
};

// Function form for callers that store later through the returned pointer: marks BEFORE the
// write, so it is only safe when no consumer can run between the mark and the store (unit
// tests, single-threaded tools). Generated code uses PPC_RAW_ADDR_W (RawStoreW).
[[gnu::always_inline]] inline uint8_t* RawAddrW(uint8_t* base, uint32_t a) {
  const uint64_t o = Fold(a);
  MarkDirtyHostOffset(o);
  return base + o;
}

}  // namespace rex::fold
