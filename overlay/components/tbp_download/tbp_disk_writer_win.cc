// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_disk_writer.h"

#include <windows.h>

#include <algorithm>
#include <ranges>
#include <vector>

#include "base/compiler_specific.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/logging.h"

namespace tbp_download {

namespace {

// Rounds `value` up to the next multiple of `alignment`.
int64_t AlignUp(int64_t value, int64_t alignment) {
  if (alignment <= 0) {
    return value;
  }
  const int64_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

HANDLE AsHandle(void* h) {
  return h ? static_cast<HANDLE>(h) : INVALID_HANDLE_VALUE;
}

bool IsValidHandle(void* h) {
  return h != nullptr && h != INVALID_HANDLE_VALUE;
}

}  // namespace

// --- AlignedBuffer --------------------------------------------------------

AlignedBuffer::AlignedBuffer() = default;

AlignedBuffer::~AlignedBuffer() {
  Reset();
}

bool AlignedBuffer::Allocate(size_t size) {
  Reset();
  if (size == 0) {
    return false;
  }
  // VirtualAlloc always returns page-aligned memory (4 KiB on x64), which
  // satisfies any realistic sector size. It also rounds the reservation up to a
  // whole page, so the usable region is never shorter than requested.
  void* mem = ::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
  if (!mem) {
    PLOG(ERROR) << "VirtualAlloc failed for " << size << " bytes";
    return false;
  }
  data_ = static_cast<uint8_t*>(mem);
  size_ = size;
  return true;
}

void AlignedBuffer::Reset() {
  if (data_) {
    ::VirtualFree(data_, 0, MEM_RELEASE);
    data_ = nullptr;
  }
  size_ = 0;
}

base::span<uint8_t> AlignedBuffer::span() {
  // The one place this class has to bridge a raw allocation into a span. Both
  // the pointer and the length come straight from the VirtualAlloc above and
  // are reset together, so the bounds are correct by construction — there is no
  // safer formulation available, which is exactly what this macro is for. Every
  // other buffer access in this file goes through the span this returns.
  return UNSAFE_BUFFERS(base::span<uint8_t>(data_, size_));
}

// --- DiskWriter -----------------------------------------------------------

DiskWriter::DiskWriter() = default;

DiskWriter::~DiskWriter() {
  if (IsValidHandle(handle_)) {
    // Destroyed without Finish(): the download did not complete, so do not
    // leave a partial file containing unzeroed cluster contents behind.
    ScrubAndClose();
  }
}

// static
bool DiskWriter::EnableFastPreallocation() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    PLOG(WARNING) << "OpenProcessToken failed";
    return false;
  }

  LUID luid = {};
  if (!::LookupPrivilegeValue(nullptr, SE_MANAGE_VOLUME_NAME, &luid)) {
    PLOG(WARNING) << "LookupPrivilegeValue(SE_MANAGE_VOLUME_NAME) failed";
    ::CloseHandle(token);
    return false;
  }

  TOKEN_PRIVILEGES privileges = {};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Luid = luid;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  const BOOL adjusted = ::AdjustTokenPrivileges(token, FALSE, &privileges,
                                                sizeof(privileges), nullptr,
                                                nullptr);
  // AdjustTokenPrivileges reports success even when it could not enable
  // everything asked for; ERROR_NOT_ALL_ASSIGNED is the real answer and means
  // the account simply does not hold the right.
  const DWORD error = ::GetLastError();
  ::CloseHandle(token);

  if (!adjusted || error != ERROR_SUCCESS) {
    return false;
  }
  return true;
}

// static
int64_t DiskWriter::QuerySectorSize(const base::FilePath& path) {
  // GetDiskFreeSpaceW wants the volume root ("V:\"), not a file path.
  const base::FilePath dir = path.EndsWithSeparator() ? path : path.DirName();
  std::wstring root = dir.value();
  wchar_t volume[MAX_PATH] = {};
  if (::GetVolumePathNameW(dir.value().c_str(), volume, MAX_PATH)) {
    root.assign(volume);
  }

  DWORD sectors_per_cluster = 0;
  DWORD bytes_per_sector = 0;
  DWORD free_clusters = 0;
  DWORD total_clusters = 0;
  if (!::GetDiskFreeSpaceW(root.c_str(), &sectors_per_cluster,
                           &bytes_per_sector, &free_clusters,
                           &total_clusters)) {
    PLOG(WARNING) << "GetDiskFreeSpaceW failed for " << root;
    return 0;
  }
  return static_cast<int64_t>(bytes_per_sector);
}

