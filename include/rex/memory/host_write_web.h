/**
 * @file        rex/memory/host_write_web.h
 * @brief       Web write-watch replacement for host writers that only hold a host pointer.
 *
 * Without page protection the web GPU caches see guest CPU writes only through the
 * dirty map (rex/ppc/fold_web.h, Memory::MarkPhysicalDirty in xmemory.h). Guest stores
 * mark it; host code that writes guest memory has to mark it too. Most host writers
 * know the guest address and call Memory::MarkPhysicalDirty(Physical); the generic
 * helpers (memory::store_and_swap / copy_and_swap, used by kernel exports writing
 * their output structures) only see a host pointer, so they call NoteHostWrite, which
 * marks the pages if the pointer lies in the guest physical block (where vertex,
 * index and texture data live) and is a range check otherwise.
 *
 * Memory::Initialize sets the block on web; natively everything here is a no-op
 * (page protection delivers the invalidations).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <rex/platform.h>

namespace rex::memory {

#if REX_PLATFORM_WEB

// Host address of guest physical address 0 (Memory::physical_membase()) and the dirty
// map (rex::fold::g_phys_dirty), null until Memory::Initialize.
inline uint8_t* g_web_physical_host_base = nullptr;
inline uint8_t* g_web_physical_dirty_map = nullptr;
// The physical block plus the page the 0xE0 view's +4 KiB reaches (fold::kDirtyPageCount).
inline constexpr uint64_t kWebPhysicalHostSize = 0x20001000ull;
inline constexpr uint32_t kWebDirtyPageShift = 12;
// NoteHostWrite calls that hit the physical block (statistics: which helpers matter).
inline std::atomic<uint64_t> g_web_host_writes_marked{0};

// Marks the dirty pages of the host range [host, host + length) if it lies in the
// guest physical block. Returns whether it did.
inline bool NoteHostWrite(const void* host, size_t length) noexcept {
  uint8_t* base = g_web_physical_host_base;
  uint64_t offset = uint64_t(reinterpret_cast<uintptr_t>(host)) -
                    uint64_t(reinterpret_cast<uintptr_t>(base));
  if (!base || !length || offset >= kWebPhysicalHostSize) {
    return false;
  }
  uint64_t last = offset + length - 1;
  if (last >= kWebPhysicalHostSize) {
    last = kWebPhysicalHostSize - 1;
  }
  uint8_t* dirty = g_web_physical_dirty_map;
  for (uint64_t page = offset >> kWebDirtyPageShift; page <= (last >> kWebDirtyPageShift);
       ++page) {
    dirty[page] = 1;
  }
  g_web_host_writes_marked.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Debug check for host writers that are expected to have marked their write: true if
// the range is outside the physical block or every page of it is dirty (not consumed
// yet). For assertions right after a write (e.g. assert_true(IsHostWriteMarked(p, n))).
inline bool IsHostWriteMarked(const void* host, size_t length) noexcept {
  uint8_t* base = g_web_physical_host_base;
  uint64_t offset = uint64_t(reinterpret_cast<uintptr_t>(host)) -
                    uint64_t(reinterpret_cast<uintptr_t>(base));
  if (!base || !length || offset >= kWebPhysicalHostSize) {
    return true;
  }
  uint64_t last = offset + length - 1;
  if (last >= kWebPhysicalHostSize) {
    last = kWebPhysicalHostSize - 1;
  }
  for (uint64_t page = offset >> kWebDirtyPageShift; page <= (last >> kWebDirtyPageShift);
       ++page) {
    if (!g_web_physical_dirty_map[page]) {
      return false;
    }
  }
  return true;
}

#else

inline bool NoteHostWrite(const void*, size_t) noexcept {
  return false;
}
inline bool IsHostWriteMarked(const void*, size_t) noexcept {
  return true;
}

#endif  // REX_PLATFORM_WEB

}  // namespace rex::memory
