/**
 * @file        fold_web_test.cpp
 * @brief       Native tests of the web guest-address fold (rex/ppc/fold_web.h)
 *
 * The web build (REX_PLATFORM_WEB) has no aliasing views; every guest access is folded with
 * a 256-entry top-byte delta table onto one window (Layout B, physical at 0x90000000). These
 * tests compile the same table and out-of-line runtime natively and check them against:
 *   - the native view table map_info (Memory::GetViewMapInfo), literally,
 *   - the native aliasing views of a live Memory (writes through one alias),
 *   - MMIOHandler routing and the dirty map / invalidation-callback consumption.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/fold_web.h>
#include <rex/ppc/memory.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>

#include "test_memory.h"

using namespace rex::fold;

namespace {

constexpr uint64_t kNativePhys = 0x100000000ull;  // native mapping offset of physical memory

// Literal map_info semantics: offset of guest address a in the window, with the physical
// block relocated from its native mapping offset (0x100000000) to phys_offset.
uint64_t MapInfoOffset(uint32_t a, uint64_t phys_offset) {
  size_t count = 0;
  const auto* views = rex::memory::Memory::GetViewMapInfo(&count);
  for (size_t i = 0; i < count; ++i) {
    const auto& v = views[i];
    if (v.virtual_address_start > 0xFFFFFFFFull) {
      continue;  // the raw physical view, not a guest virtual range
    }
    if (a >= v.virtual_address_start && a <= v.virtual_address_end) {
      uint64_t target = v.target_address;
      if (target >= kNativePhys) {
        target = target - kNativePhys + phys_offset;
      }
      return target + (a - v.virtual_address_start);
    }
  }
  return ~0ull;
}

template <typename F>
void ForSampledAddresses(F&& f) {
  uint32_t rng = 1;
  for (uint32_t hi = 0; hi < 256; ++hi) {
    for (int k = 0; k < 4096; ++k) {
      rng = rng * 1664525u + 1013904223u;
      const uint32_t low = k == 0 ? 0u : k == 1 ? 0xFFFFFFu : (rng & 0xFFFFFFu);
      f((hi << 24) | low);
    }
  }
}

// A fake window base such that base + Fold(a) for physical addresses lands in buf
// (buf[i] = physical byte i).
uint8_t* FakeBaseForPhysical(std::vector<uint8_t>& buf) {
  return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(buf.data()) - kPhysOffset);
}

struct MmioLog {
  uint32_t reads = 0;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
};

uint32_t TestMmioRead(void*, void* context, uint32_t addr) {
  auto* log = static_cast<MmioLog*>(context);
  ++log->reads;
  return 0xCAFE0000u | (addr & 0xFFFF);
}

void TestMmioWrite(void*, void* context, uint32_t addr, uint32_t value) {
  static_cast<MmioLog*>(context)->writes.emplace_back(addr, value);
}

MmioLog& RegisterTestMmioWindow() {
  // Registered once into the shared Memory's MMIOHandler (0x7FEA0000/64 KiB is the XMA window
  // in real titles; no audio system runs in the unit tests). No page protection is touched.
  static MmioLog log;
  static bool registered = false;
  rex::test::GetSharedTestMemory();
  if (!registered) {
    auto* handler = rex::runtime::MMIOHandler::global_handler();
    REQUIRE(handler != nullptr);
    REQUIRE(handler->RegisterRange(0x7FEA0000, 0xFFFF0000, 0x10000, &log, TestMmioRead,
                                   TestMmioWrite));
    registered = true;
  }
  log.reads = 0;
  log.writes.clear();
  return log;
}

}  // namespace

TEST_CASE("Fold table reproduces map_info for every top byte", "[memory][fold]") {
  constexpr FoldTable kLayoutA{kNativePhys};
  uint64_t samples = 0, mismatches_a = 0, mismatches_b = 0;
  ForSampledAddresses([&](uint32_t a) {
    ++samples;
    if (FoldWith(kLayoutA, a) != MapInfoOffset(a, kNativePhys)) {
      ++mismatches_a;
    }
    if (Fold(a) != MapInfoOffset(a, kPhysOffset)) {
      ++mismatches_b;
    }
  });
  CHECK(samples == 256 * 4096);
  CHECK(mismatches_a == 0);
  CHECK(mismatches_b == 0);
}

TEST_CASE("Fold layout B constants and window bounds", "[memory][fold]") {
  static_assert(Fold(0x82000000u) == 0x82000000u);
  static_assert(Fold(0x9A000010u) == 0x8A000010u);
  static_assert(Fold(0x7F000010u) == kPhysOffset + 0x10);
  static_assert(Fold(0xA0001234u) == kPhysOffset + 0x1234);
  static_assert(Fold(0xC0001234u) == kPhysOffset + 0x1234);
  static_assert(Fold(0xE0001234u) == kPhysOffset + 0x2234);
  static_assert(Fold(0xFFFFFFFFu) == kWindowSize - 1);
  static_assert(kWindowSize == 0xB0001000ull);
  static_assert(kDirtyPageCount == 0x20001);

  for (int i = 0; i < 256; ++i) {
    // Every delta keeps 4 KiB alignment (atomics / reservations stay naturally aligned).
    CHECK((kFold.d[i] & 0xFFF) == 0);
    const uint32_t lo = uint32_t(i) << 24, hi = lo | 0xFFFFFFu;
    CHECK(Fold(hi) < kWindowSize);
    const bool physical = i == 0x7F || i >= 0xA0;
    CHECK(IsPhysicalOffset(Fold(lo)) == physical);
    CHECK(IsPhysicalOffset(Fold(hi)) == physical);
    CHECK(IsRare(lo) == (i == 0x7F));
  }
}

TEST_CASE("Fold agrees with the native aliasing views of a live Memory", "[memory][fold]") {
  auto& memory = rex::test::GetSharedTestMemory();

  SECTION("0x9xxxxxxx xex view aliases 0x8xxxxxxx") {
    // Memory::Initialize commits 0x80000000+ and writes 0x2A6E3F38 at 0x8000001C.
    const uint32_t value = *memory.TranslateVirtual<uint32_t*>(0x8000001C);
    CHECK(value == __builtin_bswap32(0x2A6E3F38u));
    CHECK(*memory.TranslateVirtual<uint32_t*>(0x9000001C) == value);
    CHECK(Fold(0x9000001Cu) == Fold(0x8000001Cu));
  }

  SECTION("0xA0/0xC0/0xE0 views alias the physical heap at the folded offset") {
    auto* heap = memory.LookupHeap(0xA0000000);
    REQUIRE(heap != nullptr);
    uint32_t a0 = 0;
    REQUIRE(heap->Alloc(0x20000, 0x10000,
                        rex::memory::kMemoryAllocationReserve |
                            rex::memory::kMemoryAllocationCommit,
                        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                        &a0));
    const uint32_t phys = memory.GetPhysicalAddress(a0);
    REQUIRE(phys != UINT32_MAX);
    REQUIRE(phys >= 0x1000);
    const uint32_t c0 = 0xC0000000u + phys;
    const uint32_t e0 = 0xE0000000u + phys + 0x2000 - 0x1000;  // physical phys + 0x2000

    *memory.TranslateVirtual<uint32_t*>(a0) = 0x11223344u;
    *memory.TranslateVirtual<uint32_t*>(a0 + 0x2000) = 0x55667788u;
    // Native aliasing: the same bytes are visible through every view and the raw block.
    CHECK(*memory.TranslatePhysical<uint32_t*>(phys) == 0x11223344u);
    CHECK(*memory.TranslateVirtual<uint32_t*>(c0) == 0x11223344u);
    CHECK(*memory.TranslateVirtual<uint32_t*>(e0) == 0x55667788u);
    // Fold: every alias lands on kPhysOffset + physical address.
    CHECK(Fold(a0) == kPhysOffset + phys);
    CHECK(Fold(c0) == kPhysOffset + phys);
    CHECK(Fold(e0) == kPhysOffset + phys + 0x2000);
    CHECK(Fold(a0 + 0x2000) == Fold(e0));

    heap->Release(a0);
  }
}

TEST_CASE("Fold inline loads/stores and the 0x7F alias path", "[memory][fold]") {
  RegisterTestMmioWindow();
  std::vector<uint8_t> buf(0x10000, 0);
  uint8_t* base = FakeBaseForPhysical(buf);
  ClearAllDirty();

  Store32(base, 0xA0000100u, 0xAABBCCDDu);
  CHECK(buf[0x100] == 0xAA);
  CHECK(buf[0x103] == 0xDD);
  CHECK(Load32(base, 0xC0000100u) == 0xAABBCCDDu);
  CHECK(Load16(base, 0xA0000102u) == 0xCCDDu);
  CHECK(Load8(base, 0x7F000100u) == 0xAA);
  CHECK(g_phys_dirty[0] == 1);
  // 0xE0 view: physical = (a & 0x1FFFFFFF) + 0x1000.
  Store32(base, 0xE0000200u, 0x01020304u);
  CHECK(Load32(base, 0xA0001200u) == 0x01020304u);
  CHECK(g_phys_dirty[1] == 1);
  ClearAllDirty();

  // 0x7F outside the MMIO windows: physical alias, through the out-of-line path.
  Store64(base, 0x7F001000u, 0x0102030405060708ull);
  CHECK(buf[0x1000] == 0x01);
  CHECK(buf[0x1007] == 0x08);
  CHECK(g_phys_dirty[1] == 1);
  CHECK(Load64(base, 0xA0001000u) == 0x0102030405060708ull);
  CHECK(Load32(base, 0x7F001004u) == 0x05060708u);
  Store8(base, 0x7F002000u, 0x5A);
  CHECK(buf[0x2000] == 0x5A);
  CHECK(Load8(base, 0x7F002000u) == 0x5A);
  Store16(base, 0x7F002002u, 0x1234);
  CHECK(Load16(base, 0x7F002002u) == 0x1234);
  CHECK(Load16(base, 0xC0002002u) == 0x1234);

  // Raw store-side address marks dirty.
  uint8_t* p = RawAddrW(base, 0xA0003080u);
  CHECK(p == buf.data() + 0x3080);
  CHECK(g_phys_dirty[3] == 1);
  CHECK(g_phys_dirty[4] == 0);
}

TEST_CASE("Fold routes registered MMIO windows to MMIOHandler", "[memory][fold]") {
  MmioLog& log = RegisterTestMmioWindow();
  std::vector<uint8_t> buf(0x10000, 0);
  uint8_t* base = FakeBaseForPhysical(buf);
  ClearAllDirty();

  CHECK(Load32(base, 0x7FEA0010u) == 0xCAFE0010u);
  CHECK(Load16(base, 0x7FEA0020u) == 0x0020u);  // truncated register value (as PPC_MM_LOAD_U16)
  CHECK(Load8(base, 0x7FEA0031u) == 0x31u);
  CHECK(Load64(base, 0x7FEA0040u) == ((uint64_t(0xCAFE0040u) << 32) | 0xCAFE0044u));
  CHECK(log.reads == 5);

  Store32(base, 0x7FEA0100u, 0xDEADBEEFu);
  Store64(base, 0x7FEA0200u, 0x1111222233334444ull);
  Store16(base, 0x7FEA0300u, 0xABCD);
  REQUIRE(log.writes.size() == 4);
  CHECK(log.writes[0] == std::make_pair(0x7FEA0100u, 0xDEADBEEFu));
  CHECK(log.writes[1] == std::make_pair(0x7FEA0200u, 0x11112222u));  // high word first
  CHECK(log.writes[2] == std::make_pair(0x7FEA0204u, 0x33334444u));
  CHECK(log.writes[3] == std::make_pair(0x7FEA0300u, 0x0000ABCDu));

  // MMIO traffic never touches the aliased RAM nor the dirty map.
  for (auto b : buf) {
    REQUIRE(b == 0);
  }
  for (uint32_t i = 0; i < 0x100; ++i) {
    REQUIRE(g_phys_dirty[(0xEA0000 >> kDirtyPageShift) + i] == 0);
  }
}

TEST_CASE("Dirty map marking and ConsumePhysicalDirty", "[memory][fold]") {
  auto& memory = rex::test::GetSharedTestMemory();
  struct Seen {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
  } seen;
  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(
      [](void* context, uint32_t start, uint32_t length,
         bool exact) -> std::pair<uint32_t, uint32_t> {
        CHECK(exact);
        static_cast<Seen*>(context)->ranges.emplace_back(start, length);
        return {0, UINT32_MAX};
      },
      &seen);

  ClearAllDirty();
  CHECK_FALSE(memory.ConsumePhysicalDirty(0, 0x20000000));
  CHECK(seen.ranges.empty());

  MarkPhysicalRangeDirty(0x5000, 0x2001);  // pages 5, 6, 7
  MarkPhysicalRangeDirty(0x9000, 1);       // page 9
  // Virtual-only ranges never mark; a physical-view range spanning a 16 MiB view boundary does.
  MarkGuestRangeDirty(0x40000000u, 0x10000);
  MarkGuestRangeDirty(0xA0FFF000u, 0x2000);  // physical 0xFFF000 and 0x1000000
  MarkGuestRangeDirty(0xE0000000u, 1);       // physical 0x1000 (0xE0 view +4 KiB)

  // Only the requested range is consumed.
  CHECK(memory.ConsumePhysicalDirty(0x6000, 0x1000));
  REQUIRE(seen.ranges.size() == 1);
  CHECK(seen.ranges[0] == std::make_pair(0x6000u, 0x1000u));

  seen.ranges.clear();
  CHECK(memory.ConsumePhysicalDirty(0, 0x20000000));
  REQUIRE(seen.ranges.size() == 5);
  CHECK(seen.ranges[0] == std::make_pair(0x1000u, 0x1000u));
  CHECK(seen.ranges[1] == std::make_pair(0x5000u, 0x1000u));
  CHECK(seen.ranges[2] == std::make_pair(0x7000u, 0x1000u));
  CHECK(seen.ranges[3] == std::make_pair(0x9000u, 0x1000u));
  CHECK(seen.ranges[4] == std::make_pair(0xFFF000u, 0x2000u));

  seen.ranges.clear();
  CHECK_FALSE(memory.ConsumePhysicalDirty(0, 0x20000000));
  CHECK(seen.ranges.empty());

  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
}

TEST_CASE("Native PPC_RAW_ADDR_W is PPC_RAW_ADDR", "[memory][fold]") {
  // The codegen emits PPC_RAW_ADDR_W for stvx / stwcx. / dcbz; natively it must translate
  // exactly like PPC_RAW_ADDR (including the Windows 0xE0 +0x1000), with no side effects.
  auto& memory = rex::test::GetSharedTestMemory();
  uint8_t* base = memory.virtual_membase();
  for (uint32_t a : {0x00010000u, 0x40001230u, 0x7F000010u, 0x82000100u, 0x9A000010u,
                     0xA0001000u, 0xC0002000u, 0xE0003000u, 0xFFFFFF80u}) {
    CHECK(PPC_RAW_ADDR_W(a) == PPC_RAW_ADDR(a));
    CHECK(PPC_HOST_ADDR(base, a) == PPC_RAW_ADDR(a));
  }
}
