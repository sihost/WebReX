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

#include <string>
#include <string_view>

namespace rex::filesystem::web {

// Returns the mount point on success (idempotent), or "" if OPFS is unavailable.
std::string MountPersistentStorage(std::string_view mount_point = "/opfs");

}  // namespace rex::filesystem::web
