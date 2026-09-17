// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_sector_aligned_writer.h"

#include <algorithm>

#include "components/tbp_download/tbp_chunk_scheduler.h"

namespace tbp_download {

SectorAlignedWriter::SectorAlignedWriter(DiskWriter* writer,
                                          int64_t chunk_start,
                                          int64_t sector_size,
                                          bool is_final_chunk,
                                          size_t target_buffer_bytes)
    : writer_(writer),
      sector_size_(sector_size),
      is_final_chunk_(is_final_chunk),
      write_offset_(chunk_start),
      next_offset_(chunk_start) {
  int64_t capacity = ChunkScheduler::AlignDown(
      static_cast<int64_t>(target_buffer_bytes), sector_size_);
  if (capacity < sector_size_) {
    capacity = sector_size_;
  }
  buffer_.Allocate(static_cast<size_t>(capacity));
}

SectorAlignedWriter::~SectorAlignedWriter() = default;

bool SectorAlignedWriter::Append(base::span<const uint8_t> data) {
  if (failed_ || !buffer_.valid()) {
    return false;
  }

  while (!data.empty()) {
    size_t space_left = buffer_.size() - buffered_bytes_;
    size_t to_copy = std::min(data.size(), space_left);

    buffer_.span()
        .subspan(buffered_bytes_, to_copy)
        .copy_from(data.first(to_copy));
    buffered_bytes_ += to_copy;
    next_offset_ += static_cast<int64_t>(to_copy);
    data = data.subspan(to_copy);

    if (buffered_bytes_ == buffer_.size()) {
      if (!FlushWholeSectors()) {
        return false;
      }
    }
  }
  return true;
}

bool SectorAlignedWriter::FlushWholeSectors() {
  // The only caller (Append(), above) invokes this precisely when
  // buffered_bytes_ has reached the buffer's full capacity -- and Allocate()
  // always rounds that capacity to a whole number of sectors -- so there is
  // never a partial-sector remainder to carry over here. The entire buffer
  // is written and reset every time.
  if (buffered_bytes_ == 0) {
    return true;
  }
  if (!writer_->WriteAt(write_offset_, buffer_.span().first(buffered_bytes_))) {
    failed_ = true;
    return false;
  }
  write_offset_ += static_cast<int64_t>(buffered_bytes_);
  buffered_bytes_ = 0;
  return true;
}

bool SectorAlignedWriter::Finish() {
  if (failed_) {
    return false;
  }
  if (buffered_bytes_ == 0) {
    return true;
  }

  if (!is_final_chunk_) {
    // A non-final chunk's total length was computed by the scheduler to be
    // a sector multiple; landing here with a nonzero remainder means the
    // fetch delivered fewer bytes than the chunk's own range promised --
    // a truncated response, not something safe to write as a short
    // interior chunk.
    failed_ = true;
    return false;
  }

  // The true end of the file: DiskWriter::WriteAt's documented short-write
  // path handles a length that isn't a sector multiple here, and Finish()
  // on the DiskWriter itself (called by the orchestrator once every chunk
  // is done) truncates away the padding.
  bool ok = writer_->WriteAt(
      write_offset_, buffer_.span().first(buffered_bytes_));
  if (!ok) {
    failed_ = true;
    return false;
  }
  write_offset_ += static_cast<int64_t>(buffered_bytes_);
  buffered_bytes_ = 0;
  return true;
}

}  // namespace tbp_download
