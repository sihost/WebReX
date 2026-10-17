/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <string>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>

namespace rex::filesystem {

class HostPathDevice;

class HostPathEntry : public Entry {
 public:
  HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                const std::filesystem::path& host_path);
  ~HostPathEntry() override;

  static HostPathEntry* Create(Device* device, Entry* parent,
                               const std::filesystem::path& full_path,
                               rex::filesystem::FileInfo file_info);

  const std::filesystem::path& host_path() const { return host_path_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;
  bool Truncate() override;

  bool can_map() const override { return true; }
  std::unique_ptr<memory::MappedMemory> OpenMapped(memory::MappedMemory::Mode mode, size_t offset,
                                                   size_t length) override;
  void update() override;
  bool SetAttributes(uint64_t attributes) override;
  bool SetCreateTimestamp(uint64_t timestamp) override;
  bool SetAccessTimestamp(uint64_t timestamp) override;
  bool SetWriteTimestamp(uint64_t timestamp) override;

  // Open HostPathFile count (HostPathFile ctor / Destroy). ReleaseOpenFile returns true when the
  // last one closed.
  void AddOpenFile() { open_files_.fetch_add(1, std::memory_order_relaxed); }
  bool ReleaseOpenFile() { return open_files_.fetch_sub(1, std::memory_order_acq_rel) == 1; }
  // Carries out a delete-on-close (NtSetInformationFile FileDispositionInformation) once the last
  // handle closed, when the device allows it (HostPathDevice::DeletesOnClose). May destroy this
  // entry; do not touch it afterwards.
  void DeleteOnLastClose();

 private:
  friend class HostPathDevice;

  std::unique_ptr<Entry> CreateEntryInternal(const std::string_view name,
                                             uint32_t attributes) override;
  bool DeleteEntryInternal(Entry* entry) override;
  void RenameEntryInternal(const std::vector<std::string_view>& path_parts) override;

  std::filesystem::path host_path_;
  std::atomic<int> open_files_{0};
};

}  // namespace rex::filesystem
