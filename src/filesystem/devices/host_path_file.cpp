/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "host_path_entry.h"
#include "host_path_file.h"

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WEB
#include <cerrno>
#include <cstring>

#include <rex/filesystem/web_storage.h>
#endif

namespace rex::filesystem {

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {
  entry->AddOpenFile();
}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  auto* host_entry = static_cast<HostPathEntry*>(entry());
  file_handle_.reset();  // close the host descriptor before a possible unlink
  const bool last = host_entry->ReleaseOpenFile();
  delete this;
  // NT deletes a file marked with FileDispositionInformation when its last handle closes. Halo 3
  // wipes cache:\maps\cached_fonts by "find first file, mark delete, close" until the folder is
  // empty (looped forever on web while the flag was ignored), and its autosave queue deletes the
  // oldest cache:\autosave item while 25 or more exist (looped forever natively once 25 had
  // accumulated). The device decides (HostPathDevice::SetDeleteOnClose): on by default on web,
  // off by default natively, where the device folder may be the user's game folder.
  if (last && host_entry->delete_on_close()) {
    auto* device = static_cast<HostPathDevice*>(host_entry->device());
    if (device->DeletesOnClose(host_entry->path())) {
      host_entry->DeleteOnLastClose();
    } else {
      REXFS_DEBUG("HostPathFile: delete-on-close of {} not carried out (device policy)",
                  host_entry->path());
    }
  }
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

#if REX_PLATFORM_WEB
  // OPFS: a write past the origin's quota fails (WasmFS: EIO / ENOSPC) or comes up short.
  // Free evictable caches (web_storage.h) and retry the rest once; still failing, report a
  // full disk to the guest (saves fail cleanly instead of reading back truncated files).
  size_t written = 0;
  bool ok = file_handle_->Write(byte_offset, buffer.data(), buffer.size(), &written);
  if (!ok || written < buffer.size()) {
    int error = ok ? ENOSPC : errno;
    size_t done = ok ? written : 0;
    size_t remaining = buffer.size() - done;
    uint64_t freed = web::RelieveStoragePressure(uint64_t(remaining) + (uint64_t(1) << 20));
    size_t retried = 0;
    bool retry_ok = freed && file_handle_->Write(byte_offset + done, buffer.data() + done,
                                                 remaining, &retried);
    if (retry_ok && retried == remaining) {
      REXFS_WARN("HostPathFile: write of {} bytes to {} succeeded after freeing {} KiB ({})",
                 buffer.size(), entry()->path(), freed >> 10, std::strerror(error));
      if (out_bytes_written) {
        *out_bytes_written = buffer.size();
      }
      return X_STATUS_SUCCESS;
    }
    REXFS_ERROR("HostPathFile: write of {} bytes at {} to {} failed ({}; {} KiB freed): disk full",
                buffer.size(), byte_offset, entry()->path(), std::strerror(error), freed >> 10);
    if (out_bytes_written) {
      *out_bytes_written = done + (retry_ok ? retried : 0);
    }
    return X_STATUS_DISK_FULL;
  }
  if (out_bytes_written) {
    *out_bytes_written = written;
  }
  return X_STATUS_SUCCESS;
#else
  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
#endif
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem
