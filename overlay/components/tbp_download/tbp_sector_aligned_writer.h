// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TBP_SECTOR_ALIGNED_WRITER_H_
#define COMPONENTS_TBP_DOWNLOAD_TBP_SECTOR_ALIGNED_WRITER_H_

#include <stdint.h>

#include "base/containers/span.h"
#include "components/tbp_download/tbp_disk_writer.h"

namespace tbp_download {

// Assembles one chunk worker's arbitrarily-sized, arbitrarily-fragmented
// network reads into the sector-sized, sector-aligned writes DiskWriter
// requires.
//
// A network read almost never lands on a sector boundary -- TCP delivers
// whatever the kernel handed the socket, not what the disk wants. This
// buffers incoming bytes until a whole sector (or more) has accumulated,
// writes exactly that many whole sectors, and carries any remainder into the
// next Append(). Only the true end of the file may end on a short,
// non-sector-multiple write (DiskWriter's own documented tail case); every
// other boundary -- including the end of every non-final chunk -- must land
// exactly on a sector, which the scheduler already guarantees by construction
// (see SchedulerConfig::sector_size). A chunk that finishes with a nonzero
// remainder while NOT being the file's final chunk is therefore a bug or a
// truncated response, not a case this class silently papers over: Finish()
// reports it as an error rather than writing a short interior chunk.
class SectorAlignedWriter {
 public:
  // `chunk_start` is the absolute file offset this chunk begins at --
  // already guaranteed sector-aligned by the scheduler. `is_final_chunk`
  // must be true only for the chunk that covers the true end of the file.
  // `target_buffer_bytes` is rounded down to a whole number of sectors and
  // controls how many sectors are batched per WriteAt call; the default
  // is tuned for real downloads, and tests override it to exercise the
  // multi-flush path without moving megabytes of fixture data.
  SectorAlignedWriter(DiskWriter* writer,
                       int64_t chunk_start,
                       int64_t sector_size,
                       bool is_final_chunk,
                       size_t target_buffer_bytes = kDefaultTargetBufferBytes);
  ~SectorAlignedWriter();

  SectorAlignedWriter(const SectorAlignedWriter&) = delete;
  SectorAlignedWriter& operator=(const SectorAlignedWriter&) = delete;

  // Appends the next contiguous slice of this chunk's data, in order.
  // Returns false on a write failure (from the underlying DiskWriter); the
  // caller should treat the whole chunk as failed and not call Append or
  // Finish again.
  bool Append(base::span<const uint8_t> data);

  // Flushes any buffered remainder. For the final chunk this issues one
  // short, padded write via DiskWriter's documented tail path. For any
  // other chunk, a nonzero remainder here means the fetch ended short of a
  // sector boundary unexpectedly, which is reported as failure rather than
  // silently written.
  bool Finish();

  // Absolute file offset of the next byte this writer expects via Append --
  // chunk_start plus every byte accepted so far, whether already flushed to
  // disk or still buffered. Exposed for progress reporting; resume itself
  // only persists whole completed chunks (see DownloadState), so a chunk
  // killed mid-flight simply restarts from chunk_start rather than resuming
  // from this value.
  int64_t next_offset() const { return next_offset_; }

  static constexpr size_t kDefaultTargetBufferBytes = 1024 * 1024;

 private:
  // Writes as many whole sectors as are currently buffered, starting at
  // `write_offset_`, advancing both `write_offset_` and `next_offset_`.
  bool FlushWholeSectors();

  DiskWriter* writer_;
  int64_t sector_size_;
  bool is_final_chunk_;

  // Offset of the next byte to be written to disk (i.e. the start of the
  // buffered-but-not-yet-written region). Distinct from next_offset_, which
  // also counts bytes still sitting in the buffer.
  int64_t write_offset_;
  int64_t next_offset_;

  AlignedBuffer buffer_;
  // Bytes currently held in `buffer_`, always < sector_size_ once
  // FlushWholeSectors has run (that invariant is what makes the "at most
  // one short tail write" reasoning in Finish() correct).
  size_t buffered_bytes_ = 0;

  bool failed_ = false;
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TBP_SECTOR_ALIGNED_WRITER_H_
