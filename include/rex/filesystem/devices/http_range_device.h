/**
 * @file        rex/filesystem/devices/http_range_device.h
 * @brief       Web-only read-only VFS device backed by HTTP Range requests (+ OPFS cache).
 *
 * The browser build has no host directory to mount for game:/cache:. This device builds its
 * entry tree from the server manifest (GET <manifest_url> -> {"files":[{path,size,sha256}]})
 * and serves reads with synchronous `Range:` requests (sync XHR) in aligned blocks. The missing
 * blocks of one read are split over the reader and a small pool of fetch threads (each a
 * Worker issuing its own sync XHR), and a sequential reader gets the next blocks fetched
 * ahead on the pool; a block being downloaded is never requested twice (readers wait for it).
 * A service worker in front of the data URLs (the Halo page's web/sw.js) sees the same
 * Range + If-Match GETs, just several at a time. Blocks go through two caches:
 *   1. an in-memory LRU of blocks shared by all files;
 *   2. optionally a persistent block cache in a directory on the OPFS mount
 *      (rex/filesystem/web_storage.h), keyed by sha256, which a loader may also pre-populate
 *      with complete files.
 * The server contract (Range, ETag = "sha256", If-Match, manifest) is in docs/port/vfs.md of
 * the project repo.
 *
 * Only compiled for Emscripten (src/filesystem/CMakeLists.txt).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/filesystem/device.h>

namespace rex::filesystem {

struct HttpRangeDeviceOptions {
  // URL prefix of the data root. A manifest path "maps/shared.map" is fetched from
  // base_url + "maps/shared.map" (each segment percent-encoded).
  std::string base_url = "/data/";
  std::string manifest_url = "/manifest.json";
  // Directory for the persistent block cache (e.g. "/opfs/rex-cache"); empty = memory only.
  std::string persistent_cache_dir;
  // Fetch/cache granularity. Reads are rounded out to whole blocks.
  uint32_t block_size = 1u << 20;
  // In-memory LRU capacity, in blocks.
  uint32_t memory_cache_blocks = 64;
  // Upper bound for one coalesced Range request (consecutive missing blocks).
  uint32_t max_request_bytes = 8u << 20;
  // Fetch threads (Workers doing sync XHR) that download the missing blocks of a read in
  // parallel with the reader and run read-ahead. -1 = cvar vfs_http_fetch_threads; 0 = every
  // request on the reading thread, one at a time, and no read-ahead.
  int32_t fetch_threads = -1;
  // Blocks fetched ahead (on the fetch threads) of a file that is read sequentially.
  // -1 = cvar vfs_http_read_ahead_blocks; 0 = off. Capped at memory_cache_blocks / 2.
  int32_t read_ahead_blocks = -1;
  // Device name reported to the guest (NtQueryVolumeInformationFile).
  std::string name = "STFS";
};

struct HttpRangeFileInfo {
  std::string path;    // manifest path, '/'-separated, as served
  uint64_t size = 0;
  std::string sha256;  // lowercase hex; the server's strong ETag is "\"" + sha256 + "\""
  uint64_t write_timestamp = 0;  // FILETIME (100 ns since 1601), 0 if unknown
};

class HttpRangeEntry;

class HttpRangeDevice : public Device {
 public:
  HttpRangeDevice(const std::string_view mount_path, HttpRangeDeviceOptions options);
  ~HttpRangeDevice() override;

  // Downloads and parses the manifest, builds the entry tree, opens the persistent cache.
  bool Initialize() override;
  // Same, from an already-fetched manifest JSON (used by tests and by loaders that have it).
  bool InitializeFromManifest(const std::string& manifest_json);

  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return true; }
  const std::string& name() const override { return options_.name; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  const HttpRangeDeviceOptions& options() const { return options_; }
  const std::vector<HttpRangeFileInfo>& files() const { return files_; }

  // Reads up to `length` bytes of files()[file_index] at `offset` into `buffer`.
  // Returns false on a transport/consistency error (logged); short reads at EOF succeed.
  bool Read(size_t file_index, uint64_t offset, uint8_t* buffer, size_t length,
            size_t* out_bytes_read);

  struct Stats {
    uint64_t requests = 0;          // HTTP Range requests issued
    uint64_t bytes_downloaded = 0;  // payload bytes received
    uint64_t memory_hits = 0;       // blocks served from the in-memory LRU
    uint64_t persistent_hits = 0;   // blocks served from the OPFS cache
    uint64_t pool_requests = 0;     // requests issued on the fetch threads (subset of requests)
    uint64_t read_ahead_blocks = 0; // blocks downloaded by read-ahead
    uint64_t inflight_waits = 0;    // blocks a read waited for (already being downloaded)
  };
  Stats stats() const;

 private:
  struct BlockKey {
    uint32_t file;
    uint32_t block;
    bool operator==(const BlockKey& o) const { return file == o.file && block == o.block; }
  };
  struct BlockKeyHash {
    size_t operator()(const BlockKey& k) const {
      return (size_t(k.file) << 32) ^ size_t(k.block);
    }
  };
  struct CachedBlock {
    BlockKey key;
    std::vector<uint8_t> data;
  };
  struct PersistentFile;
  struct FetchJob;
  struct FetchPool;

  bool BuildTree();
  std::string UrlFor(const HttpRangeFileInfo& file) const;
  uint32_t BlockCount(const HttpRangeFileInfo& file) const;
  size_t BlockBytes(const HttpRangeFileInfo& file, uint32_t block) const;

  // Copies a cached block out (memory, then persistent). Returns false if not cached.
  bool CopyCachedBlock(uint32_t file, uint32_t block, uint64_t in_block_offset, uint8_t* dst,
                       size_t length);
  void InsertMemoryBlock(uint32_t file, uint32_t block, const uint8_t* data, size_t length);
  bool ReadPersistentBlock(uint32_t file, uint32_t block, std::vector<uint8_t>& out);
  void WritePersistentBlocks(uint32_t file, uint32_t first_block, const uint8_t* data,
                             size_t length);
  PersistentFile* GetPersistentFile(uint32_t file);
  // Downloads blocks [first, last] in one Range request into `out`.
  bool FetchBlocks(uint32_t file, uint32_t first, uint32_t last, std::vector<uint8_t>& out);
  // FetchBlocks for blocks the caller claimed in inflight_, then stores them in both caches,
  // releases the claims (also on failure) and wakes readers waiting for them.
  bool FetchClaimed(uint32_t file, uint32_t first, uint32_t last, std::vector<uint8_t>& out);
  // cache_mutex_ held: block is in the memory LRU or the persistent cache.
  bool IsCachedLocked(uint32_t file, uint32_t block);
  // Queues read-ahead after a read of blocks [first, last] if the file is read sequentially.
  void MaybeReadAhead(uint32_t file, uint32_t first, uint32_t last);
  void StartPool();
  void StopPool();

  HttpRangeDeviceOptions options_;
  std::vector<HttpRangeFileInfo> files_;
  std::unique_ptr<Entry> root_entry_;

  mutable std::mutex cache_mutex_;
  std::list<CachedBlock> lru_;  // front = most recent
  std::unordered_map<BlockKey, std::list<CachedBlock>::iterator, BlockKeyHash> lru_index_;
  std::unordered_map<uint32_t, std::unique_ptr<PersistentFile>> persistent_;
  bool persistent_enabled_ = false;
  // Blocks some thread is downloading right now (guarded by cache_mutex_); inflight_cv_ is
  // notified whenever claims are released.
  std::unordered_set<BlockKey, BlockKeyHash> inflight_;
  std::condition_variable inflight_cv_;
  std::unordered_map<uint32_t, uint32_t> last_read_block_;  // file -> last block of its last read
  uint32_t read_ahead_ = 0;
  std::unique_ptr<FetchPool> pool_;

  std::atomic<uint64_t> stat_requests_{0};
  std::atomic<uint64_t> stat_bytes_{0};
  std::atomic<uint64_t> stat_memory_hits_{0};
  std::atomic<uint64_t> stat_persistent_hits_{0};
  std::atomic<uint64_t> stat_pool_requests_{0};
  std::atomic<uint64_t> stat_read_ahead_blocks_{0};
  std::atomic<uint64_t> stat_inflight_waits_{0};
};

}  // namespace rex::filesystem
