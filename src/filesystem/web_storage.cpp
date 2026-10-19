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

#include <emscripten/em_asm.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging.h>

extern "C" {
struct Backend;
typedef struct Backend* backend_t;
backend_t wasmfs_create_opfs_backend(void) __attribute__((weak));
int wasmfs_create_directory(const char* path, mode_t mode, backend_t backend)
    __attribute__((weak));
}

// Browser main thread (run through emscripten_async_run_in_main_runtime_thread).
EM_JS(void, rex_storage_js_request_persist, (), {
  const st = Module['rexStorage'] || (Module['rexStorage'] = {});
  const hook = (state) => {
    console.log('[storage] persistent storage: ' + state);
    if (typeof Module['rexOnStoragePersist'] == 'function') {
      try {
        Module['rexOnStoragePersist']({state : state});
      } catch (e) {
        console.error(e);
      }
    }
  };
  const s = typeof navigator != 'undefined' ? navigator.storage : null;
  if (!s || !s.persist || !s.persisted) {
    hook('unsupported');
    return;
  }
  const refresh = () => {
    if (st.refreshEstimate) st.refreshEstimate();
  };
  const gestures = [ 'pointerdown', 'keydown', 'touchend' ];
  const request = () => {
    for (const g of gestures) window.removeEventListener(g, request, true);
    if (st.persistRequested) return;
    st.persistRequested = true;
    hook('requesting');
    s.persist().then((granted) => { hook(granted ? 'granted' : 'denied'); refresh(); },
                     (e) => { hook('denied'); refresh(); });
  };
  s.persisted().then((p) => {
    if (p) {
      hook('already');
      refresh();
      return;
    }
    // Firefox needs transient activation for its prompt; Chromium does not mind.
    if (navigator.userActivation && navigator.userActivation.isActive) {
      request();
    } else {
      hook('waiting');  // for the first pointer / key / touch gesture
      for (const g of gestures) window.addEventListener(g, request, true);
    }
  }, () => hook('unsupported'));
});

EM_JS(void, rex_storage_js_refresh_estimate, (), {
  const st = Module['rexStorage'] || (Module['rexStorage'] = {});
  const s = typeof navigator != 'undefined' ? navigator.storage : null;
  const report = Module['_rex_web_storage_estimate_result'] ||
                 wasmExports['rex_web_storage_estimate_result'];
  if (!s || !s.estimate || !report) return;
  if (!st.refreshEstimate) {
    st.refreshEstimate = () => {
      Promise.all([ s.estimate(), s.persisted ? s.persisted() : Promise.resolve(null) ])
          .then(([ e, p ]) => {
            const usage = e.usage || 0, quota = e.quota || 0;
            const fsu = e.usageDetails && typeof e.usageDetails.fileSystem == 'number'
                            ? e.usageDetails.fileSystem
                            : -1;
            const persisted = p === null ? -1 : (p ? 1 : 0);
            report(usage, quota, fsu, persisted);
            const key = Math.round(usage / 1048576) + '/' + Math.round(quota / 1048576) + '/' +
                        persisted;
            if (key != st.estimateKey) {
              st.estimateKey = key;
              console.log('[storage] usage ' + (usage / 1048576).toFixed(1) + ' MiB' +
                          (fsu >= 0 ? ' (OPFS ' + (fsu / 1048576).toFixed(1) + ' MiB)' : '') +
                          ' of ' + (quota / 1048576).toFixed(0) + ' MiB quota, persisted ' +
                          (persisted < 0 ? 'unknown' : persisted ? 'yes' : 'no'));
            }
          }, () => {});
    };
    setInterval(st.refreshEstimate, 30000);
  }
  st.refreshEstimate();
});

