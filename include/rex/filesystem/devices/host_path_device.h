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

#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/device.h>

namespace rex::filesystem {

class HostPathEntry;

class HostPathDevice : public Device {
 public:
  HostPathDevice(const std::string_view mount_path, const std::filesystem::path& host_path,
                 bool read_only);
  ~HostPathDevice() override;

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return read_only_; }
  const std::filesystem::path& host_path() const { return host_path_; }

  // NT delete-on-close: an entry marked with FileDispositionInformation is removed from the host
  // folder when its last handle closes (HostPathFile::Destroy). Default: on for web builds (the
  // folder is the app's own OPFS storage), off natively (the folder is often the user's own game
  // folder, so marked entries stay, as in SDK <= v0.5.0). keep_prefixes: device-relative paths
  // ('\\'-separated, case-insensitive, e.g. "saved_films") whose entries are never deleted this
  // way even when enabled. Games that delete "until the folder is empty / below a cap" loop
  // forever on a device that ignores the flag (Halo 3 cache:\\autosave holds at most 25 items).
  void SetDeleteOnClose(bool enabled, std::vector<std::string> keep_prefixes = {});
  // Whether a marked entry at this device-relative path is deleted when its last handle closes.
  bool DeletesOnClose(std::string_view entry_path) const;

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 128 * 1024; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  void PopulateEntry(HostPathEntry* parent_entry);

  std::string name_;
  std::filesystem::path host_path_;
  std::unique_ptr<Entry> root_entry_;
  bool read_only_;
  bool delete_on_close_;
  std::vector<std::string> delete_on_close_keep_;
};

}  // namespace rex::filesystem