bool DiskWriter::Open(const base::FilePath& path, int64_t total_size) {
  Close();
  finished_ = false;
  used_fast_preallocation_ = false;
  path_ = path;

  sector_size_ = QuerySectorSize(path);
  if (sector_size_ <= 0) {
    LOG(ERROR) << "Could not determine sector size for " << path;
    return false;
  }

  HANDLE handle = ::CreateFileW(
      path.value().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
      nullptr, CREATE_ALWAYS,
      FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    PLOG(ERROR) << "CreateFileW failed for " << path;
    return false;
  }
  handle_ = handle;

  if (total_size == kUnknownTotalSize || total_size <= 0) {
    reserved_size_ = 0;
    return true;
  }

  // Reserve the clusters up front so the filesystem is not extending the file
  // on every write, and so a full disk is discovered now rather than at 99%.
  // The reservation is rounded up to a sector because an unbuffered handle
  // cannot have a file size that is not a sector multiple until the final
  // truncation in Finish().
  reserved_size_ = AlignUp(total_size, sector_size_);

  FILE_ALLOCATION_INFO allocation = {};
  allocation.AllocationSize.QuadPart = reserved_size_;
  if (!::SetFileInformationByHandle(handle, FileAllocationInfo, &allocation,
                                    sizeof(allocation))) {
    PLOG(ERROR) << "Reserving " << reserved_size_ << " bytes failed for "
                << path;
    Close();
    return false;
  }

  // Move the end of file out to the reserved size. Without this the file is
  // still logically zero-length and SetFileValidData has nothing to extend.
  LARGE_INTEGER end = {};
  end.QuadPart = reserved_size_;
  if (!::SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) ||
      !::SetEndOfFile(handle)) {
    PLOG(ERROR) << "Setting end of file to " << reserved_size_ << " failed";
    Close();
    return false;
  }

  // The fast path. Without it NTFS zero-fills the newly allocated clusters
  // lazily, which on a multi-gigabyte file is real, measurable time spent
  // writing zeroes that our download is about to overwrite anyway.
  //
  // A failure here is expected and benign when the process does not hold
  // SeManageVolumePrivilege; the file is already correctly sized either way.
  if (::SetFileValidData(handle, reserved_size_)) {
    used_fast_preallocation_ = true;
  } else {
    DVLOG(1) << "SetFileValidData unavailable, falling back to lazy zero-fill";
  }

  return true;
}

bool DiskWriter::WriteAt(int64_t offset, base::span<const uint8_t> data) {
  if (!IsValidHandle(handle_)) {
    LOG(ERROR) << "WriteAt on a closed writer";
    return false;
  }
  if (data.empty()) {
    return true;
  }
  if (sector_size_ <= 0 || offset % sector_size_ != 0) {
    LOG(ERROR) << "Unaligned write offset " << offset << " (sector "
               << sector_size_ << ")";
    return false;
  }

  // An unbuffered handle can only transfer whole sectors, so a final short
  // chunk has to be rounded up. The padding bytes land inside the space we
  // already reserved and are cut away by Finish().
  const int64_t aligned_length =
      AlignUp(static_cast<int64_t>(data.size()), sector_size_);

  AlignedBuffer padded;
  base::span<const uint8_t> source = data;
  const bool needs_padding =
      aligned_length != static_cast<int64_t>(data.size()) ||
      (reinterpret_cast<uintptr_t>(data.data()) %
       static_cast<uintptr_t>(sector_size_)) != 0;

  if (needs_padding) {
    // Either the caller's buffer is not sector-aligned, or this is the ragged
    // tail of the file. Both are fixed the same way: copy into an aligned
    // buffer sized to a whole number of sectors.
    if (!padded.Allocate(static_cast<size_t>(aligned_length))) {
      return false;
    }
    base::span<uint8_t> target = padded.span();
    target.first(data.size()).copy_from(data);
    std::ranges::fill(target.subspan(data.size()), uint8_t{0});
    source = target;
  }

  int64_t written_total = 0;
  while (written_total < aligned_length) {
    OVERLAPPED overlapped = {};
    const int64_t position = offset + written_total;
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFF);
    overlapped.OffsetHigh = static_cast<DWORD>((position >> 32) & 0xFFFFFFFF);

    // Chunk the transfer: WriteFile takes a DWORD length, and very large single
    // writes are no faster than a few large ones.
    const int64_t remaining = aligned_length - written_total;
    const base::span<const uint8_t> block =
        source.subspan(static_cast<size_t>(written_total),
                       static_cast<size_t>(
                           std::min<int64_t>(remaining, 32 * 1024 * 1024)));

    DWORD written = 0;
    if (!::WriteFile(AsHandle(handle_), block.data(),
                     static_cast<DWORD>(block.size()), &written, &overlapped)) {
      PLOG(ERROR) << "WriteFile failed at offset " << position;
      return false;
    }
    if (written == 0) {
      LOG(ERROR) << "WriteFile wrote 0 bytes at offset " << position;
      return false;
    }
    written_total += written;
  }

  return true;
}

