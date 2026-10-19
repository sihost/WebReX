/**
 * @file        filesystem/devices/http_range_device.cpp
 * @brief       Web-only read-only VFS device over HTTP Range requests with OPFS block cache.
 *
 * See rex/filesystem/devices/http_range_device.h and docs/port/vfs.md (project repo).
 *
 * Transport: synchronous XMLHttpRequest from a pthread. Every guest file read runs on a pthread
 * (a Worker; PROXY_TO_PTHREAD keeps even main() off the browser main thread), and sync XHR with
 * responseType "arraybuffer" is allowed in Workers. Parallelism comes from more pthreads: the
 * missing blocks of a read are split into pieces, the reader fetches one and a pool of fetch
 * threads (--vfs_http_fetch_threads) the others, and sequential readers get blocks read ahead
 * (--vfs_http_read_ahead_blocks) on the same pool. Under node (tests) the same entry points
 * read from the local directory named by $REX_WEB_TEST_DATA_ROOT instead.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/filesystem/devices/http_range_device.h>

#include <rex/platform.h>

static_assert(REX_PLATFORM_WEB, "HttpRangeDevice is web-only (Emscripten)");

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <span>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/web_storage.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/string/utf8.h>

REXCVAR_DEFINE_INT32(vfs_http_retry_budget_ms, 60000, "Filesystem",
                     "Web HttpRangeDevice: how long a game data read keeps retrying a network "
                     "error (or HTTP 5xx/408/429) with exponential backoff before the guest gets "
                     "an I/O error. Time the browser reports offline does not count. 0 = one "
                     "immediate retry only")
    .range(0, 3600000);
REXCVAR_DEFINE_INT32(vfs_http_retry_max_delay_ms, 5000, "Filesystem",
                     "Web HttpRangeDevice: longest pause between two retries of a failing read "
                     "(the pause doubles from 250 ms up to this)")
    .range(50, 60000);
REXCVAR_DEFINE_INT32(vfs_http_fetch_threads, 4, "Filesystem",
                     "Web HttpRangeDevice: fetch threads (Workers, one sync Range request each) "
                     "that download the missing blocks of a read in parallel with the reader "
                     "and run read-ahead. 0 = one request at a time on the reader, no read-ahead")
    .range(0, 16);
REXCVAR_DEFINE_INT32(vfs_http_read_ahead_blocks, 8, "Filesystem",
                     "Web HttpRangeDevice: blocks fetched ahead of a file read sequentially "
                     "(needs vfs_http_fetch_threads > 0; capped at half the memory cache). 0 = off")
    .range(0, 64);
REXCVAR_DEFINE_INT32(vfs_cache_max_mb, 0, "Filesystem",
                     "Web HttpRangeDevice: size cap of the persistent (OPFS) block cache in MiB; "
                     "its least recently used files are deleted to stay below it. 0 = no fixed "
                     "cap (still bounded by vfs_cache_reserve_mb)")
    .range(0, 1 << 20);
REXCVAR_DEFINE_INT32(vfs_cache_reserve_mb, 256, "Filesystem",
                     "Web HttpRangeDevice: MiB of the origin's storage quota kept free for saves: "
                     "the block cache stops growing and evicts its least recently used files when "
                     "navigator.storage.estimate() reports less than this left")
    .range(0, 1 << 20);

// -------------------------------------------------------------------------------------------
// JS transport. All pointers and 64-bit integers are passed as doubles (exact below 2^53), which
// avoids BigInt handling for wasm64 pointers in EM_JS.
// -------------------------------------------------------------------------------------------

// Text results are handed over in two steps (length, then copy into a C++ buffer) so the JS
// side never calls malloc: no -sEXPORTED_FUNCTIONS=_malloc requirement on the final link.
EM_JS_DEPS(rex_http_range_device, "$UTF8ToString,$lengthBytesUTF8,$stringToUTF8");

// Reads bytes [offset, offset + length) of `url` into wasm memory at `dst`.
// Returns the number of bytes copied (>= 0), or -HTTP status (e.g. -412 ETag mismatch,
// -404, -416), or -1 for a network error.
// clang-format off
EM_JS(double, rex_web_http_read_range,
      (double url_ptr, double offset, double length, double dst, double if_match_ptr), {
  var url = UTF8ToString(url_ptr);
  var ifMatch = if_match_ptr ? UTF8ToString(if_match_ptr) : "";
  if (length <= 0) return 0;
  if (typeof ENVIRONMENT_IS_NODE != "undefined" && ENVIRONMENT_IS_NODE) {
    var root = process.env["REX_WEB_TEST_DATA_ROOT"];
    if (!root) return -1;
    var fs = require("fs");
    var urlPath = url;
    var scheme = urlPath.indexOf("://");
    if (scheme >= 0) {
      var slash = urlPath.indexOf("/", scheme + 3);
      urlPath = slash >= 0 ? urlPath.substring(slash) : "/";
    }
    var fsPath = root + decodeURIComponent(urlPath);
    var fd;
    try { fd = fs.openSync(fsPath, "r"); } catch (e) { return -404; }
    try {
      var size = fs.fstatSync(fd).size;
      if (offset >= size) return -416;
      var n = Math.min(length, size - offset);
      var buf = Buffer.alloc(n);
      var got = fs.readSync(fd, buf, 0, n, offset);
      HEAPU8.set(buf.subarray(0, got), dst);
      globalThis["__rexWebTestRequests"] = (globalThis["__rexWebTestRequests"] || 0) + 1;
      return got;
    } finally { fs.closeSync(fd); }
  }
  var xhr = new XMLHttpRequest();
  xhr.open("GET", url, false);
  xhr.responseType = "arraybuffer";
  xhr.setRequestHeader("Range", "bytes=" + offset + "-" + (offset + length - 1));
  if (ifMatch) xhr.setRequestHeader("If-Match", ifMatch);
  try { xhr.send(null); } catch (e) { return -1; }
  if (xhr.status !== 206 && xhr.status !== 200) return -(xhr.status || 1);
  var body = new Uint8Array(xhr.response);
  if (xhr.status === 200) {
    // The server ignored Range (violates the contract); still correct, just wasteful.
    body = body.subarray(offset, offset + length);
  }
  var n2 = Math.min(body.length, length);
  HEAPU8.set(body.subarray(0, n2), dst);
  return n2;
});

// 1 if the browser reports no network (navigator.onLine === false in this Worker), else 0.
EM_JS(int, rex_web_is_offline, (), {
  return (typeof navigator != 'undefined' && navigator.onLine === false) ? 1 : 0;
});

// Browser main thread: Module.rexOnIoStall({path, attempt, waitedMs, offline, done, ok}) so the
// page can show "reconnecting" while a read retries (done = it went through or gave up).
EM_JS(void, rex_web_notify_io_stall,
      (double path_ptr, int attempt, double waited_ms, int offline, int done, int ok), {
  var cb = Module['rexOnIoStall'];
  if (typeof cb != 'function') return;
  try {
    cb({path: UTF8ToString(path_ptr), attempt: attempt, waitedMs: waited_ms, offline: !!offline,
        done: !!done, ok: !!ok});
  } catch (e) {
    console.error(e);
  }
});

// Copies the pending text (see below) into dst (capacity cap, NUL-terminated) and clears it.
EM_JS(void, rex_web_take_text, (double dst, double cap), {
  stringToUTF8(globalThis["__rexPendingText"] || "", dst, cap);
  globalThis["__rexPendingText"] = undefined;
});

// GET `url` as text into the calling thread's pending-text slot. Returns its UTF-8 byte length,
// or -1 on failure.
EM_JS(double, rex_web_http_get_text, (double url_ptr), {
  var url = UTF8ToString(url_ptr);
  var text;
  if (typeof ENVIRONMENT_IS_NODE != "undefined" && ENVIRONMENT_IS_NODE) {
    var root = process.env["REX_WEB_TEST_DATA_ROOT"];
    if (!root) return -1;
    try { text = require("fs").readFileSync(root + decodeURIComponent(url), "utf8"); }
    catch (e) { return -1; }
  } else {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", url, false);
    try { xhr.send(null); } catch (e) { return -1; }
    if (xhr.status !== 200) return -1;
    text = xhr.responseText;
  }
  globalThis["__rexPendingText"] = text;
  return lengthBytesUTF8(text);
});

// Parses the manifest JSON and flattens it to "size\tsha256\tmtime\tpath\n" lines in the
// pending-text slot. Returns the UTF-8 byte length, or -1 if the JSON is invalid.
EM_JS(double, rex_web_manifest_to_tsv, (double json_ptr), {
  try {
    var m = JSON.parse(UTF8ToString(json_ptr));
    var lines = [];
    var TAB = String.fromCharCode(9);
    var NL = String.fromCharCode(10);
    var CR = String.fromCharCode(13);
    for (var f of (m["files"] || [])) {
      var p = String(f["path"] || "");
      if (!p || p.indexOf(TAB) >= 0 || p.indexOf(NL) >= 0 || p.indexOf(CR) >= 0) continue;
      var size = Number(f["size"]);
      if (!Number.isSafeInteger(size) || size < 0) continue;
      var sha = String(f["sha256"] || "").toLowerCase();
      if (!new RegExp("^[0-9a-f]*$").test(sha)) sha = "";
      var mtime = Number(f["mtime"] || 0) || 0;
      lines.push(size + TAB + sha + TAB + mtime + TAB + p);
    }
    var text = lines.join(NL);
    globalThis["__rexPendingText"] = text;
    return lengthBytesUTF8(text);
  } catch (e) {
    return -1;
  }
});
// clang-format on

namespace rex::filesystem {

namespace {

double PtrArg(const void* ptr) {
  return static_cast<double>(reinterpret_cast<uintptr_t>(ptr));
}

struct IoStallNotice {
  std::string path;
  int attempt;
  double waited_ms;
  int offline, done, ok;
};

// Any thread (a reader or a fetch thread): posts the stall status to the page.
void NotifyIoStall(const std::string& path, int attempt, double waited_ms, bool offline,
                   bool done, bool ok) {
  auto* n = new IoStallNotice{path, attempt, waited_ms, offline, done, ok};
  auto run = [](void* arg) {
    auto* s = static_cast<IoStallNotice*>(arg);
    rex_web_notify_io_stall(PtrArg(s->path.c_str()), s->attempt, s->waited_ms, s->offline,
                            s->done, s->ok);
    delete s;
  };
  if (emscripten_is_main_runtime_thread()) {
    run(n);
  } else if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(),
                                     emscripten_main_runtime_thread_id(), run, n)) {
    delete n;
  }
}

// Worth retrying: network error (-1, includes CORS/abort), server errors and the transient
// client statuses. 404/412/416 and other 4xx fail at once (retrying cannot fix them).
bool IsRetryable(double got) {
  int status = int(-got);
  return got == -1 || status == 408 || status == 425 || status == 429 || status >= 500;
}

// Collects the pending text after a successful rex_web_* call that returned `length`.
bool TakeJsText(double length, std::string* out) {
  if (length < 0) {
    return false;
  }
  std::vector<char> buffer(size_t(length) + 1);
  rex_web_take_text(PtrArg(buffer.data()), double(buffer.size()));
  out->assign(buffer.data(), size_t(length));
  return true;
}

std::string PercentEncodeSegment(std::string_view segment) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(segment.size());
  for (unsigned char c : segment) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out.push_back(char(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
  return out;
}

uint64_t UnixToFileTime(double unix_seconds) {
  if (unix_seconds <= 0) {
    return 0;
  }
  return static_cast<uint64_t>((unix_seconds + 11644473600.0) * 10000000.0);
}

}  // namespace

// -------------------------------------------------------------------------------------------
// Entry / File
// -------------------------------------------------------------------------------------------

class HttpRangeFile final : public File {
 public:
  HttpRangeFile(uint32_t file_access, Entry* entry, HttpRangeDevice* device, int64_t file_index)
      : File(file_access, entry),
        device_(device),
        file_index_(file_index) {}

  void Destroy() override { delete this; }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override {
    if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
      return X_STATUS_ACCESS_DENIED;
    }
    if (file_index_ < 0) {
      return X_STATUS_FILE_IS_A_DIRECTORY;
    }
    if (!device_->Read(size_t(file_index_), byte_offset, buffer.data(), buffer.size(),
                       out_bytes_read)) {
      return X_STATUS_UNSUCCESSFUL;
    }
    return X_STATUS_SUCCESS;
  }

  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* out_bytes_written) override {
    if (out_bytes_written) {
      *out_bytes_written = 0;
    }
    return X_STATUS_ACCESS_DENIED;
  }

  X_STATUS SetLength(size_t) override { return X_STATUS_ACCESS_DENIED; }

 private:
  HttpRangeDevice* device_;
  int64_t file_index_;
};

class HttpRangeEntry final : public Entry {
 public:
  HttpRangeEntry(HttpRangeDevice* device, Entry* parent, const std::string_view path,
                 int64_t file_index)
      : Entry(device, parent, path), file_index_(file_index) {}

  X_STATUS Open(uint32_t desired_access, File** out_file) override {
    if (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData)) {
      REXFS_ERROR("HttpRangeDevice: write access requested for read-only {}", path());
      return X_STATUS_ACCESS_DENIED;
    }
    *out_file = new HttpRangeFile(desired_access, this, static_cast<HttpRangeDevice*>(device_),
                                  file_index_);
    return X_STATUS_SUCCESS;
  }

  static HttpRangeEntry* MakeDirectory(HttpRangeDevice* device, Entry* parent,
                                       const std::string_view path) {
    auto* entry = new HttpRangeEntry(device, parent, path, -1);
    entry->attributes_ = kFileAttributeDirectory;
    return entry;
  }

  static HttpRangeEntry* MakeFile(HttpRangeDevice* device, Entry* parent,
                                  const std::string_view path, size_t file_index,
                                  const HttpRangeFileInfo& info) {
    auto* entry = new HttpRangeEntry(device, parent, path, int64_t(file_index));
    entry->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
    entry->size_ = size_t(info.size);
    entry->allocation_size_ = rex::round_up(size_t(info.size), size_t(device->bytes_per_sector()));
    entry->create_timestamp_ = info.write_timestamp;
    entry->access_timestamp_ = info.write_timestamp;
    entry->write_timestamp_ = info.write_timestamp;
    return entry;
  }

  HttpRangeEntry* FindChild(std::string_view name) {
    return static_cast<HttpRangeEntry*>(GetChild(name));
  }
  HttpRangeEntry* AddChild(HttpRangeEntry* child) {
    children_.push_back(std::unique_ptr<Entry>(child));
    return child;
  }
  bool is_directory() const { return file_index_ < 0; }

 private:
  int64_t file_index_;
};

// -------------------------------------------------------------------------------------------
// Persistent (OPFS) block cache: <dir>/<key>.data (sparse) + <key>.map (1 bit per block).
// A loader may store a complete copy as <key>.data plus an empty <key>.complete marker.
// -------------------------------------------------------------------------------------------

struct HttpRangeDevice::PersistentFile {
  std::string base;  // <dir>/<key>
  int data_fd = -1;
  int map_fd = -1;
  bool complete = false;
  std::vector<uint8_t> bitmap;

  ~PersistentFile() {
    if (data_fd >= 0) close(data_fd);
    if (map_fd >= 0) close(map_fd);
  }
  bool Has(uint32_t block) const {
    return complete || (block / 8 < bitmap.size() && (bitmap[block / 8] >> (block % 8)) & 1);
  }
};

// -------------------------------------------------------------------------------------------
// Fetch pool: threads that run FetchClaimed for pieces of demand reads (first) and read-ahead.
// -------------------------------------------------------------------------------------------

struct HttpRangeDevice::FetchJob {
  enum class State { kQueued, kRunning, kDone };
  uint32_t file = 0;
  uint32_t first = 0;
  uint32_t last = 0;
  std::vector<uint8_t> data;
  bool ok = false;
  State state = State::kQueued;  // guarded by FetchPool::mutex
};

struct HttpRangeDevice::FetchPool {
  std::mutex mutex;
  std::condition_variable work_cv;  // workers: a job was queued or stop
  std::condition_variable done_cv;  // readers: a job finished
  std::deque<std::shared_ptr<FetchJob>> demand;
  std::deque<std::shared_ptr<FetchJob>> ahead;
  std::vector<std::thread> threads;
  uint32_t thread_count = 0;
  bool stop = false;

  // Removes `job` from the demand queue if no worker took it yet (the reader runs it itself).
  bool TakeBackLocked(const std::shared_ptr<FetchJob>& job) {
    auto it = std::find(demand.begin(), demand.end(), job);
    if (it == demand.end()) return false;
    demand.erase(it);
    job->state = FetchJob::State::kRunning;
    return true;
  }
};

HttpRangeDevice::HttpRangeDevice(const std::string_view mount_path,
                                 HttpRangeDeviceOptions options)
    : Device(mount_path), options_(std::move(options)) {
  options_.block_size = std::max<uint32_t>(options_.block_size, 4096);
  options_.max_request_bytes = std::max(options_.max_request_bytes, options_.block_size);
}

HttpRangeDevice::~HttpRangeDevice() {
  if (evictor_id_) {
    web::UnregisterStorageEvictor(evictor_id_);
    evictor_id_ = 0;
  }
  StopPool();
}

void HttpRangeDevice::StartPool() {
  int32_t threads = options_.fetch_threads >= 0 ? options_.fetch_threads
                                                : REXCVAR_GET(vfs_http_fetch_threads);
  int32_t ahead = options_.read_ahead_blocks >= 0 ? options_.read_ahead_blocks
                                                  : REXCVAR_GET(vfs_http_read_ahead_blocks);
  threads = std::clamp(threads, 0, 16);
  read_ahead_ = threads > 0 ? uint32_t(std::clamp(ahead, 0, 64)) : 0;
  read_ahead_ = std::min(read_ahead_, options_.memory_cache_blocks / 2);
  if (threads <= 0 || pool_) return;
  pool_ = std::make_unique<FetchPool>();
  pool_->thread_count = uint32_t(threads);
  for (int32_t i = 0; i < threads; ++i) {
    pool_->threads.emplace_back([this] {
      FetchPool& pool = *pool_;
      std::unique_lock<std::mutex> lock(pool.mutex);
      for (;;) {
        pool.work_cv.wait(lock, [&] { return pool.stop || !pool.demand.empty() || !pool.ahead.empty(); });
        if (pool.stop) return;
        auto& queue = !pool.demand.empty() ? pool.demand : pool.ahead;
        std::shared_ptr<FetchJob> job = std::move(queue.front());
        queue.pop_front();
        job->state = FetchJob::State::kRunning;
        lock.unlock();
        stat_pool_requests_.fetch_add(1, std::memory_order_relaxed);
        job->ok = FetchClaimed(job->file, job->first, job->last, job->data);
        lock.lock();
        job->state = FetchJob::State::kDone;
        pool.done_cv.notify_all();
      }
    });
  }
}

void HttpRangeDevice::StopPool() {
  if (!pool_) return;
  std::vector<std::shared_ptr<FetchJob>> dropped;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    pool_->stop = true;
    dropped.assign(pool_->ahead.begin(), pool_->ahead.end());
    pool_->ahead.clear();
  }
  pool_->work_cv.notify_all();
  for (auto& t : pool_->threads) {
    if (t.joinable()) t.join();
  }
  // Release the claims of read-ahead that never ran (no reader can be left at this point).
  std::lock_guard<std::mutex> lock(cache_mutex_);
  for (auto& job : dropped) {
    for (uint32_t b = job->first; b <= job->last; ++b) inflight_.erase(BlockKey{job->file, b});
  }
  pool_.reset();
}

bool HttpRangeDevice::Initialize() {
  std::string json;
  if (!TakeJsText(rex_web_http_get_text(PtrArg(options_.manifest_url.c_str())), &json) ||
      json.empty()) {
    REXFS_ERROR("HttpRangeDevice: could not fetch manifest {}", options_.manifest_url);
    return false;
  }
  return InitializeFromManifest(json);
}

bool HttpRangeDevice::InitializeFromManifest(const std::string& manifest_json) {
  std::string tsv;
  if (!TakeJsText(rex_web_manifest_to_tsv(PtrArg(manifest_json.c_str())), &tsv)) {
    REXFS_ERROR("HttpRangeDevice: manifest is not valid JSON");
    return false;
  }
  files_.clear();
  size_t pos = 0;
  while (pos < tsv.size()) {
    size_t end = tsv.find('\n', pos);
    if (end == std::string::npos) end = tsv.size();
    std::string_view line(tsv.data() + pos, end - pos);
    pos = end + 1;
    size_t t1 = line.find('\t');
    size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
    size_t t3 = t2 == std::string_view::npos ? t2 : line.find('\t', t2 + 1);
    if (t3 == std::string_view::npos) continue;
    HttpRangeFileInfo info;
    info.size = std::strtoull(std::string(line.substr(0, t1)).c_str(), nullptr, 10);
    info.sha256 = std::string(line.substr(t1 + 1, t2 - t1 - 1));
    info.write_timestamp = UnixToFileTime(std::strtod(std::string(line.substr(t2 + 1, t3 - t2 - 1)).c_str(), nullptr));
    info.path = std::string(line.substr(t3 + 1));
    std::replace(info.path.begin(), info.path.end(), '\\', '/');
    while (!info.path.empty() && info.path.front() == '/') info.path.erase(info.path.begin());
    if (!info.path.empty()) files_.push_back(std::move(info));
  }
  if (files_.empty()) {
    REXFS_ERROR("HttpRangeDevice: manifest has no usable files");
    return false;
  }
  if (files_.size() > UINT32_MAX) {
    return false;
  }

  if (!options_.persistent_cache_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(options_.persistent_cache_dir, ec);
    persistent_enabled_ = std::filesystem::is_directory(options_.persistent_cache_dir, ec);
    if (!persistent_enabled_) {
      REXFS_WARN("HttpRangeDevice: persistent cache dir {} unusable; memory cache only",
                 options_.persistent_cache_dir);
    } else {
      {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        ScanPersistentCacheLocked();
      }
      // Saves come first: a failing save write frees block cache files (web_storage.h).
      if (!evictor_id_) {
        evictor_id_ = web::RegisterStorageEvictor(
            "game data block cache " + options_.persistent_cache_dir, [this](uint64_t bytes) {
              std::lock_guard<std::mutex> lock(cache_mutex_);
              return EvictPersistentLocked(bytes, std::string());
            });
      }
    }
  }

  if (!BuildTree()) {
    return false;
  }
  StartPool();
  REXFS_INFO(
      "HttpRangeDevice: {} files from {} (base {}, block {} KiB, cache {}, fetch threads {}, "
      "read-ahead {} blocks)",
      files_.size(), options_.manifest_url, options_.base_url, options_.block_size / 1024,
      persistent_enabled_ ? options_.persistent_cache_dir : std::string("memory only"),
      pool_ ? pool_->thread_count : 0, read_ahead_);
  return true;
}

bool HttpRangeDevice::BuildTree() {
  auto* root = HttpRangeEntry::MakeDirectory(this, nullptr, "");
  root_entry_.reset(root);
  for (size_t i = 0; i < files_.size(); ++i) {
    const auto& info = files_[i];
    HttpRangeEntry* dir = root;
    std::string_view rest(info.path);
    for (;;) {
      size_t slash = rest.find('/');
      std::string_view part = rest.substr(0, slash);
      if (part.empty()) {
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
        continue;
      }
      std::string guest_path = rex::string::utf8_join_guest_paths(dir->path(), part);
      HttpRangeEntry* child = dir->FindChild(part);
      if (slash == std::string_view::npos) {
        if (child) {
          REXFS_WARN("HttpRangeDevice: duplicate manifest path {}", info.path);
        } else {
          dir->AddChild(HttpRangeEntry::MakeFile(this, dir, guest_path, i, info));
        }
        break;
      }
      if (!child) {
        child = dir->AddChild(HttpRangeEntry::MakeDirectory(this, dir, guest_path));
      } else if (!child->is_directory()) {
        REXFS_WARN("HttpRangeDevice: {} is both a file and a directory", guest_path);
        break;
      }
      dir = child;
      rest.remove_prefix(slash + 1);
    }
  }
  return true;
}

void HttpRangeDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HttpRangeDevice::ResolvePath(const std::string_view path) {
  // The VFS strips the mount prefix: "maps\shared.map". Lookups are case-insensitive.
  return root_entry_ ? root_entry_->ResolvePath(path) : nullptr;
}

std::string HttpRangeDevice::UrlFor(const HttpRangeFileInfo& file) const {
  std::string url = options_.base_url;
  if (!url.empty() && url.back() != '/') url.push_back('/');
  std::string_view rest(file.path);
  bool first = true;
  while (!rest.empty()) {
    size_t slash = rest.find('/');
    if (!first) url.push_back('/');
    url += PercentEncodeSegment(rest.substr(0, slash));
    first = false;
    if (slash == std::string_view::npos) break;
    rest.remove_prefix(slash + 1);
  }
  return url;
}

uint32_t HttpRangeDevice::BlockCount(const HttpRangeFileInfo& file) const {
  return uint32_t((file.size + options_.block_size - 1) / options_.block_size);
}

size_t HttpRangeDevice::BlockBytes(const HttpRangeFileInfo& file, uint32_t block) const {
  uint64_t start = uint64_t(block) * options_.block_size;
  return size_t(std::min<uint64_t>(options_.block_size, file.size - start));
}

HttpRangeDevice::PersistentFile* HttpRangeDevice::GetPersistentFile(uint32_t file) {
  // cache_mutex_ held.
  if (!persistent_enabled_) return nullptr;
  auto it = persistent_.find(file);
  if (it != persistent_.end()) return it->second.get();

  const auto& info = files_[file];
  std::string key = info.sha256;
  if (key.empty()) {
    // No content hash: key by path + size (a changed file of the same size would be stale;
    // the server contract requires sha256).
    key = "p_" + std::to_string(std::hash<std::string>{}(info.path)) + "_" +
          std::to_string(info.size);
  }
  std::string base = options_.persistent_cache_dir + "/" + key;
  auto pf = std::make_unique<PersistentFile>();
  pf->base = base;
  TouchPersistentLocked(base, 0);
  struct stat st {};
  if (::stat((base + ".complete").c_str(), &st) == 0 && ::stat((base + ".data").c_str(), &st) == 0 &&
      uint64_t(st.st_size) == info.size) {
    pf->complete = true;
    pf->data_fd = ::open((base + ".data").c_str(), O_RDONLY);
  } else {
    pf->data_fd = ::open((base + ".data").c_str(), O_RDWR | O_CREAT, 0644);
    pf->map_fd = ::open((base + ".map").c_str(), O_RDWR | O_CREAT, 0644);
    pf->bitmap.assign((BlockCount(info) + 7) / 8, 0);
    if (pf->map_fd >= 0) {
      ssize_t got = ::pread(pf->map_fd, pf->bitmap.data(), pf->bitmap.size(), 0);
      (void)got;  // a short/new map file leaves the remaining bits zero
    }
  }
  if (pf->data_fd < 0 || (!pf->complete && pf->map_fd < 0)) {
    REXFS_WARN("HttpRangeDevice: cannot open persistent cache for {} ({})", info.path, errno);
    pf.reset();
  }
  auto* raw = pf.get();
  persistent_.emplace(file, std::move(pf));  // a null entry disables it for this file
  return raw;
}

bool HttpRangeDevice::ReadPersistentBlock(uint32_t file, uint32_t block,
                                          std::vector<uint8_t>& out) {
  // cache_mutex_ held.
  PersistentFile* pf = GetPersistentFile(file);
  if (!pf || !pf->Has(block)) return false;
  TouchPersistentLocked(pf->base, 0);
  size_t bytes = BlockBytes(files_[file], block);
  out.resize(bytes);
  ssize_t got = ::pread(pf->data_fd, out.data(), bytes, off_t(uint64_t(block) * options_.block_size));
  return got == ssize_t(bytes);
}

void HttpRangeDevice::WritePersistentBlocks(uint32_t file, uint32_t first_block,
                                            const uint8_t* data, size_t length) {
  // cache_mutex_ held.
  PersistentFile* pf = GetPersistentFile(file);
  if (!pf || pf->complete) return;
  off_t offset = off_t(uint64_t(first_block) * options_.block_size);
  // The .data file grows to the end of the furthest block written (OPFS files are not
  // sparse), plus the bitmap.
  struct stat st {};
  uint64_t old_size = ::fstat(pf->data_fd, &st) == 0 ? uint64_t(st.st_size) : 0;
  uint64_t new_size = std::max<uint64_t>(old_size, uint64_t(offset) + length);
  const std::string base = pf->base;
  if (new_size > old_size && !ReservePersistentLocked(new_size - old_size, base)) {
    ++stat_skipped_writes_;
    return;  // no room: these blocks stay memory-only
  }
  // Eviction never closes the file being written, so pf is still valid.
  if (::pwrite(pf->data_fd, data, length, offset) != ssize_t(length)) {
    // Quota or I/O error: don't mark the blocks, and make room for the next writes (saves
    // included) right away.
    REXFS_WARN("HttpRangeDevice: block cache write of {} KiB failed ({}); evicting", length >> 10,
               errno);
    EvictPersistentLocked(uint64_t(length) * 4, base);
    ++stat_skipped_writes_;
    return;
  }
  TouchPersistentLocked(base, new_size + pf->bitmap.size());
  uint32_t last_block = first_block + uint32_t((length + options_.block_size - 1) / options_.block_size) - 1;
  for (uint32_t b = first_block; b <= last_block; ++b) {
    pf->bitmap[b / 8] |= uint8_t(1u << (b % 8));
  }
  // Data first, then the bits that vouch for it.
  size_t byte0 = first_block / 8, byte1 = last_block / 8;
  ::pwrite(pf->map_fd, pf->bitmap.data() + byte0, byte1 - byte0 + 1, off_t(byte0));
}

bool HttpRangeDevice::CopyCachedBlock(uint32_t file, uint32_t block, uint64_t in_block_offset,
                                      uint8_t* dst, size_t length) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto it = lru_index_.find(BlockKey{file, block});
  if (it != lru_index_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);
    std::memcpy(dst, it->second->data.data() + in_block_offset, length);
    stat_memory_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  std::vector<uint8_t> data;
  if (!ReadPersistentBlock(file, block, data)) return false;
  std::memcpy(dst, data.data() + in_block_offset, length);
  stat_persistent_hits_.fetch_add(1, std::memory_order_relaxed);
  // Promote to memory.
  lru_.push_front(CachedBlock{BlockKey{file, block}, std::move(data)});
  lru_index_[BlockKey{file, block}] = lru_.begin();
  while (lru_.size() > options_.memory_cache_blocks) {
    lru_index_.erase(lru_.back().key);
    lru_.pop_back();
  }
  return true;
}

void HttpRangeDevice::InsertMemoryBlock(uint32_t file, uint32_t block, const uint8_t* data,
                                        size_t length) {
  // cache_mutex_ held.
  if (options_.memory_cache_blocks == 0) return;
  BlockKey key{file, block};
  auto it = lru_index_.find(key);
  if (it != lru_index_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);
    return;
  }
  lru_.push_front(CachedBlock{key, std::vector<uint8_t>(data, data + length)});
  lru_index_[key] = lru_.begin();
  while (lru_.size() > options_.memory_cache_blocks) {
    lru_index_.erase(lru_.back().key);
    lru_.pop_back();
  }
}

bool HttpRangeDevice::FetchBlocks(uint32_t file, uint32_t first, uint32_t last,
                                  std::vector<uint8_t>& out) {
  const auto& info = files_[file];
  uint64_t offset = uint64_t(first) * options_.block_size;
  uint64_t end = std::min<uint64_t>(uint64_t(last + 1) * options_.block_size, info.size);
  size_t length = size_t(end - offset);
  out.resize(length);
  std::string url = UrlFor(info);
  std::string if_match = info.sha256.empty() ? std::string() : "\"" + info.sha256 + "\"";
  // Retries (network errors, 5xx/408/425/429): the first at once, then with exponential backoff
  // (250 ms doubling to vfs_http_retry_max_delay_ms) until vfs_http_retry_budget_ms of online
  // time has passed; while the browser reports offline, wait for it to come back (not counted).
  // A Wi-Fi or tunnel blip during a map load then delays the read instead of failing it. All
  // state is local: fetch threads run this concurrently.
  using Clock = std::chrono::steady_clock;
  const auto budget = std::chrono::milliseconds(REXCVAR_GET(vfs_http_retry_budget_ms));
  const auto max_delay = std::chrono::milliseconds(REXCVAR_GET(vfs_http_retry_max_delay_ms));
  const Clock::time_point start = Clock::now();
  Clock::duration offline_time{0};
  auto delay = std::chrono::milliseconds(250);
  bool stalled = false;
  Clock::time_point last_notice{};
  auto waited_ms = [&] {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };
  for (int attempt = 1;; ++attempt) {
    stat_requests_.fetch_add(1, std::memory_order_relaxed);
    double got = rex_web_http_read_range(PtrArg(url.c_str()), double(offset), double(length),
                                         PtrArg(out.data()),
                                         if_match.empty() ? 0.0 : PtrArg(if_match.c_str()));
    if (got >= 0) {
      stat_bytes_.fetch_add(uint64_t(got), std::memory_order_relaxed);
      bool ok = size_t(got) == length;
      if (!ok) {
        REXFS_ERROR("HttpRangeDevice: short response for {} [{}+{}]: {} bytes", info.path, offset,
                    length, size_t(got));
        stat_failed_reads_.fetch_add(1, std::memory_order_relaxed);
      } else if (stalled) {
        REXFS_WARN("HttpRangeDevice: {} [{}+{}] read again after {} attempts, {:.0f} ms",
                   info.path, offset, length, attempt, waited_ms());
      }
      if (stalled) NotifyIoStall(info.path, attempt, waited_ms(), false, true, ok);
      return ok;
    }
    if (got == -412) {
      REXFS_ERROR("HttpRangeDevice: {} changed on the server (ETag != sha256 {}); reload the page",
                  info.path, info.sha256);
      stat_failed_reads_.fetch_add(1, std::memory_order_relaxed);
      if (stalled) NotifyIoStall(info.path, attempt, waited_ms(), false, true, false);
      return false;
    }
    if (!IsRetryable(got)) {
      REXFS_ERROR("HttpRangeDevice: HTTP {} for {} [{}+{}]", -got, info.path, offset, length);
      stat_failed_reads_.fetch_add(1, std::memory_order_relaxed);
      if (stalled) NotifyIoStall(info.path, attempt, waited_ms(), false, true, false);
      return false;
    }
    if (Clock::now() - start - offline_time >= budget) {
      REXFS_ERROR(
          "HttpRangeDevice: {} reading {} [{}+{}], giving up after {} attempts in {:.0f} ms "
          "(vfs_http_retry_budget_ms {})",
          got == -1 ? std::string("network error") : fmt::format("HTTP {}", -got), info.path,
          offset, length, attempt, waited_ms(), budget.count());
      stat_failed_reads_.fetch_add(1, std::memory_order_relaxed);
      if (stalled) NotifyIoStall(info.path, attempt, waited_ms(), false, true, false);
      return false;
    }
    stat_retries_.fetch_add(1, std::memory_order_relaxed);
    if (attempt == 1) {
      continue;  // one immediate retry (as before): most blips are a single dropped request
    }
    bool offline = rex_web_is_offline() != 0;
    if (!stalled) {
      stalled = true;
      stat_stalls_.fetch_add(1, std::memory_order_relaxed);
      REXFS_WARN("HttpRangeDevice: {} reading {} [{}+{}]; retrying with backoff for up to {} ms{}",
                 got == -1 ? std::string("network error") : fmt::format("HTTP {}", -got),
                 info.path, offset, length, budget.count(),
                 offline ? " (browser offline: waiting for the network)" : "");
    }
    // At most one page notice a second per read.
    if (Clock::now() - last_notice >= std::chrono::seconds(1)) {
      last_notice = Clock::now();
      NotifyIoStall(info.path, attempt, waited_ms(), offline, false, false);
    }
    if (offline) {
      // Poll navigator.onLine (cheap) instead of burning requests; retry once it is back.
      Clock::time_point offline_start = Clock::now();
      while (rex_web_is_offline() && Clock::now() - offline_start < std::chrono::minutes(30)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (Clock::now() - last_notice >= std::chrono::seconds(1)) {
          last_notice = Clock::now();
          NotifyIoStall(info.path, attempt, waited_ms(), true, false, false);
        }
      }
      offline_time += Clock::now() - offline_start;
      delay = std::chrono::milliseconds(250);
      continue;
    }
    std::this_thread::sleep_for(delay);
    delay = std::min<std::chrono::milliseconds>(delay * 2, max_delay);
  }
}

bool HttpRangeDevice::IsCachedLocked(uint32_t file, uint32_t block) {
  if (lru_index_.count(BlockKey{file, block})) return true;
  PersistentFile* pf = GetPersistentFile(file);
  return pf && pf->Has(block);
}

bool HttpRangeDevice::FetchClaimed(uint32_t file, uint32_t first, uint32_t last,
                                   std::vector<uint8_t>& out) {
  const bool ok = FetchBlocks(file, first, last, out);
  const auto& info = files_[file];
  const uint32_t bs = options_.block_size;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (ok) {
      for (uint32_t b = first; b <= last; ++b) {
        InsertMemoryBlock(file, b, out.data() + uint64_t(b - first) * bs, BlockBytes(info, b));
      }
      WritePersistentBlocks(file, first, out.data(), out.size());
    }
    for (uint32_t b = first; b <= last; ++b) {
      inflight_.erase(BlockKey{file, b});
    }
  }
  inflight_cv_.notify_all();
  return ok;
}

void HttpRangeDevice::MaybeReadAhead(uint32_t file, uint32_t first, uint32_t last) {
  if (!pool_ || read_ahead_ == 0) return;
  {
    // Enough read-ahead queued already (the pool is the bottleneck): skip.
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->ahead.size() >= size_t(pool_->thread_count) * 2) return;
  }
  const uint32_t count = BlockCount(files_[file]);
  const uint32_t piece_blocks =
      std::clamp<uint32_t>((read_ahead_ + pool_->thread_count - 1) / pool_->thread_count, 1,
                           std::max<uint32_t>(1, options_.max_request_bytes / options_.block_size));
  std::vector<std::shared_ptr<FetchJob>> jobs;
  uint64_t blocks = 0;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = last_read_block_.find(file);
    const bool sequential =
        it != last_read_block_.end() && first >= it->second && first <= it->second + 1;
    last_read_block_[file] = last;
    if (!sequential) return;
    const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(last) + read_ahead_, count - 1));
    for (uint64_t b = uint64_t(last) + 1; b <= end; ++b) {
      BlockKey key{file, uint32_t(b)};
      if (inflight_.count(key) || IsCachedLocked(file, uint32_t(b))) continue;
      inflight_.insert(key);
      ++blocks;
      if (!jobs.empty() && jobs.back()->last + 1 == b &&
          jobs.back()->last - jobs.back()->first + 1 < piece_blocks) {
        jobs.back()->last = uint32_t(b);
      } else {
        auto job = std::make_shared<FetchJob>();
        job->file = file;
        job->first = job->last = uint32_t(b);
        jobs.push_back(std::move(job));
      }
    }
  }
  if (jobs.empty()) return;
  stat_read_ahead_blocks_.fetch_add(blocks, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    for (auto& job : jobs) pool_->ahead.push_back(std::move(job));
  }
  pool_->work_cv.notify_all();
}

bool HttpRangeDevice::Read(size_t file_index, uint64_t offset, uint8_t* buffer, size_t length,
                           size_t* out_bytes_read) {
  *out_bytes_read = 0;
  if (file_index >= files_.size()) return false;
  const uint32_t file = uint32_t(file_index);
  const auto& info = files_[file];
  if (offset >= info.size || length == 0) return true;  // EOF, like pread
  length = size_t(std::min<uint64_t>(length, info.size - offset));

  const uint32_t bs = options_.block_size;
  const uint32_t first = uint32_t(offset / bs);
  const uint32_t last = uint32_t((offset + length - 1) / bs);
  const uint32_t max_blocks = std::max<uint32_t>(1, options_.max_request_bytes / bs);
  const uint64_t read_end = offset + length;

  auto copy_part = [&](uint32_t block, const uint8_t* block_data) {
    uint64_t block_start = uint64_t(block) * bs;
    uint64_t from = std::max(offset, block_start);
    uint64_t to = std::min<uint64_t>(read_end, block_start + BlockBytes(info, block));
    std::memcpy(buffer + (from - offset), block_data + (from - block_start), size_t(to - from));
  };
  auto copy_cached = [&](uint32_t block) {
    uint64_t block_start = uint64_t(block) * bs;
    uint64_t from = std::max(offset, block_start);
    uint64_t to = std::min<uint64_t>(read_end, block_start + BlockBytes(info, block));
    return CopyCachedBlock(file, block, from - block_start, buffer + (from - offset),
                           size_t(to - from));
  };

  // 1. Cached blocks.
  std::vector<uint32_t> missing;
  for (uint32_t b = first; b <= last; ++b) {
    if (!copy_cached(b)) missing.push_back(b);
  }

  if (!missing.empty()) {
    // 2. Claim the missing blocks nobody is downloading yet, as runs of consecutive blocks.
    // Blocks already in flight (read-ahead, another reader) are waited for in step 4.
    struct Run {
      uint32_t first, last;
    };
    std::vector<Run> runs;
    std::vector<uint32_t> waits;
    uint32_t claimed = 0;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      for (uint32_t b : missing) {
        BlockKey key{file, b};
        if (inflight_.count(key) || IsCachedLocked(file, b)) {
          waits.push_back(b);
          continue;
        }
        inflight_.insert(key);
        ++claimed;
        if (!runs.empty() && runs.back().last + 1 == b) {
          runs.back().last = b;
        } else {
          runs.push_back(Run{b, b});
        }
      }
    }

    // 3. One piece per thread (the reader + the pool), each one Range request of at most
    // max_request_bytes. The reader fetches the first piece and every piece no fetch thread has
    // started yet, so a busy pool never makes a read slower than fetching alone.
    const uint32_t parallel = pool_ ? pool_->thread_count + 1 : 1;
    const uint32_t piece_blocks =
        std::clamp<uint32_t>((claimed + parallel - 1) / parallel, 1, max_blocks);
    std::vector<std::shared_ptr<FetchJob>> jobs;
    for (const Run& r : runs) {
      for (uint64_t b = r.first; b <= r.last; b += piece_blocks) {
        auto job = std::make_shared<FetchJob>();
        job->file = file;
        job->first = uint32_t(b);
        job->last = uint32_t(std::min<uint64_t>(r.last, b + piece_blocks - 1));
        jobs.push_back(std::move(job));
      }
    }
    const bool use_pool = pool_ && jobs.size() > 1;
    if (use_pool) {
      {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        for (size_t i = 1; i < jobs.size(); ++i) pool_->demand.push_back(jobs[i]);
      }
      pool_->work_cv.notify_all();
    }
    for (size_t i = 0; i < jobs.size(); ++i) {
      bool mine = i == 0 || !use_pool;
      if (!mine) {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        mine = pool_->TakeBackLocked(jobs[i]);
      }
      if (mine) {
        jobs[i]->ok = FetchClaimed(file, jobs[i]->first, jobs[i]->last, jobs[i]->data);
        if (use_pool) {
          std::lock_guard<std::mutex> lock(pool_->mutex);
          jobs[i]->state = FetchJob::State::kDone;
        } else {
          jobs[i]->state = FetchJob::State::kDone;
        }
      }
    }
    if (use_pool) {
      std::unique_lock<std::mutex> lock(pool_->mutex);
      pool_->done_cv.wait(lock, [&] {
        return std::all_of(jobs.begin(), jobs.end(), [](const std::shared_ptr<FetchJob>& j) {
          return j->state == FetchJob::State::kDone;
        });
      });
    }
    bool ok = true;
    for (const auto& job : jobs) {
      if (!job->ok) {
        ok = false;
        continue;
      }
      for (uint32_t b = job->first; b <= job->last; ++b) {
        copy_part(b, job->data.data() + uint64_t(b - job->first) * bs);
      }
    }
    if (!ok) return false;

    // 4. Blocks another thread was downloading. A queued read-ahead piece is taken over by the
    // reader instead of waiting behind the pool; a block that is gone again (evicted, or that
    // download failed) is fetched here.
    for (uint32_t b : waits) {
      for (;;) {
        std::shared_ptr<FetchJob> takeover;
        if (pool_) {
          std::lock_guard<std::mutex> lock(pool_->mutex);
          for (auto it = pool_->ahead.begin(); it != pool_->ahead.end(); ++it) {
            if ((*it)->file == file && (*it)->first <= b && b <= (*it)->last) {
              takeover = std::move(*it);
              pool_->ahead.erase(it);
              takeover->state = FetchJob::State::kRunning;
              break;
            }
          }
        }
        if (takeover) {
          if (!FetchClaimed(file, takeover->first, takeover->last, takeover->data)) return false;
          copy_part(b, takeover->data.data() + uint64_t(b - takeover->first) * bs);
          break;
        }
        {
          std::unique_lock<std::mutex> lock(cache_mutex_);
          if (inflight_.count(BlockKey{file, b})) {
            stat_inflight_waits_.fetch_add(1, std::memory_order_relaxed);
            inflight_cv_.wait(lock, [&] { return !inflight_.count(BlockKey{file, b}); });
          }
        }
        if (copy_cached(b)) break;
        bool mine = false;
        {
          std::lock_guard<std::mutex> lock(cache_mutex_);
          if (inflight_.insert(BlockKey{file, b}).second) mine = true;
        }
        if (!mine) continue;
        std::vector<uint8_t> data;
        if (!FetchClaimed(file, b, b, data)) return false;
        copy_part(b, data.data());
        break;
      }
    }
  }

  MaybeReadAhead(file, first, last);
  *out_bytes_read = length;
  return true;
}

HttpRangeDevice::Stats HttpRangeDevice::stats() const {
  Stats s;
  s.requests = stat_requests_.load(std::memory_order_relaxed);
  s.retries = stat_retries_.load(std::memory_order_relaxed);
  s.stalls = stat_stalls_.load(std::memory_order_relaxed);
  s.failed_reads = stat_failed_reads_.load(std::memory_order_relaxed);
  s.bytes_downloaded = stat_bytes_.load(std::memory_order_relaxed);
  s.memory_hits = stat_memory_hits_.load(std::memory_order_relaxed);
  s.persistent_hits = stat_persistent_hits_.load(std::memory_order_relaxed);
  s.pool_requests = stat_pool_requests_.load(std::memory_order_relaxed);
  s.read_ahead_blocks = stat_read_ahead_blocks_.load(std::memory_order_relaxed);
  s.inflight_waits = stat_inflight_waits_.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(cache_mutex_);
  s.persistent_bytes = cache_bytes_;
  s.evicted_files = stat_evicted_files_;
  s.evicted_bytes = stat_evicted_bytes_;
  s.skipped_writes = stat_skipped_writes_;
  return s;
}

void HttpRangeDevice::ScanPersistentCacheLocked() {
  cache_files_.clear();
  cache_bytes_ = 0;
  std::error_code ec;
  std::vector<std::pair<std::filesystem::file_time_type, std::string>> order;
  for (const auto& entry : std::filesystem::directory_iterator(options_.persistent_cache_dir, ec)) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec)) continue;
    std::string path = entry.path().string();
    std::string base = path.substr(0, path.size() - entry.path().extension().string().size());
    uint64_t size = entry.file_size(entry_ec);
    CacheFileUse& use = cache_files_[base];
    use.bytes += entry_ec ? 0 : size;
    cache_bytes_ += entry_ec ? 0 : size;
    if (entry.path().extension() == ".data") {
      order.emplace_back(entry.last_write_time(entry_ec), base);
    }
  }
  // Recency from the modification times (older files are evicted first).
  std::sort(order.begin(), order.end());
  for (const auto& [time, base] : order) {
    cache_files_[base].last_use = ++use_clock_;
  }
  REXFS_INFO("HttpRangeDevice: persistent cache {}: {} files, {:.1f} MiB (cap {} MiB, reserve {} MiB)",
             options_.persistent_cache_dir, order.size(), double(cache_bytes_) / 1048576.0,
             options_.persistent_cache_max_bytes >= 0 ? options_.persistent_cache_max_bytes >> 20
                                                      : int64_t(REXCVAR_GET(vfs_cache_max_mb)),
             options_.persistent_cache_reserve_bytes >= 0
                 ? options_.persistent_cache_reserve_bytes >> 20
                 : int64_t(REXCVAR_GET(vfs_cache_reserve_mb)));
}

void HttpRangeDevice::TouchPersistentLocked(const std::string& base, uint64_t file_bytes) {
  CacheFileUse& use = cache_files_[base];
  use.last_use = ++use_clock_;
  if (file_bytes > use.bytes) {
    cache_bytes_ += file_bytes - use.bytes;
    use.bytes = file_bytes;
  }
}

uint64_t HttpRangeDevice::EvictPersistentLocked(uint64_t bytes, const std::string& keep_base) {
  uint64_t freed = 0;
  while (freed < bytes) {
    auto victim = cache_files_.end();
    for (auto it = cache_files_.begin(); it != cache_files_.end(); ++it) {
      if (it->first != keep_base && it->second.bytes &&
          (victim == cache_files_.end() || it->second.last_use < victim->second.last_use)) {
        victim = it;
      }
    }
    if (victim == cache_files_.end()) break;
    const std::string base = victim->first;
    // Close it first (WasmFS holds an exclusive OPFS access handle per open file); the next
    // access reopens it empty.
    for (auto it = persistent_.begin(); it != persistent_.end();) {
      if (it->second && it->second->base == base) {
        it = persistent_.erase(it);
      } else {
        ++it;
      }
    }
    for (const char* ext : {".data", ".map", ".complete"}) {
      ::unlink((base + ext).c_str());
    }
    freed += victim->second.bytes;
    cache_bytes_ -= std::min(cache_bytes_, victim->second.bytes);
    bytes_since_estimate_ -= int64_t(victim->second.bytes);
    ++stat_evicted_files_;
    stat_evicted_bytes_ += victim->second.bytes;
    REXFS_INFO("HttpRangeDevice: evicted cache file {} ({:.1f} MiB)", base,
               double(victim->second.bytes) / 1048576.0);
    cache_files_.erase(victim);
  }
  return freed;
}

bool HttpRangeDevice::ReservePersistentLocked(uint64_t bytes, const std::string& keep_base) {
  int64_t cap = options_.persistent_cache_max_bytes >= 0
                    ? options_.persistent_cache_max_bytes
                    : int64_t(REXCVAR_GET(vfs_cache_max_mb)) << 20;
  if (cap > 0) {
    if (bytes > uint64_t(cap)) return false;
    if (cache_bytes_ + bytes > uint64_t(cap)) {
      EvictPersistentLocked(cache_bytes_ + bytes - uint64_t(cap), keep_base);
      if (cache_bytes_ + bytes > uint64_t(cap)) return false;
    }
  }
  web::StorageEstimate estimate = web::GetStorageEstimate();
  if (estimate.valid && estimate.quota > 0) {
    if (estimate.sequence != estimate_sequence_) {
      estimate_sequence_ = estimate.sequence;
      bytes_since_estimate_ = 0;
    }
    double reserve = double(options_.persistent_cache_reserve_bytes >= 0
                                ? options_.persistent_cache_reserve_bytes
                                : int64_t(REXCVAR_GET(vfs_cache_reserve_mb)) << 20);
    double left = estimate.quota - estimate.usage - double(bytes_since_estimate_) - double(bytes);
    if (left < reserve) {
      uint64_t want = uint64_t(reserve - left) + 1;
      uint64_t freed = EvictPersistentLocked(want, keep_base);
      if (freed < want) return false;
    }
  }
  bytes_since_estimate_ += int64_t(bytes);
  return true;
}

}  // namespace rex::filesystem