EM_JS(void, rex_storage_js_tab_role, (int max_slots), {
  const maxSlots = max_slots;
  const report = Module['_rex_web_tab_role_result'] || wasmExports['rex_web_tab_role_result'];
  let forced = -1;
  try {
    const q = new URLSearchParams(typeof location != 'undefined' ? location.search : '');
    const v = q.get('rex_slot') ?? q.get('profile');
    if (v !== null && String(parseInt(v, 10)) === v && parseInt(v, 10) >= 0) {
      forced = parseInt(v, 10);
    }
  } catch (e) {
  }
  const locks = typeof navigator != 'undefined' ? navigator.locks : null;
  if (!locks || !report) {
    if (report) report(1, forced >= 0 ? forced : 0, 0, forced >= 0 ? 1 : 0);
    return;
  }
  const hold = () => new Promise(() => {});  // Released when the tab goes away.
  const st = Module['rexStorage'] || (Module['rexStorage'] = {});
  const takeSlot = (writer) => {
    if (forced >= 0) {
      locks.request('rex-tab-slot-' + forced, {ifAvailable : true}, (l) => {
        if (!l) console.warn('[storage] tab slot ' + forced + ' (URL) is in use by another tab');
        st.slot = forced;
        report(writer, forced, 1, 1);
        return l ? hold() : undefined;
      });
      return;
    }
    const next = (n) => {
      if (n >= maxSlots) {
        console.warn('[storage] all ' + maxSlots + ' tab slots in use; sharing slot 0');
        st.slot = 0;
        report(writer, 0, 1, 0);
        return;
      }
      locks.request('rex-tab-slot-' + n, {ifAvailable : true}, (l) => {
        if (!l) {
          next(n + 1);
          return;
        }
        st.slot = n;
        report(writer, n, 1, 0);
        return hold();
      });
    };
    next(0);
  };
  // A reloading page may still hold the writer lock for a moment: wait up to 1.5 s.
  const options = typeof AbortSignal != 'undefined' && AbortSignal.timeout
                      ? {signal : AbortSignal.timeout(1500)}
                      : {ifAvailable : true};
  locks
      .request('rex-storage-writer', options,
               (l) => {
                 st.storageWriter = !!l;
                 takeSlot(l ? 1 : 0);
                 return l ? hold() : undefined;
               })
      .catch(() => {
        if (st.storageWriter === undefined) {
          st.storageWriter = false;
          takeSlot(0);
        }
      });
});

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
  RefreshStorageEstimate();
  return mounted;
}

// ---- Persistence and quota ---------------------------------------------------------------

namespace {

struct EstimateState {
  std::atomic<bool> valid{false};
  std::atomic<double> usage{0.0};
  std::atomic<double> quota{0.0};
  std::atomic<double> file_system_usage{-1.0};
  std::atomic<int> persisted{-1};
  std::atomic<uint64_t> sequence{0};
};
EstimateState& Estimate() {
  static EstimateState state;
  return state;
}

}  // namespace

void RequestPersistentStorage() {
  static std::atomic<bool> requested{false};
  if (requested.exchange(true)) {
    return;
  }
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, reinterpret_cast<void*>(&rex_storage_js_request_persist));
}

void RefreshStorageEstimate() {
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, reinterpret_cast<void*>(&rex_storage_js_refresh_estimate));
}

StorageEstimate GetStorageEstimate() {
  EstimateState& state = Estimate();
  StorageEstimate estimate;
  estimate.valid = state.valid.load(std::memory_order_acquire);
  estimate.usage = state.usage.load(std::memory_order_relaxed);
  estimate.quota = state.quota.load(std::memory_order_relaxed);
  estimate.file_system_usage = state.file_system_usage.load(std::memory_order_relaxed);
  estimate.persisted = state.persisted.load(std::memory_order_relaxed);
  estimate.sequence = state.sequence.load(std::memory_order_relaxed);
  return estimate;
}

// ---- Storage pressure ----------------------------------------------------------------------

namespace {

struct EvictorRegistry {
  std::mutex mutex;
  uint32_t next_id = 1;
  std::vector<std::pair<uint32_t, std::pair<std::string, StorageEvictor>>> evictors;
};
EvictorRegistry& Evictors() {
  static EvictorRegistry registry;
  return registry;
}

}  // namespace

uint32_t RegisterStorageEvictor(std::string name, StorageEvictor evictor) {
  EvictorRegistry& registry = Evictors();
  std::lock_guard<std::mutex> lock(registry.mutex);
  uint32_t id = registry.next_id++;
  registry.evictors.push_back({id, {std::move(name), std::move(evictor)}});
  return id;
}

void UnregisterStorageEvictor(uint32_t id) {
  EvictorRegistry& registry = Evictors();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (auto it = registry.evictors.begin(); it != registry.evictors.end(); ++it) {
    if (it->first == id) {
      registry.evictors.erase(it);
      return;
    }
  }
}

