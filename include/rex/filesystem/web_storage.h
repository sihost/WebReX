/**
 * @file        rex/filesystem/web_storage.h
 * @brief       Web-only: persistent browser storage (OPFS) for the VFS.
 *
 * On Emscripten builds linked with -sWASMFS, MountPersistentStorage() mounts the Origin
 * Private File System at `mount_point` with the WasmFS OPFS backend. Plain POSIX / std::filesystem
 * I/O under that directory is then persistent and synchronous from any pthread (WasmFS proxies
 * it to its OPFS worker, which holds FileSystemSyncAccessHandles). Use it for:
 *   - xstorage: (HostPathDevice over "<mount>/xstorage", writable), and
 *   - the HttpRangeDevice persistent block cache ("<mount>/rex-cache").
 *
 * Without -sWASMFS (or off the web) it returns an empty string and storage is not persistent.
 * Must not be called on the browser main thread (PROXY_TO_PTHREAD main() is fine).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace rex::filesystem::web {

// Returns the mount point on success (idempotent), or "" if OPFS is unavailable.
std::string MountPersistentStorage(std::string_view mount_point = "/opfs");

// ---- Persistence and quota -------------------------------------------------------------
// OPFS starts "best-effort": under storage pressure the browser may evict the whole origin,
// saves included. RequestPersistentStorage asks for navigator.storage.persist() (Chromium
// decides without a prompt; Firefox prompts and needs a gesture): at once when the page has
// transient user activation, else at the first pointer / key / touch gesture. The page may
// define Module.rexOnStoragePersist(event) to explain it; it gets {state: "waiting" (for a
// gesture) | "requesting" | "granted" | "denied" | "already" | "unsupported"}. Any thread,
// idempotent.
void RequestPersistentStorage();

struct StorageEstimate {
  bool valid = false;  // false until the first navigator.storage.estimate() answered
  double usage = 0.0;  // bytes, whole origin
  double quota = 0.0;
  double file_system_usage = -1.0;  // usageDetails.fileSystem (Chromium: OPFS), -1 if unknown
  double caches_usage = -1.0;       // usageDetails.caches (Cache Storage: a service worker's
                                    // data chunk store), -1 if unknown
  double indexed_db_usage = -1.0;   // usageDetails.indexedDB, -1 if unknown
  // Chrome's StaticStorageQuota: quota == usage + 10 GiB whatever the real limit is, so the
  // quota says nothing (a write fails first); false when it looks real.
  bool quota_padded = false;
  int persisted = -1;               // navigator.storage.persisted(): 1 / 0, -1 unknown
  uint64_t sequence = 0;            // incremented by every answer
};
// The last estimate. The main thread refreshes it every 30 s once MountPersistentStorage ran
// (and logs a line to the console when usage, quota or persistence changed); Refresh asks
// for a new one now (asynchronous).
StorageEstimate GetStorageEstimate();
void RefreshStorageEstimate();

// ---- Storage pressure ------------------------------------------------------------------
// Persistent data (saves, cache:, xstorage:) and evictable caches (the HttpRangeDevice block
// cache, the WGSL shader storage) share the origin's quota. Evictable caches register a
// callback that deletes cached files to free at least `bytes` and returns the bytes freed.
using StorageEvictor = std::function<uint64_t(uint64_t bytes)>;
uint32_t RegisterStorageEvictor(std::string name, StorageEvictor evictor);
void UnregisterStorageEvictor(uint32_t id);
// Writers of persistent data call this after a write failed (quota exceeded / EIO): runs the
// evictors (in registration order) until `bytes` are freed; returns the total freed.
uint64_t RelieveStoragePressure(uint64_t bytes);

// ---- Several tabs of the same origin ---------------------------------------------------
// OPFS files are opened with exclusive FileSystemSyncAccessHandles, so two tabs cannot both
// keep the same cache file open (the second gets EACCES), and they would share one profile
// and one set of saves. AcquireTabRole coordinates through Web Locks (navigator.locks),
// holding the locks for the tab's lifetime:
//  - storage_writer: this tab got "rex-storage-writer" (waiting up to 1.5 s, so a reload
//    gets it back from the unloading page): it owns the writable caches (block cache, shader
//    storage). Other tabs should keep their caches in memory.
//  - slot: the first free "rex-tab-slot-N" (N < max_slots), or the page URL's ?rex_slot=N /
//    ?profile=N. Slot 0 keeps the default save locations and profile; slot N > 0 should use
//    its own save roots and profile (gamertag / XUID cvars, see user_profile.cpp).
// Without Web Locks (or OPFS) every tab is writer and slot 0, as before. Blocking (not on
// the browser main thread); the first call decides, later calls return the same role.
struct TabRole {
  bool storage_writer = true;
  uint32_t slot = 0;
  bool from_locks = false;  // false: Web Locks unavailable / not asked (defaults)
  bool slot_forced = false;  // the slot came from the URL
};
TabRole AcquireTabRole(uint32_t max_slots = 8);
// AcquireTabRole().storage_writer if it ran, else true.
bool IsStorageWriter();

}  // namespace rex::filesystem::web
