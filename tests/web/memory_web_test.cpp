/**
 * @file        tests/web/memory_web_test.cpp
 * @brief       Node-runnable test of the web rex::memory backend (src/core/memory_web.cpp):
 *              4 KiB granularity (D5), zeroed reservations, commit inside a reservation,
 *              decommit zero-fill, protect no-op, QueryProtect, release, no file mappings.
 *
 * Build + run: tests/web/run_memory_web_test.bat.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/memory/utils.h>

namespace m = rex::memory;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

static bool AllZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i]) return false;
  }
  return true;
}

int main() {
  CHECK(m::page_size() == 4096);
  CHECK(m::allocation_granularity() == 4096);  // D5: no 0xE0 host_address_offset on web
  CHECK(!m::IsWritableExecutableMemorySupported());

  const size_t size = 1024 * 1024;
  auto* base = static_cast<uint8_t*>(
      m::AllocFixed(nullptr, size, m::AllocationType::kReserve, m::PageAccess::kNoAccess));
  CHECK(base != nullptr);
  if (!base) return 1;
  CHECK((reinterpret_cast<uintptr_t>(base) & 0xFFFF) == 0);
  CHECK(AllZero(base, size));

  // Commit inside the reservation returns the requested address; outside fails.
  CHECK(m::AllocFixed(base + 0x3000, 0x2000, m::AllocationType::kCommit,
                      m::PageAccess::kReadWrite) == base + 0x3000);
  CHECK(m::AllocFixed(base + size - 0x1000, 0x2000, m::AllocationType::kCommit,
                      m::PageAccess::kReadWrite) == nullptr);
  CHECK(m::AllocFixed(reinterpret_cast<void*>(0x10000), 0x1000, m::AllocationType::kCommit,
                      m::PageAccess::kReadWrite) == nullptr);

  // Committed contents survive a re-commit (as VirtualAlloc(MEM_COMMIT) over committed pages).
  std::memset(base + 0x3000, 0xAB, 0x2000);
  m::AllocFixed(base + 0x3000, 0x2000, m::AllocationType::kCommit, m::PageAccess::kReadWrite);
  CHECK(base[0x3000] == 0xAB && base[0x4FFF] == 0xAB);

  // Decommit zero-fills (rounded to 4 KiB pages), so a later commit sees fresh pages.
  CHECK(m::DeallocFixed(base + 0x3000, 0x1001, m::DeallocationType::kDecommit));
  CHECK(AllZero(base + 0x3000, 0x2000));
  CHECK(!m::DeallocFixed(reinterpret_cast<void*>(0x10000), 0x1000,
                         m::DeallocationType::kDecommit));

  // Protect is an accepted no-op; QueryProtect reports read/write and the rest of the block.
  m::PageAccess old = m::PageAccess::kNoAccess;
  CHECK(m::Protect(base, 0x1000, m::PageAccess::kReadOnly, &old));
  CHECK(old == m::PageAccess::kReadWrite);
  size_t len = 0;
  m::PageAccess access = m::PageAccess::kNoAccess;
  CHECK(m::QueryProtect(base + 0x1000, len, access));
  CHECK(len == size - 0x1000 && access == m::PageAccess::kReadWrite);

  // Release frees the block; it is unknown afterwards.
  CHECK(!m::DeallocFixed(base + 0x1000, 0, m::DeallocationType::kRelease));
  CHECK(m::DeallocFixed(base, 0, m::DeallocationType::kRelease));
  CHECK(!m::QueryProtect(base, len, access) && len == 0);

  // No shared memory objects on the web.
  CHECK(m::CreateFileMappingHandle("rex_test", 0x10000, m::PageAccess::kReadWrite, true) ==
        m::kFileMappingHandleInvalid);
  CHECK(m::MapFileView(m::kFileMappingHandleInvalid, nullptr, 0x10000, m::PageAccess::kReadWrite,
                       0) == nullptr);

  std::printf("%d checks, %d failures\n%s memory_web\n", g_checks, g_failures,
              g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
