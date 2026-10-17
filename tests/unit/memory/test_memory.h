/**
 * @file        test_memory.h
 * @brief       Process-wide guest Memory instance shared by the memory unit tests
 *
 * Only one rex::memory::Memory (and one MMIOHandler) may exist per process, so every test
 * TU that needs one must go through this inline accessor (one static across TUs).
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace rex::test {

inline rex::memory::Memory& GetSharedTestMemory() {
  static rex::memory::Memory memory;
  static bool initialized = false;
  if (!initialized) {
    rex::InitLogging();
    bool result = memory.Initialize();
    REQUIRE(result);
    initialized = true;
  }
  return memory;
}

}  // namespace rex::test