bool DiskWriter::Finish(int64_t final_size) {
  if (!IsValidHandle(handle_)) {
    LOG(ERROR) << "Finish on a closed writer";
    return false;
  }

  // Cut away the preallocation slack and any sector padding from the last
  // write, so the file ends up its true length.
  //
  // This cannot be done on our own handle: an unbuffered handle can only
  // position the file pointer on a sector boundary, so SetEndOfFile to an
  // arbitrary length fails with ERROR_INVALID_PARAMETER — and a real file's
  // length is almost never a sector multiple. Close the unbuffered handle and
  // do the truncation through an ordinary buffered one, which has no such
  // restriction. (Found by the ragged-tail test rather than by reading docs.)
  Close();

  base::File file(path_,
                  base::File::FLAG_OPEN | base::File::FLAG_WRITE);
  if (!file.IsValid()) {
    LOG(ERROR) << "Reopening " << path_ << " to truncate failed: "
               << base::File::ErrorToString(file.error_details());
    base::DeleteFile(path_);
    return false;
  }
  if (!file.SetLength(final_size)) {
    PLOG(ERROR) << "Truncating " << path_ << " to " << final_size << " failed";
    file.Close();
    base::DeleteFile(path_);
    return false;
  }

  finished_ = true;
  return true;
}

void DiskWriter::ScrubAndClose() {
  if (!IsValidHandle(handle_)) {
    return;
  }

  // Overwrite whatever the reservation exposed. Without SetFileValidData the
  // clusters were already zeroed by NTFS and this is redundant but harmless;
  // with it, this is what keeps remnants of unrelated deleted files from
  // surviving in an abandoned partial download.
  if (used_fast_preallocation_ && reserved_size_ > 0) {
    AlignedBuffer zeros;
    const int64_t block = std::min<int64_t>(reserved_size_, 8 * 1024 * 1024);
    if (zeros.Allocate(static_cast<size_t>(AlignUp(block, sector_size_)))) {
      std::ranges::fill(zeros.span(), uint8_t{0});
      for (int64_t offset = 0; offset < reserved_size_;
           offset += static_cast<int64_t>(zeros.size())) {
        const int64_t remaining = reserved_size_ - offset;
        const base::span<const uint8_t> chunk = zeros.span().first(
            static_cast<size_t>(std::min<int64_t>(
                remaining, static_cast<int64_t>(zeros.size()))));
        OVERLAPPED overlapped = {};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
        DWORD written = 0;
        if (!::WriteFile(AsHandle(handle_), chunk.data(),
                         static_cast<DWORD>(chunk.size()), &written,
                         &overlapped)) {
          // Best effort: the file is deleted below regardless.
          break;
        }
      }
    }
  }

  Close();
  if (!path_.empty()) {
    base::DeleteFile(path_);
  }
}

void DiskWriter::Close() {
  if (IsValidHandle(handle_)) {
    ::CloseHandle(AsHandle(handle_));
  }
  handle_ = nullptr;
}

bool DiskWriter::is_open() const {
  return IsValidHandle(handle_);
}

}  // namespace tbp_download
