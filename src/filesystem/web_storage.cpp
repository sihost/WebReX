/**
 * @file        filesystem/web_storage.cpp
 * @brief       Web-only: mount OPFS through the WasmFS OPFS backend (see web_storage.h).
 *
 * The WasmFS entry points are declared weak so the SDK links with or without -sWASMFS; without
 * it they resolve to null and persistence is reported as unavailable.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/filesystem/web_storage.h>

#include <rex/platform.h>

static_assert(REX_PLATFORM_WEB, "web_storage.cpp is web-only (Emscripten)");

#include <emscripten/threading.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <mutex>
#include <string>

#include <rex/logging.h>

extern "C" {
struct Backend;
typedef struct Backend* backend_t;
backend_t wasmfs_create_opfs_backend(void) __attribute__((weak));
int wasmfs_create_directory(const char* path, mode_t mode, backend_t backend)
    __attribute__((weak));
}

namespace rex::filesystem::web {

std::string MountPersistentStorage(std::string_view mount_point) {
  static std::mutex mutex;
  static std::string mounted;
  std::lock_guard<std::mutex> lock(mutex);
  if (!mounted.empty()) {
    return mounted;
  }
  if (!wasmfs_create_opfs_backend || !wasmfs_create_directory) {
    REXFS_WARN("web storage: built without -sWASMFS; OPFS unavailable, saves are not persistent");
    return {};
  }
  if (emscripten_is_main_browser_thread()) {
    REXFS_ERROR("web storage: MountPersistentStorage must not run on the browser main thread");
    return {};
  }
  backend_t backend = wasmfs_create_opfs_backend();
  if (!backend) {
    REXFS_ERROR("web storage: OPFS backend creation failed (no navigator.storage?)");
    return {};
  }
  std::string path(mount_point);
  if (wasmfs_create_directory(path.c_str(), 0777, backend) != 0) {
    REXFS_ERROR("web storage: cannot mount OPFS at {} (errno {})", path, errno);
    return {};
  }
  REXFS_INFO("web storage: OPFS mounted at {}", path);
  mounted = path;
  return mounted;
}

}  // namespace rex::filesystem::web
