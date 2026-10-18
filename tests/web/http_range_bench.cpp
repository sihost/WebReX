/**
 * @file        tests/web/http_range_bench.cpp
 * @brief       Browser throughput benchmark of HttpRangeDevice (serial vs fetch pool +
 *              read-ahead) against a real server, e.g. the Halo server with --throttle.
 *
 * Build: tests/web/run_http_range_bench.bat (em++ -> http_range_bench.js + .html). Serve the
 * output directory with the game server (--web <dir>) so /data/ and /manifest.json are same
 * origin, then open /http_range_bench.html (tests/web/run_in_chrome.mjs --url ... --done
 * "BENCH DONE"). Query: ?file=<manifest path suffix> (default zanzibar.map), ?mib=<MiB per
 * workload> (default 16), ?sw=1 registers /sw.js first (the Halo page's service worker) so
 * the requests go through it. Every (config, workload) pair reads its own region of the file,
 * so no run is served from a cache an earlier one filled. Memory cache only (no OPFS).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <emscripten/em_js.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/filesystem/devices/http_range_device.h>

using rex::filesystem::HttpRangeDevice;
using rex::filesystem::HttpRangeDeviceOptions;
using Clock = std::chrono::steady_clock;

// clang-format off
EM_JS(double, bench_query_number, (double name_ptr, double fallback), {
  var v = new URLSearchParams(location.search).get(UTF8ToString(name_ptr));
  return v === null ? fallback : Number(v);
});
EM_JS(void, bench_query_string, (double name_ptr, double dst, double cap), {
  var v = new URLSearchParams(location.search).get(UTF8ToString(name_ptr)) || "";
  stringToUTF8(v, dst, cap);
});
// clang-format on
EM_JS_DEPS(http_range_bench, "$UTF8ToString,$stringToUTF8");

namespace {

double Arg(const char* name, double fallback) {
  return bench_query_number(double(reinterpret_cast<uintptr_t>(name)), fallback);
}

std::string ArgString(const char* name, const char* fallback) {
  char buf[512] = {};
  bench_query_string(double(reinterpret_cast<uintptr_t>(name)),
                     double(reinterpret_cast<uintptr_t>(buf)), double(sizeof(buf)));
  return buf[0] ? std::string(buf) : std::string(fallback);
}

struct Config {
  const char* name;
  int32_t threads;
  int32_t ahead;
};

}  // namespace

int main() {
  const std::string want = ArgString("file", "zanzibar.map");
  const uint64_t region = uint64_t(Arg("mib", 16)) << 20;
  const Config configs[] = {
      {"serial (before)", 0, 0},
      {"pool 2, ahead 4", 2, 4},
      {"pool 4, ahead 8", 4, 8},
      {"pool 5, ahead 8", 5, 8},
  };
  struct Workload {
    const char* name;
    size_t chunk;
  };
  const Workload workloads[] = {{"sequential 64 KiB reads", 64 << 10},
                                {"8 MiB reads", 8 << 20}};

  uint64_t next_region = 0;
  for (const Config& c : configs) {
    HttpRangeDeviceOptions o;
    o.fetch_threads = c.threads;
    o.read_ahead_blocks = c.ahead;
    HttpRangeDevice device("\\Device\\Bench", o);
    if (!device.Initialize()) {
      std::printf("manifest failed\nBENCH DONE\n");
      return 1;
    }
    size_t file = SIZE_MAX;
    for (size_t i = 0; i < device.files().size(); ++i) {
      const std::string& p = device.files()[i].path;
      if (p.size() >= want.size() && p.compare(p.size() - want.size(), want.size(), want) == 0) {
        file = i;
        break;
      }
    }
    if (file == SIZE_MAX) {
      std::printf("no %s in the manifest\nBENCH DONE\n", want.c_str());
      return 1;
    }
    const uint64_t size = device.files()[file].size;
    for (const Workload& w : workloads) {
      uint64_t base = next_region;
      next_region += region + (8 << 20);  // gap: read-ahead past a region never feeds the next
      if (next_region > size) {
        std::printf("file too small for every region\nBENCH DONE\n");
        return 1;
      }
      std::vector<uint8_t> buf(w.chunk);
      auto before = device.stats();
      auto t0 = Clock::now();
      bool ok = true;
      for (uint64_t off = base; off < base + region && ok; off += w.chunk) {
        size_t got = 0;
        ok = device.Read(file, off, buf.data(), w.chunk, &got) && got == w.chunk;
      }
      double s = std::chrono::duration<double>(Clock::now() - t0).count();
      auto after = device.stats();
      std::printf("BENCH %-16s | %-24s | %6.2f MB/s | %5.2f s | %3llu requests (%llu on pool), "
                  "%llu blocks ahead, %llu waits%s\n",
                  c.name, w.name, double(region) / 1e6 / s, s,
                  (unsigned long long)(after.requests - before.requests),
                  (unsigned long long)(after.pool_requests - before.pool_requests),
                  (unsigned long long)(after.read_ahead_blocks - before.read_ahead_blocks),
                  (unsigned long long)(after.inflight_waits - before.inflight_waits),
                  ok ? "" : " | READ FAILED");
    }
  }
  std::printf("BENCH DONE\n");
  return 0;
}
