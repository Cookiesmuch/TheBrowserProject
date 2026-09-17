// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TBP_CHUNK_SCHEDULER_H_
#define COMPONENTS_TBP_DOWNLOAD_TBP_CHUNK_SCHEDULER_H_

#include <stdint.h>

#include <vector>

namespace tbp_download {

// Sentinel for a length that the server did not report (no Content-Length, or
// chunked transfer encoding). A download of unknown length cannot be split.
inline constexpr int64_t kUnknownSize = -1;

// Sentinel for a chunk whose end is not known ahead of time. Only ever appears
// on the final chunk of an unknown-length download, which is fetched with an
// open-ended "Range: bytes=N-" request.
inline constexpr int64_t kUnknownEnd = -1;

// One contiguous byte range assigned to exactly one worker. Both bounds are
// inclusive, matching HTTP Range semantics ("bytes=0-1023" is 1024 bytes), so
// that a Chunk maps to a request header without any off-by-one adjustment.
struct Chunk {
  int64_t start = 0;
  int64_t end = kUnknownEnd;

  // Length in bytes, or kUnknownSize if `end` is kUnknownEnd.
  int64_t size() const;

  bool operator==(const Chunk& other) const;
};

// Named performance presets. The Background profile is what game-mode
// throttling switches to in order to free bandwidth.
enum class DownloadProfile {
  kMaxPerformance,
  kBalanced,
  kBackground,
};

struct SchedulerConfig {
  // Upper bound on concurrent connections for a single download. The scheduler
  // may use fewer when the file is too small to justify them.
  int max_connections = 1;

  // A file is never split into pieces smaller than this. Prevents shredding a
  // small file into dozens of requests whose per-request overhead costs more
  // than the parallelism saves.
  int64_t min_chunk_size = 0;

  // Physical sector size of the destination volume. Every chunk boundary the
  // scheduler emits is aligned to this, because the disk writer opens its
  // handle with FILE_FLAG_NO_BUFFERING and Windows rejects unaligned writes.
  //
  // The sole exception is the final chunk's `end`, which lands on the real end
  // of the file and is therefore only sector-aligned by coincidence. The writer
  // handles that tail by writing one padded sector and then truncating to the
  // true length, which is the standard technique for unbuffered I/O.
  int64_t sector_size = 4096;

  bool IsValid() const;
};

// Splitting policy for the download engine. Pure computation: no I/O, no
// network, no threading, no Chromium dependencies beyond the standard library.
// Every decision about how many connections to open and where to cut the file
// lives here so it can be tested exhaustively without a fixture.
class ChunkScheduler {
 public:
  static SchedulerConfig ConfigForProfile(DownloadProfile profile);

  // Splits a download into chunks.
  //
  // Returns a single chunk covering the whole file when the size is unknown,
  // when the server does not advertise range support, or when the file is too
  // small to split under `config`. Never returns an empty vector for a
  // non-negative size, so callers always have at least one unit of work.
  static std::vector<Chunk> Split(int64_t total_size,
                                  bool supports_ranges,
                                  const SchedulerConfig& config);

  // Given the ranges already on disk, returns the work still outstanding.
  // `completed` need not be sorted or merged; overlapping and adjacent entries
  // are coalesced before the gaps are computed. This is the resume path: after
  // a crash the store hands back whatever was durably recorded, and whatever
  // is missing gets rescheduled.
  static std::vector<Chunk> RemainingChunks(int64_t total_size,
                                            const std::vector<Chunk>& completed,
                                            const SchedulerConfig& config);

  // Rounds `offset` down to a sector boundary. Exposed for the disk writer,
  // which needs the same arithmetic when positioning a write.
  static int64_t AlignDown(int64_t offset, int64_t sector_size);
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TBP_CHUNK_SCHEDULER_H_
