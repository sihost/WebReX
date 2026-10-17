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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
#ifdef __EMSCRIPTEN__
const std::string kHost = "/tmp/rex_host_path_test/cache";
#else
// Native: also covers the Windows HostPathDevice (the flag was never acted on there before).
const std::string kHost =
    (std::filesystem::temp_directory_path() / "rex_host_path_test" / "cache").generic_string();
#endif

bool HostExists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(p), ec);
}

// Delete-on-close policy for Mount(): the platform default (on web, off natively) unless set.
int g_policy = -1;  // -1 default, 0 off, 1 on
std::vector<std::string> g_keep;

std::unique_ptr<VirtualFileSystem> Mount() {
  auto vfs = std::make_unique<VirtualFileSystem>();
  auto dev = std::make_unique<HostPathDevice>("\\CACHE", kHost, false);
  if (g_policy >= 0) dev->SetDeleteOnClose(g_policy == 1, g_keep);
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

// Halo 3 autosave_queue_save_item_remove_stale_items (0x822C5C78): count the items in
// cache:\autosave; while 25 or more exist, delete the oldest and run again. Returns how many
// rounds ran before fewer than 25 were left (max_rounds = it never got there).
int AutosavePrune(VirtualFileSystem* vfs, int max_rounds) {
  int rounds = 0;
  for (; rounds < max_rounds; ++rounds) {
    auto names = Children(vfs, "cache:\\autosave");
    if (names.size() < 25) break;
    std::sort(names.begin(), names.end());  // names sort like their write times here
    std::string p = "cache:\\autosave\\" + names[0];
    File* f = Open(vfs, p.c_str(), FileDisposition::kOpen,
                   FileAccess::kGenericRead | 0x00010000 /* DELETE */);
    if (!f) break;
    f->entry()->SetForDeletion(true);
    f->Destroy();
  }
  return rounds;
}

void MakeFile(VirtualFileSystem* vfs, const std::string& guest_path) {
  File* f = Open(vfs, guest_path.c_str(), FileDisposition::kCreate, kReadWrite);
  CHECK(f != nullptr);
  if (f) {
    size_t n = 0;
    f->WriteSync({reinterpret_cast<const uint8_t*>("x"), 1}, 0, &n);
    f->Destroy();
  }
}

void MarkAndClose(VirtualFileSystem* vfs, const std::string& guest_path) {
  File* f = Open(vfs, guest_path.c_str(), FileDisposition::kOpen,
                 FileAccess::kGenericRead | 0x00010000 /* DELETE */);
  CHECK(f != nullptr);
  if (f) {
    f->entry()->SetForDeletion(true);
    f->Destroy();
  }
}

// 9-11: the device policy (HostPathDevice::SetDeleteOnClose), incl. the native default.
void PolicyTests() {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(kHost), ec);
  const std::string host_autosave = kHost + "/autosave";
  const std::string host_films = kHost + "/saved_films";

  // 9. Policy off (the native default before this change, kept for devices that do not opt in):
  //    the marked file stays, and the autosave prune never gets below 25.
  g_policy = 0;
  g_keep.clear();
  auto vfs = Mount();
  CHECK(vfs != nullptr);
  if (!vfs) return;
  CHECK(vfs->CreatePath("cache:\\autosave", rex::filesystem::kFileAttributeDirectory));
  CHECK(vfs->CreatePath("cache:\\saved_films", rex::filesystem::kFileAttributeDirectory));
  for (int i = 0; i < 25; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "cache:\\autosave\\asq%016X.slayer", 0x6AD2B585 + i);
    MakeFile(vfs.get(), name);
  }
  MakeFile(vfs.get(), "cache:\\saved_films\\riverwo_F9EB78A3.film");
  CHECK(Children(vfs.get(), "cache:\\autosave").size() == 25);
  CHECK(AutosavePrune(vfs.get(), 50) == 50);  // the native hang
  CHECK(HostExists(host_autosave + "/asq000000006AD2B585.slayer"));

  // 10. Policy on with saved_films kept (what Halo 3 mounts natively): the prune deletes exactly
  //     the oldest item; a marked film stays on the host and in the VFS.
  g_policy = 1;
  g_keep = {"Saved_Films"};
  vfs.reset();
  vfs = Mount();
  CHECK(vfs != nullptr);
  if (!vfs) return;
  CHECK(AutosavePrune(vfs.get(), 50) == 1);
  CHECK(!HostExists(host_autosave + "/asq000000006AD2B585.slayer"));
  CHECK(HostExists(host_autosave + "/asq000000006AD2B586.slayer"));
  CHECK(Children(vfs.get(), "cache:\\autosave").size() == 24);
  MarkAndClose(vfs.get(), "cache:\\saved_films\\riverwo_F9EB78A3.film");
  CHECK(HostExists(host_films + "/riverwo_F9EB78A3.film"));
  CHECK(Has(Children(vfs.get(), "cache:\\saved_films"), "riverwo_F9EB78A3.film"));
  // A prefix only matches whole path components (saved_films_x is not kept).
  MakeFile(vfs.get(), "cache:\\saved_films_x");
  MarkAndClose(vfs.get(), "cache:\\saved_films_x");
  CHECK(!HostExists(kHost + "/saved_films_x"));

  // 11. Policy on, nothing kept: the film goes too.
  g_keep.clear();
  vfs.reset();
  vfs = Mount();
  CHECK(vfs != nullptr);
  if (!vfs) return;
  MarkAndClose(vfs.get(), "cache:\\saved_films\\riverwo_F9EB78A3.film");
  CHECK(!HostExists(host_films + "/riverwo_F9EB78A3.film"));
  vfs.reset();
  g_policy = -1;
  std::filesystem::remove_all(std::filesystem::path(kHost), ec);
}

int main() {
  std::printf("host_path_device_test (%s)\n", kHost.c_str());
#ifndef __EMSCRIPTEN__
  // Natively the default leaves marked entries alone; sections 1-8 test NT semantics.
  g_policy = 1;
  {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(kHost), ec);
  }
#endif
  std::string host_fonts = kHost + "/maps/cached_fonts";
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
  vfs.reset();

  PolicyTests();

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
