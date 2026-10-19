/**
 * @file        tests/web/cvar_stub.cpp
 * @brief       Minimal cvar registry for the standalone web tests that compile single SDK
 *              sources (threading_web.cpp defines cvars) without src/core/cvar.cpp and its
 *              CLI11/toml++/spdlog dependencies. Cvars keep their defaults.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/cvar.h>

namespace rex::cvar {

std::vector<FlagEntry>& GetRegistry() {
  static std::vector<FlagEntry> registry;
  return registry;
}

void RegisterFlag(FlagEntry entry) { GetRegistry().push_back(std::move(entry)); }

}  // namespace rex::cvar
