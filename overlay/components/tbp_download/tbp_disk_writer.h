// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TBP_DISK_WRITER_H_
#define COMPONENTS_TBP_DOWNLOAD_TBP_DISK_WRITER_H_

#include <stdint.h>

#include "base/containers/span.h"
#include "base/files/file_path.h"

namespace tbp_download {

// A page-aligned heap buffer suitable for unbuffered (Direct I/O) writes.
//
// Windows requires that buffers handed to a FILE_FLAG_NO_BUFFERING handle start
// on a memory boundary that is a multiple of the volume's sector size. Ordinary
// heap allocations make no such guarantee, so chunk buffers have to come from
// here rather than from std::vector or new[].
class AlignedBuffer {
 public:
  AlignedBuffer();
  ~AlignedBuffer();

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  // Reserves `size` bytes, rounded up to a whole number of pages. Returns false
  // if the allocation fails. Any previous allocation is released first.
  bool Allocate(size_t size);

  void Reset();

  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool valid() const { return data_ != nullptr; }

  base::span<uint8_t> span();

 private:
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// Writes downloaded bytes to disk at arbitrary offsets, bypassing the OS file
// cache.
//
// The engine already owns its chunk buffers and its own record of what has been
// written, and never reads back what it just wrote, so letting Windows
// double-buffer the same data through its page cache costs memory bandwidth and
// CPU for nothing. Opening unbuffered also lets the NVMe controller DMA straight
// out of our buffers.
//
// That choice is why the alignment rules below exist, and why they propagate all
// the way back into ChunkScheduler: with FILE_FLAG_NO_BUFFERING, Windows rejects
// any write whose offset, length, or buffer address is not a multiple of the
// volume's sector size.
//
// Not thread-safe; one instance belongs to one download and is driven from that
// download's file sequence.
class DiskWriter {
 public:
  DiskWriter();
  ~DiskWriter();

  DiskWriter(const DiskWriter&) = delete;
  DiskWriter& operator=(const DiskWriter&) = delete;

  // Tries to enable SeManageVolumePrivilege for this process, which is what
  // SetFileValidData requires. Idempotent and cheap to call repeatedly.
  //
  // Returns whether the privilege is now held. A false return is not an error:
  // it means preallocation falls back to the slower path that lets NTFS zero
  // the clusters lazily. The installer is expected to grant this right, so in a
  // normal install this returns true; it returns false for dev builds and for
  // machines where policy stripped the right.
  static bool EnableFastPreallocation();

  // Logical sector size of the volume containing `path`, or 0 if it cannot be
  // determined. This is the alignment every offset and length must respect.
  static int64_t QuerySectorSize(const base::FilePath& path);

  // Creates (or truncates) `path`, opens it unbuffered, and reserves
  // `total_size` bytes so the file does not have to grow during the download.
  //
  // Pass kUnknownTotalSize when the length is not known ahead of time; the file
  // is then created without reservation and simply grows.
  //
  // Pass `preserve_existing_content = true` when resuming a download whose
  // destination file already holds real, previously-written bytes that must
  // not be discarded -- e.g. after a resume from persisted DownloadState.
  // This opens an existing file in place instead of truncating it; if no
  // file exists yet at `path`, it is created fresh exactly as when this is
  // false. Getting this wrong silently destroys a resume's whole point, so
  // it is an explicit, named argument rather than inferred from context.
  bool Open(const base::FilePath& path,
            int64_t total_size,
            bool preserve_existing_content = false);

  // Writes `data` at `offset`.
  //
  // `offset` must be sector-aligned and `data.data()` must be sector-aligned
  // (use AlignedBuffer). `data.size()` must be a multiple of the sector size
  // *except* for the write that covers the end of the file, which may be short;
  // that case is handled by padding out to a full sector and recording the true
  // length, which Finish() then truncates to.
  bool WriteAt(int64_t offset, base::span<const uint8_t> data);

  // Truncates the file to `final_size` (undoing both the preallocation and any
  // sector padding from the final write) and closes it.
  bool Finish(int64_t final_size);

  // Closes without truncating. Safe to call on an already-closed writer.
  void Close();

  bool is_open() const;

  // Whether Open() managed to use the instant-reservation path. Exposed so the
  // engine can report why a download had a slow start, and so tests can assert
  // both paths.
  bool used_fast_preallocation() const { return used_fast_preallocation_; }

  int64_t sector_size() const { return sector_size_; }

  static constexpr int64_t kUnknownTotalSize = -1;

 private:
  // Zeroes a partially-written file before deleting it.
  //
  // SetFileValidData deliberately skips zero-filling, which is exactly why it
  // needs a privilege: until our data overwrites them, the reserved clusters
  // still hold whatever a previously deleted file left there. A download that is
  // cancelled or fails therefore leaves a file containing fragments of unrelated
  // deleted data, so an abandoned partial file is scrubbed rather than left on
  // disk.
  void ScrubAndClose();

  void* handle_ = nullptr;  // HANDLE, kept as void* to avoid <windows.h> here.
  int64_t sector_size_ = 0;
  int64_t reserved_size_ = 0;
  bool used_fast_preallocation_ = false;
  bool finished_ = false;
  base::FilePath path_;
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TBP_DISK_WRITER_H_
