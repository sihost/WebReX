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

#include <rex/platform.h>

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
#if REX_PLATFORM_WEB
  // NT deletes a file marked with FileDispositionInformation when its last handle closes. Halo 3
  // wipes cache:\maps\cached_fonts by "find first file, mark delete, close" until the folder is
  // empty, which looped forever while the flag was ignored (second boot with a persisted
  // OPFS cache:). Native builds keep ignoring the flag for now: there cache: is the user's game
  // folder and Halo marks its shipped saved_films for deletion at every boot (STUBS.md).
  if (last && host_entry->delete_on_close()) {
    host_entry->DeleteOnLastClose();
  }
#else
  (void)last;
#endif
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

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
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
