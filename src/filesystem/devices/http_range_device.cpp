/**
 * @file        filesystem/devices/http_range_device.cpp
 * @brief       Web-only read-only VFS device over HTTP Range requests with OPFS block cache.
 *
 * See rex/filesystem/devices/http_range_device.h and docs/port/vfs.md (project repo).
 *
 * Transport: synchronous XMLHttpRequest from the calling pthread. Every guest file read runs
 * on a pthread (a Worker; PROXY_TO_PTHREAD keeps even main() off the browser main thread), and
 * sync XHR with responseType "arraybuffer" is allowed in Workers. Under node (tests) the same
 * entry points read from the local directory named by $REX_WEB_TEST_DATA_ROOT instead.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/filesystem/devices/http_range_device.h>

#include <rex/platform.h>

static_assert(REX_PLATFORM_WEB, "HttpRangeDevice is web-only (Emscripten)");

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/string/utf8.h>

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

HttpRangeDevice::HttpRangeDevice(const std::string_view mount_path,
                                 HttpRangeDeviceOptions options)
    : Device(mount_path), options_(std::move(options)) {
  options_.block_size = std::max<uint32_t>(options_.block_size, 4096);
  options_.max_request_bytes = std::max(options_.max_request_bytes, options_.block_size);
}

HttpRangeDevice::~HttpRangeDevice() = default;

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
    }
  }

  if (!BuildTree()) {
    return false;
  }
  REXFS_INFO("HttpRangeDevice: {} files from {} (base {}, block {} KiB, cache {})", files_.size(),
             options_.manifest_url, options_.base_url, options_.block_size / 1024,
             persistent_enabled_ ? options_.persistent_cache_dir : std::string("memory only"));
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
  if (::pwrite(pf->data_fd, data, length, offset) != ssize_t(length)) {
    return;  // quota or I/O error: just don't mark the blocks
  }
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
  // One retry on network errors (not on HTTP errors).
  for (int attempt = 0; attempt < 2; ++attempt) {
    stat_requests_.fetch_add(1, std::memory_order_relaxed);
    double got = rex_web_http_read_range(PtrArg(url.c_str()), double(offset), double(length),
                                         PtrArg(out.data()),
                                         if_match.empty() ? 0.0 : PtrArg(if_match.c_str()));
    if (got >= 0) {
      stat_bytes_.fetch_add(uint64_t(got), std::memory_order_relaxed);
      if (size_t(got) != length) {
        REXFS_ERROR("HttpRangeDevice: short response for {} [{}+{}]: {} bytes", info.path, offset,
                    length, size_t(got));
        return false;
      }
      return true;
    }
    if (got == -412) {
      REXFS_ERROR("HttpRangeDevice: {} changed on the server (ETag != sha256 {}); reload the page",
                  info.path, info.sha256);
      return false;
    }
    if (got != -1) {
      REXFS_ERROR("HttpRangeDevice: HTTP {} for {} [{}+{}]", -got, info.path, offset, length);
      return false;
    }
  }
  REXFS_ERROR("HttpRangeDevice: network error reading {} [{}+{}]", info.path, offset, length);
  return false;
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

  std::vector<uint8_t> fetched;
  uint32_t block = first;
  while (block <= last) {
    uint64_t block_start = uint64_t(block) * bs;
    uint64_t from = std::max(offset, block_start);
    uint64_t to = std::min<uint64_t>(read_end, block_start + BlockBytes(info, block));
    if (CopyCachedBlock(file, block, from - block_start, buffer + (from - offset),
                        size_t(to - from))) {
      ++block;
      continue;
    }
    // Coalesce the run of missing blocks into one request.
    uint32_t run_end = block;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      while (run_end < last && run_end - block + 1 < max_blocks) {
        uint32_t next = run_end + 1;
        if (lru_index_.count(BlockKey{file, next})) break;
        PersistentFile* pf = GetPersistentFile(file);
        if (pf && pf->Has(next)) break;
        run_end = next;
      }
    }
    if (!FetchBlocks(file, block, run_end, fetched)) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      for (uint32_t b = block; b <= run_end; ++b) {
        const uint8_t* data = fetched.data() + uint64_t(b - block) * bs;
        InsertMemoryBlock(file, b, data, BlockBytes(info, b));
      }
      WritePersistentBlocks(file, block, fetched.data(), fetched.size());
    }
    for (uint32_t b = block; b <= run_end; ++b) {
      copy_part(b, fetched.data() + uint64_t(b - block) * bs);
    }
    block = run_end + 1;
  }
  *out_bytes_read = length;
  return true;
}

HttpRangeDevice::Stats HttpRangeDevice::stats() const {
  Stats s;
  s.requests = stat_requests_.load(std::memory_order_relaxed);
  s.bytes_downloaded = stat_bytes_.load(std::memory_order_relaxed);
  s.memory_hits = stat_memory_hits_.load(std::memory_order_relaxed);
  s.persistent_hits = stat_persistent_hits_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace rex::filesystem