uint64_t RelieveStoragePressure(uint64_t bytes) {
  // Copied out so an evictor may take its own locks (and register / unregister).
  std::vector<std::pair<std::string, StorageEvictor>> evictors;
  {
    EvictorRegistry& registry = Evictors();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (const auto& entry : registry.evictors) {
      evictors.push_back(entry.second);
    }
  }
  uint64_t freed = 0;
  for (const auto& [name, evictor] : evictors) {
    if (freed >= bytes) {
      break;
    }
    uint64_t evicted = evictor(bytes - freed);
    freed += evicted;
    REXFS_WARN("web storage: storage pressure: {} freed {} KiB", name, evicted >> 10);
  }
  StorageEstimate estimate = GetStorageEstimate();
  REXFS_WARN(
      "web storage: storage pressure ({} KiB wanted, {} KiB freed); last estimate {:.1f} of "
      "{:.1f} MiB, persisted {}",
      bytes >> 10, freed >> 10, estimate.usage / 1048576.0, estimate.quota / 1048576.0,
      estimate.persisted);
  RefreshStorageEstimate();
  return freed;
}

// ---- Tabs --------------------------------------------------------------------------------

namespace {

struct TabRoleState {
  std::mutex mutex;
  bool decided = false;
  TabRole role;
  // Main thread -> caller.
  std::atomic<int> answered{0};
  std::atomic<int> writer{1};
  std::atomic<int> slot{0};
  std::atomic<int> from_locks{0};
  std::atomic<int> forced{0};
};
TabRoleState& TabRoles() {
  static TabRoleState state;
  return state;
}

}  // namespace

TabRole AcquireTabRole(uint32_t max_slots) {
  TabRoleState& state = TabRoles();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.decided) {
    return state.role;
  }
  state.decided = true;
  if (emscripten_is_main_browser_thread()) {
    REXFS_ERROR("web storage: AcquireTabRole must not run on the browser main thread");
    return state.role;
  }
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, reinterpret_cast<void*>(&rex_storage_js_tab_role), int(max_slots));
  // The answer comes from the main thread's lock callbacks (normally well under 2 s).
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!state.answered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (state.answered.load(std::memory_order_acquire)) {
    state.role.storage_writer = state.writer.load(std::memory_order_relaxed) != 0;
    state.role.slot = uint32_t(std::max(0, state.slot.load(std::memory_order_relaxed)));
    state.role.from_locks = state.from_locks.load(std::memory_order_relaxed) != 0;
    state.role.slot_forced = state.forced.load(std::memory_order_relaxed) != 0;
  } else {
    REXFS_WARN("web storage: no Web Locks answer within 5 s; acting as the only tab");
  }
  REXFS_INFO("web storage: tab role: {} storage writer, slot {}{}{}",
             state.role.storage_writer ? "the" : "not the", state.role.slot,
             state.role.slot_forced ? " (from the URL)" : "",
             state.role.from_locks ? "" : " (no Web Locks)");
  return state.role;
}

bool IsStorageWriter() {
  TabRoleState& state = TabRoles();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.role.storage_writer;
}

}  // namespace rex::filesystem::web

// Browser main thread (navigator.storage.estimate / persisted answered).
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_storage_estimate_result(double usage, double quota,
                                                                     double file_system_usage,
                                                                     int persisted) {
  auto& state = rex::filesystem::web::Estimate();
  state.usage.store(usage, std::memory_order_relaxed);
  state.quota.store(quota, std::memory_order_relaxed);
  state.file_system_usage.store(file_system_usage, std::memory_order_relaxed);
  state.persisted.store(persisted, std::memory_order_relaxed);
  state.sequence.fetch_add(1, std::memory_order_relaxed);
  state.valid.store(true, std::memory_order_release);
}

// Browser main thread (Web Locks answered).
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_tab_role_result(int writer, int slot, int from_locks,
                                                             int forced) {
  auto& state = rex::filesystem::web::TabRoles();
  state.writer.store(writer, std::memory_order_relaxed);
  state.slot.store(slot, std::memory_order_relaxed);
  state.from_locks.store(from_locks, std::memory_order_relaxed);
  state.forced.store(forced, std::memory_order_relaxed);
  state.answered.store(1, std::memory_order_release);
}
