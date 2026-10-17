/**
 * @file        tests/web/host_path_device_test.cpp
 * @brief       Node-runnable test of the writable HostPathDevice on WasmFS (web cache:/xstorage:):
 *              read/write handles, create -> write -> close -> reopen -> delete-on-close ->
 *              enumerate, delete with two open handles, non-empty directory delete, and a second
 *              device on the same folder (what a page reload sees).
 *
 * Reproduces the Halo 3 second-boot hang (docs/port/vfs.md 5): a read/write handle was opened
 * write-only (POSIX O_RDONLY|O_WRONLY == O_WRONLY), so preferences.dat read back as defaults, and
 * FileDispositionInformation was ignored, so "find first, mark delete, close" never emptied
 * cache:\maps\cached_fonts. Build + run: tests/web/run_host_path_device_test.bat. Under node the
 * WasmFS root is the in-memory backend; OPFS itself is covered by the browser gate run
 * (server/tools/gate1.mjs --runs 2 --opfs-dump cache,xstorage).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>

using rex::X_STATUS;
using rex::filesystem::Entry;
using rex::filesystem::File;
using rex::filesystem::FileAccess;
using rex::filesystem::FileAction;
using rex::filesystem::FileDisposition;
using rex::filesystem::HostPathDevice;
using rex::filesystem::VirtualFileSystem;

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

constexpr uint32_t kReadWrite = FileAccess::kGenericRead | FileAccess::kGenericWrite;
const char* kHost = "/tmp/rex_host_path_test/cache";

bool HostExists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

std::unique_ptr<VirtualFileSystem> Mount() {
  auto vfs = std::make_unique<VirtualFileSystem>();
  auto dev = std::make_unique<HostPathDevice>("\\CACHE", kHost, false);
  if (!dev->Initialize()) return nullptr;
  if (!vfs->RegisterDevice(std::move(dev))) return nullptr;
  vfs->RegisterSymbolicLink("cache:", "\\CACHE");
  return vfs;
}

File* Open(VirtualFileSystem* vfs, const char* path, FileDisposition disp, uint32_t access,
           bool dir = false) {
  File* f = nullptr;
  FileAction action;
  X_STATUS st = vfs->OpenFile(nullptr, path, disp, access, dir, !dir, &f, &action);
  return st == 0 ? f : nullptr;
}

std::vector<std::string> Children(VirtualFileSystem* vfs, const char* dir) {
  std::vector<std::string> names;
  Entry* e = vfs->ResolvePath(dir);
  if (e) {
    for (const auto& c : e->children()) names.push_back(c->name());
  }
  return names;
}

bool Has(const std::vector<std::string>& v, const char* n) {
  for (const auto& s : v)
    if (s == n) return true;
  return false;
}

}  // namespace

int main() {
  std::printf("host_path_device_test (WasmFS, %s)\n", kHost);
  std::string host_fonts = std::string(kHost) + "/maps/cached_fonts";
  auto vfs = Mount();
  CHECK(vfs != nullptr);
  if (!vfs) return 1;

  // 1. Halo's preferences.dat pattern: one read/write handle used for write, then read back.
  const char payload[] = "preferences: font date 01DD5DBF80155000";
  {
    File* f = Open(vfs.get(), "cache:\\preferences.dat", FileDisposition::kOverwriteIf, kReadWrite);
    CHECK(f != nullptr);
    if (f) {
      size_t n = 0;
      CHECK(f->WriteSync({reinterpret_cast<const uint8_t*>(payload), sizeof(payload)}, 0, &n) == 0);
      CHECK(n == sizeof(payload));
      char back[sizeof(payload)] = {};
      size_t r = 0;
      CHECK(f->ReadSync({reinterpret_cast<uint8_t*>(back), sizeof(back)}, 0, &r) == 0);
      CHECK(r == sizeof(payload) && std::memcmp(back, payload, sizeof(payload)) == 0);
      f->Destroy();
    }
  }

  // 2. Font cache rebuild: create dir + files, write, close.
  CHECK(vfs->CreatePath("cache:\\maps\\cached_fonts", rex::filesystem::kFileAttributeDirectory));
  const char* fonts[] = {"lucida_console-18", "font_table.txt", "fixedsys-9"};
  for (const char* name : fonts) {
    std::string p = std::string("cache:\\maps\\cached_fonts\\") + name;
    File* f = Open(vfs.get(), p.c_str(), FileDisposition::kCreate, kReadWrite);
    CHECK(f != nullptr);
    if (f) {
      size_t n = 0;
      CHECK(f->WriteSync({reinterpret_cast<const uint8_t*>(name), std::strlen(name)}, 0, &n) == 0);
      f->Destroy();
    }
  }
  CHECK(Children(vfs.get(), "cache:\\maps\\cached_fonts").size() == 3);

  // 3. "Reload": a second device on the same folder sees the files and reads preferences.dat
  //    through a read/write handle.
  vfs.reset();
  vfs = Mount();
  CHECK(vfs != nullptr);
  if (!vfs) return 1;
  CHECK(Children(vfs.get(), "cache:\\maps\\cached_fonts").size() == 3);
  {
    File* f = Open(vfs.get(), "cache:\\preferences.dat", FileDisposition::kOpenIf, kReadWrite);
    CHECK(f != nullptr);
    if (f) {
      CHECK(f->entry()->size() == sizeof(payload));
      char back[sizeof(payload)] = {};
      size_t r = 0;
      CHECK(f->ReadSync({reinterpret_cast<uint8_t*>(back), sizeof(back)}, 0, &r) == 0);
      CHECK(r == sizeof(payload) && std::memcmp(back, payload, sizeof(payload)) == 0);
      f->Destroy();
    }
  }

  // 4. Halo's wipe loop: enumerate, open the first file, mark delete-on-close, close; repeat.
  int rounds = 0;
  while (rounds < 10) {
    auto names = Children(vfs.get(), "cache:\\maps\\cached_fonts");
    if (names.empty()) break;
    std::string p = "cache:\\maps\\cached_fonts\\" + names[0];
    File* f = Open(vfs.get(), p.c_str(), FileDisposition::kOpen,
                   FileAccess::kGenericRead | 0x00010000 /* DELETE */);
    CHECK(f != nullptr);
    if (!f) break;
    f->entry()->SetForDeletion(true);
    f->Destroy();
    CHECK(!HostExists(host_fonts + "/" + names[0]));
    ++rounds;
  }
  CHECK(rounds == 3);
  CHECK(Children(vfs.get(), "cache:\\maps\\cached_fonts").empty());

  // 5. Delete-on-close with two handles: the file goes away only after the last close.
  {
    File* a = Open(vfs.get(), "cache:\\maps\\cached_fonts\\two", FileDisposition::kCreate, kReadWrite);
    File* b = Open(vfs.get(), "cache:\\maps\\cached_fonts\\two", FileDisposition::kOpen, kReadWrite);
    CHECK(a != nullptr && b != nullptr);
    if (a && b) {
      a->entry()->SetForDeletion(true);
      a->Destroy();
      CHECK(HostExists(host_fonts + "/two"));
      CHECK(Has(Children(vfs.get(), "cache:\\maps\\cached_fonts"), "two"));
      b->Destroy();
      CHECK(!HostExists(host_fonts + "/two"));
      CHECK(!Has(Children(vfs.get(), "cache:\\maps\\cached_fonts"), "two"));
    }
  }

  // 6. Un-marking before close keeps the file.
  {
    File* f = Open(vfs.get(), "cache:\\maps\\cached_fonts\\keep", FileDisposition::kCreate, kReadWrite);
    CHECK(f != nullptr);
    if (f) {
      f->entry()->SetForDeletion(true);
      f->entry()->SetForDeletion(false);
      f->Destroy();
      CHECK(HostExists(host_fonts + "/keep"));
    }
  }

  // 7. A non-empty directory is not deleted on close (NT: STATUS_DIRECTORY_NOT_EMPTY); an empty
  //    one is.
  {
    File* d = Open(vfs.get(), "cache:\\maps\\cached_fonts", FileDisposition::kOpen,
                   FileAccess::kGenericRead, true);
    CHECK(d != nullptr);
    if (d) {
      d->entry()->SetForDeletion(true);
      d->Destroy();
      CHECK(HostExists(host_fonts));
    }
    CHECK(vfs->DeletePath("cache:\\maps\\cached_fonts\\keep"));
    d = Open(vfs.get(), "cache:\\maps\\cached_fonts", FileDisposition::kOpen,
             FileAccess::kGenericRead, true);
    CHECK(d != nullptr);
    if (d) {
      d->entry()->SetForDeletion(true);
      d->Destroy();
      CHECK(!HostExists(host_fonts));
      CHECK(!Has(Children(vfs.get(), "cache:\\maps"), "cached_fonts"));
    }
  }

  // 8. Reload again: the deletions persisted, preferences.dat is still there.
  vfs.reset();
  vfs = Mount();
  CHECK(vfs != nullptr);
  if (vfs) {
    CHECK(vfs->ResolvePath("cache:\\maps\\cached_fonts") == nullptr);
    CHECK(Has(Children(vfs.get(), "cache:\\"), "preferences.dat"));
  }

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
