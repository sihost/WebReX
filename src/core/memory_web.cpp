/**
 * @file        core/memory_web.cpp
 * @brief       Web (Emscripten, wasm64) backend for rex::memory host page APIs.
 *
 * Replaces memory_posix.cpp when REX_PLATFORM_WEB. Wasm linear memory has no pages to
 * reserve, protect or decommit, no shm_open and no /proc/self/maps, so:
 *  - page_size() and allocation_granularity() report 4 KiB (PHASE1_PLAN D5): the guest
 *    window fold already contains the 0xE0 +0x1000, so the Windows-style 64 KiB
 *    host_address_offset must never be applied on top of it (docs/port/memory.md 4.1).
 *  - AllocFixed(nullptr, ...) carves a zeroed, 64 KiB-aligned block out of the heap and
 *    remembers it. AllocFixed(addr, ...) only succeeds inside such a block (commit on an
 *    existing reservation); wasm can not place memory at an arbitrary address.
 *  - Decommit zero-fills the range, so a later commit sees zero pages exactly like
 *    VirtualAlloc(MEM_COMMIT) after MEM_DECOMMIT (memory.md section 8). Committing pages
 *    that are already committed keeps their contents, as on Windows.
 *  - Protect() is a successful no-op; every page is read/write. QueryProtect() reports
 *    kReadWrite inside tracked blocks.
 *  - File mappings are not supported (no shared memory objects); they fail cleanly. The web
 *    guest window does not use them (src/system/xmemory.cpp, REX_PLATFORM_WEB branch).
 *
 * The guest window itself is allocated by Memory::Initialize (WS-A1) and does not go through
 * these functions.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

static_assert(REX_PLATFORM_WEB, "This file is web-only (Emscripten)");

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <rex/memory/utils.h>

namespace rex::memory {

namespace {

constexpr size_t kWebPageSize = 4096;
constexpr size_t kReservationAlignment = 64 * 1024;  // what VirtualAlloc hands out

struct Reservation {
  size_t size;  // usable bytes from the aligned base
  void* raw;    // pointer returned by malloc
};

std::mutex g_reservations_mutex;
std::map<uintptr_t, Reservation> g_reservations;  // keyed by aligned base

size_t RoundUpToPage(size_t value) {
  return (value + kWebPageSize - 1) & ~(kWebPageSize - 1);
}

// Returns the reservation containing [address, address + length), or end().
std::map<uintptr_t, Reservation>::iterator FindReservationLocked(uintptr_t address,
                                                                 size_t length) {
  auto it = g_reservations.upper_bound(address);
  if (it == g_reservations.begin()) {
    return g_reservations.end();
  }
  --it;
  const uintptr_t begin = it->first;
  const uintptr_t end = begin + it->second.size;
  if (address < begin || address >= end || length > end - address) {
    return g_reservations.end();
  }
  return it;
}

void WarnOnce(std::atomic<bool>& flag, const char* message) {
  if (!flag.exchange(true)) {
    std::fprintf(stderr, "[rex::memory/web] %s\n", message);
  }
}

}  // namespace

size_t page_size() {
  return kWebPageSize;
}

size_t allocation_granularity() {
  return kWebPageSize;
}

bool IsWritableExecutableMemorySupported() {
  return false;  // wasm has no executable data memory at all
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  (void)allocation_type;
  (void)access;
  if (length == 0) {
    return nullptr;
  }
  length = RoundUpToPage(length);

  std::lock_guard<std::mutex> lock(g_reservations_mutex);
  if (base_address) {
    // Commit (or re-reserve) inside an existing block. Fresh pages are zero; pages that are
    // still committed keep their contents.
    auto it = FindReservationLocked(reinterpret_cast<uintptr_t>(base_address), length);
    return it == g_reservations.end() ? nullptr : base_address;
  }

  // New block. Reserve and commit are the same thing here; the block is zeroed up front
  // (calloc), which only touches memory the heap had already used before.
  if (length > SIZE_MAX - kReservationAlignment) {
    return nullptr;
  }
  void* raw = std::calloc(1, length + kReservationAlignment);
  if (!raw) {
    return nullptr;
  }
  uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + kReservationAlignment - 1) &
                      ~uintptr_t(kReservationAlignment - 1);
  g_reservations.emplace(aligned, Reservation{length, raw});
  return reinterpret_cast<void*>(aligned);
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  std::lock_guard<std::mutex> lock(g_reservations_mutex);
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      if (length == 0) {
        return false;
      }
      auto it = FindReservationLocked(address, 1);
      if (it == g_reservations.end()) {
        return false;
      }
      // Clip to the block and zero, so the next commit sees fresh pages.
      const uintptr_t end = it->first + it->second.size;
      const size_t clipped = std::min<size_t>(RoundUpToPage(length), end - address);
      std::memset(base_address, 0, clipped);
      return true;
    }
    case DeallocationType::kRelease: {
      auto it = g_reservations.find(address);
      if (it == g_reservations.end()) {
        return false;
      }
      std::free(it->second.raw);
      g_reservations.erase(it);
      return true;
    }
    default:
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  (void)base_address;
  (void)length;
  (void)access;
  // No page protection in wasm: accept the request, report the only state pages can be in.
  if (out_old_access) {
    *out_old_access = PageAccess::kReadWrite;
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  std::lock_guard<std::mutex> lock(g_reservations_mutex);
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  auto it = FindReservationLocked(address, 1);
  if (it == g_reservations.end()) {
    length = 0;
    access_out = PageAccess::kNoAccess;
    return false;
  }
  length = static_cast<size_t>(it->first + it->second.size - address);
  access_out = PageAccess::kReadWrite;
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)path;
  (void)length;
  (void)access;
  (void)commit;
  static std::atomic<bool> warned{false};
  WarnOnce(warned, "CreateFileMappingHandle: no shared memory objects on the web (unsupported)");
  return kFileMappingHandleInvalid;
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)handle;
  (void)path;
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  (void)handle;
  (void)base_address;
  (void)length;
  (void)access;
  (void)file_offset;
  return nullptr;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)handle;
  (void)base_address;
  (void)length;
  return false;
}

}  // namespace rex::memory
