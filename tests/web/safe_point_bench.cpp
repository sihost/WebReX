/**
 * @file        tests/web/safe_point_bench.cpp
 * @brief       Cost of rex::thread::SafePoint() in a kernel export thunk on wasm (the
 *              PPC_HOOK_SAFE_POINT prologue, rex/ppc/function.h): calls a noinline thunk with and
 *              without the safe point and prints the difference per call.
 *
 * Build + run (node, from the SDK root):
 *   em++ -std=c++23 -O2 -DNDEBUG -m64 -pthread -fwasm-exceptions -fno-char8_t -Iinclude
 *        -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sENVIRONMENT=node tests/web/safe_point_bench.cpp
 *        src/core/threading_web.cpp src/core/threading.cpp src/core/clock_posix.cpp
 *        src/core/fiber_web.cpp tests/web/cvar_stub.cpp -o safe_point_bench.js
 *   node safe_point_bench.js
 * 2026-10-18 (node 24, busy machine): 6.41 ns/call without, 6.19 with: below the noise
 * (one relaxed i32.atomic.load + branch).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>

#include <rex/thread/safe_point.h>

namespace {

struct Ctx {
  uint64_t r3;
  uint64_t r4;
};

__attribute__((noinline)) void Handler(Ctx& ctx) { ctx.r3 = ctx.r3 * 3 + ctx.r4; }

__attribute__((noinline)) void ThunkPlain(Ctx& ctx) { Handler(ctx); }

__attribute__((noinline)) void ThunkSafePoint(Ctx& ctx) {
  rex::thread::SafePoint();
  Handler(ctx);
}

template <void (*F)(Ctx&)>
double NsPerCall(uint64_t n) {
  Ctx ctx{1, 2};
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < n; ++i) {
    ctx.r4 = i;
    F(ctx);
  }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (ctx.r3 == 42) std::printf(" ");  // keep the result alive
  return s * 1e9 / double(n);
}

}  // namespace

int main() {
  constexpr uint64_t kCalls = 200'000'000;
  double best_plain = 1e9, best_sp = 1e9;
  for (int round = 0; round < 5; ++round) {
    double p = NsPerCall<ThunkPlain>(kCalls);
    double s = NsPerCall<ThunkSafePoint>(kCalls);
    if (p < best_plain) best_plain = p;
    if (s < best_sp) best_sp = s;
  }
  std::printf("thunk without safe point: %.3f ns/call\n", best_plain);
  std::printf("thunk with safe point:    %.3f ns/call (+%.3f ns)\n", best_sp, best_sp - best_plain);
  return 0;
}
