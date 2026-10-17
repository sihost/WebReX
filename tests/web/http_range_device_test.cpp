/**
 * @file        tests/web/http_range_device_test.cpp
 * @brief       Node-runnable test of the web HttpRangeDevice (manifest, tree, block reads,
 *              memory LRU, persistent block cache, errors, concurrent readers).
 *
 * Under node the device's transport reads the host directory $REX_WEB_TEST_DATA_ROOT instead of
 * issuing XHRs; this test creates that data itself. Build + run:
 * tests/web/run_http_range_device_test.bat (needs the wasm64 SDK libs in out/wasm64).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <emscripten/em_js.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/filesystem/devices/http_range_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

using rex::filesystem::Entry;
using rex::filesystem::File;
using rex::filesystem::HttpRangeDevice;
using rex::filesystem::HttpRangeDeviceOptions;
using rex::X_STATUS;

// clang-format off
EM_JS(void, test_write_host_file, (double path_ptr, double data_ptr, double length), {
  var fs = require("fs");
  var path = require("path");
  var p = UTF8ToString(path_ptr);
  fs.mkdirSync(path.dirname(p), {recursive: true});
  fs.writeFileSync(p, HEAPU8.slice(data_ptr, data_ptr + length));
});
EM_JS(double, test_host_data_root, (double dst, double cap), {
  var os = require("os");
  var path = require("path");
  // Set by the runner before node starts: every Worker gets a copy of the environment at spawn.
  var root = process.env["REX_WEB_TEST_DATA_ROOT"] || path.join(os.tmpdir(), "rex_http_range_test");
  process.env["REX_WEB_TEST_DATA_ROOT"] = root;
  stringToUTF8(root, dst, cap);
  return lengthBytesUTF8(root);
});
EM_JS(double, test_request_count, (), { return globalThis["__rexWebTestRequests"] || 0; });
// clang-format on
EM_JS_DEPS(http_range_device_test, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

uint8_t Expected(uint64_t i) {
  return uint8_t((i * 2654435761u) >> 13);
}

constexpr uint64_t kMapSize = 3 * 1024 * 1024 + 12345;  // not block aligned

double P(const void* p) {
  return double(reinterpret_cast<uintptr_t>(p));
}

std::string ReadAll(File* file, uint64_t offset, size_t length, size_t* got, X_STATUS* status) {
  std::string out(length, '\0');
  *status = file->ReadSync(std::span<uint8_t>(reinterpret_cast<uint8_t*>(out.data()), length),
                           offset, got);
  out.resize(*got);
  return out;
}

bool Matches(const std::string& data, uint64_t offset) {
  for (size_t i = 0; i < data.size(); ++i) {
    if (uint8_t(data[i]) != Expected(offset + i)) return false;
  }
  return true;
}

HttpRangeDeviceOptions Options() {
  HttpRangeDeviceOptions o;
  o.base_url = "/data/";
  o.manifest_url = "/manifest.json";
  o.persistent_cache_dir = "/rex-cache";  // MEMFS under node: exercises the code path
  o.block_size = 64 * 1024;
  o.memory_cache_blocks = 8;
  o.max_request_bytes = 256 * 1024;
  return o;
}

}  // namespace

int main() {
  // Host-side data root: <tmp>/data/maps/Test.map, <tmp>/data/init.txt, <tmp>/manifest.json.
  char root_c[1024] = {};
  test_host_data_root(P(root_c), double(sizeof(root_c)));
  std::string root(root_c);
  std::vector<uint8_t> map(kMapSize);
  for (uint64_t i = 0; i < kMapSize; ++i) map[i] = Expected(i);
  test_write_host_file(P((root + "/data/maps/Test.map").c_str()), P(map.data()), double(map.size()));
  std::string init = "rasterizer_wireframe 0\n";
  test_write_host_file(P((root + "/data/init.txt").c_str()), P(init.data()), double(init.size()));
  std::string manifest = "{\"files\":[{\"path\":\"maps/Test.map\",\"size\":" +
                         std::to_string(kMapSize) +
                         ",\"sha256\":\"ABCDEF0123\"},{\"path\":\"init.txt\",\"size\":" +
                         std::to_string(init.size()) +
                         "},{\"path\":\"maps/missing.map\",\"size\":1000},"
                         "{\"path\":\"bad\\ttab\",\"size\":1}]}";
  test_write_host_file(P((root + "/manifest.json").c_str()), P(manifest.data()),
                       double(manifest.size()));

  // 1. Manifest + tree.
  HttpRangeDevice device("\\Device\\Web\\Game", Options());
  CHECK(device.Initialize());
  CHECK(device.files().size() == 3);  // the tab path is rejected
  CHECK(device.files()[0].sha256 == "abcdef0123");
  Entry* maps = device.ResolvePath("MAPS");
  CHECK(maps && (maps->attributes() & rex::filesystem::kFileAttributeDirectory));
  Entry* entry = device.ResolvePath("maps\\test.MAP");  // case-insensitive
  CHECK(entry && entry->size() == kMapSize);
  CHECK(entry && entry->name() == "Test.map");
  CHECK(device.ResolvePath("maps\\nope.map") == nullptr);
  if (!entry) return 1;

  // 2. Write access is refused.
  File* file = nullptr;
  CHECK(entry->Open(rex::filesystem::FileAccess::kFileWriteData, &file) == X_STATUS_ACCESS_DENIED);
  CHECK(entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) == X_STATUS_SUCCESS);

  // 3. Reads: inside a block, across blocks, coalesced run, tail, EOF.
  size_t got = 0;
  X_STATUS st = 0;
  struct Case { uint64_t offset; size_t length; size_t expect; };
  const Case cases[] = {
      {0, 16, 16},
      {65536 - 7, 20, 20},
      {100000, 600000, 600000},  // > max_request_bytes: several coalesced requests
      {kMapSize - 100, 1000, 100},
      {kMapSize, 10, 0},
      {kMapSize + 5000, 10, 0},
  };
  for (const auto& c : cases) {
    std::string data = ReadAll(file, c.offset, c.length, &got, &st);
    CHECK(st == X_STATUS_SUCCESS);
    CHECK(got == c.expect);
    CHECK(Matches(data, c.offset));
  }
  auto s1 = device.stats();
  CHECK(s1.requests > 0 && s1.bytes_downloaded > 0);

  // 4. Re-reading a recently read range hits the memory LRU: no new request.
  ReadAll(file, kMapSize - 50, 16, &got, &st);
  auto s2 = device.stats();
  CHECK(s2.requests == s1.requests);
  CHECK(s2.memory_hits > s1.memory_hits);

  // 5. Whole file read; then early blocks were evicted from the 8-block LRU and come back from
  //    the persistent cache, still without new requests.
  std::string all = ReadAll(file, 0, kMapSize, &got, &st);
  CHECK(st == X_STATUS_SUCCESS && got == kMapSize && Matches(all, 0));
  auto s3 = device.stats();
  ReadAll(file, 0, 16, &got, &st);
  auto s4 = device.stats();
  CHECK(s4.requests == s3.requests);
  CHECK(s4.persistent_hits > s3.persistent_hits);
  file->Destroy();

  // 6. A second device on the same persistent dir downloads nothing.
  {
    HttpRangeDevice again("\\Device\\Web\\Game2", Options());
    CHECK(again.Initialize());
    Entry* e2 = again.ResolvePath("maps\\Test.map");
    File* f2 = nullptr;
    CHECK(e2 && e2->Open(rex::filesystem::FileAccess::kGenericRead, &f2) == X_STATUS_SUCCESS);
    if (f2) {
      std::string d = ReadAll(f2, 1234567, 300000, &got, &st);
      CHECK(st == X_STATUS_SUCCESS && got == 300000 && Matches(d, 1234567));
      f2->Destroy();
    }
    CHECK(again.stats().requests == 0);
    CHECK(again.stats().persistent_hits > 0);
  }

  // 7. Small file, and a manifest entry the server does not have (404 -> I/O error).
  Entry* init_entry = device.ResolvePath("INIT.TXT");
  File* f3 = nullptr;
  CHECK(init_entry && init_entry->Open(rex::filesystem::FileAccess::kGenericRead, &f3) == X_STATUS_SUCCESS);
  if (f3) {
    std::string d = ReadAll(f3, 0, 4096, &got, &st);
    CHECK(st == X_STATUS_SUCCESS && d == init);
    f3->Destroy();
  }
  Entry* missing = device.ResolvePath("maps\\missing.map");
  File* f4 = nullptr;
  CHECK(missing && missing->Open(rex::filesystem::FileAccess::kGenericRead, &f4) == X_STATUS_SUCCESS);
  if (f4) {
    ReadAll(f4, 0, 10, &got, &st);
    CHECK(st == X_STATUS_UNSUCCESSFUL);
    f4->Destroy();
  }

  // 8. Concurrent readers on a fresh, memory-only device.
  {
    auto o = Options();
    o.persistent_cache_dir.clear();
    o.memory_cache_blocks = 4;
    HttpRangeDevice conc("\\Device\\Web\\Game3", o);
    CHECK(conc.Initialize());
    Entry* e = conc.ResolvePath("maps\\Test.map");
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        File* f = nullptr;
        if (e->Open(rex::filesystem::FileAccess::kGenericRead, &f) != X_STATUS_SUCCESS) {
          ++bad;
          return;
        }
        uint64_t x = 12345 + t;
        for (int i = 0; i < 200; ++i) {
          x = x * 6364136223846793005ull + 1442695040888963407ull;
          uint64_t off = (x >> 20) % kMapSize;
          size_t len = size_t((x >> 8) % 200000);
          size_t g = 0;
          X_STATUS s = 0;
          std::string d = ReadAll(f, off, len, &g, &s);
          if (s != X_STATUS_SUCCESS || g != std::min<uint64_t>(len, kMapSize - off) ||
              !Matches(d, off)) {
            ++bad;
          }
        }
        f->Destroy();
      });
    }
    for (auto& th : threads) th.join();
    CHECK(bad == 0);
  }

  std::printf("transport requests: %.0f\n", test_request_count());
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  std::printf("%s http_range_device\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
