// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_chunk_scheduler.h"

#include <algorithm>

namespace tbp_download {

namespace {

constexpr int64_t kMiB = 1024 * 1024;

// Splits the inclusive range [start, end] into at most `max_pieces` chunks
// whose starts are all sector-aligned.
//
// The final chunk absorbs the remainder left over by alignment rounding, so it
// is the largest. Returns a single chunk when the range cannot be usefully
// divided (one piece requested, or a sector larger than the would-be piece).
std::vector<Chunk> SplitRange(int64_t start,
                              int64_t end,
                              int max_pieces,
                              const SchedulerConfig& config) {
  const int64_t length = end - start + 1;
  if (max_pieces <= 1 || length <= 0) {
    return {Chunk{start, end}};
  }

  // Never produce a piece smaller than min_chunk_size.
  int64_t pieces = max_pieces;
  if (config.min_chunk_size > 0) {
    pieces = std::min<int64_t>(pieces, length / config.min_chunk_size);
  }
  if (pieces <= 1) {
    return {Chunk{start, end}};
  }

  // Round the piece size down to a sector boundary so every boundary we emit
  // is a legal offset for an unbuffered write. Rounding down (rather than up)
  // guarantees the pieces still cover the range with the tail absorbing the
  // remainder.
  const int64_t piece =
      ChunkScheduler::AlignDown(length / pieces, config.sector_size);
  if (piece <= 0) {
    // The range is smaller than one sector's worth per piece; not worth
    // splitting, and splitting would emit unaligned boundaries.
    return {Chunk{start, end}};
  }

  std::vector<Chunk> chunks;
  chunks.reserve(static_cast<size_t>(pieces));
  int64_t offset = start;
  for (int64_t i = 0; i < pieces - 1; ++i) {
    chunks.push_back(Chunk{offset, offset + piece - 1});
    offset += piece;
  }
  // Last piece runs to the true end of the range.
  chunks.push_back(Chunk{offset, end});
  return chunks;
}

// Sorts and coalesces overlapping or touching ranges.
std::vector<Chunk> Coalesce(std::vector<Chunk> ranges) {
  if (ranges.empty()) {
    return ranges;
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const Chunk& a, const Chunk& b) { return a.start < b.start; });

  std::vector<Chunk> merged;
  merged.push_back(ranges.front());
  for (size_t i = 1; i < ranges.size(); ++i) {
    Chunk& last = merged.back();
    const Chunk& next = ranges[i];
    // Touching counts as overlapping: [0,9] and [10,19] merge into [0,19].
    if (next.start <= last.end + 1) {
      last.end = std::max(last.end, next.end);
    } else {
      merged.push_back(next);
    }
  }
  return merged;
}

}  // namespace

int64_t Chunk::size() const {
  if (end == kUnknownEnd) {
    return kUnknownSize;
  }
  return end - start + 1;
}

bool Chunk::operator==(const Chunk& other) const {
  return start == other.start && end == other.end;
}

bool SchedulerConfig::IsValid() const {
  return max_connections >= 1 && min_chunk_size >= 0 && sector_size > 0 &&
         (sector_size & (sector_size - 1)) == 0;  // power of two
}

// static
int64_t ChunkScheduler::AlignDown(int64_t offset, int64_t sector_size) {
  if (sector_size <= 0 || offset <= 0) {
    return 0;
  }
  return offset - (offset % sector_size);
}

// static
SchedulerConfig ChunkScheduler::ConfigForProfile(DownloadProfile profile) {
  SchedulerConfig config;
  config.sector_size = 4096;
  switch (profile) {
    case DownloadProfile::kMaxPerformance:
      config.max_connections = 32;
      config.min_chunk_size = 1 * kMiB;
      break;
    case DownloadProfile::kBalanced:
      config.max_connections = 8;
      config.min_chunk_size = 2 * kMiB;
      break;
    case DownloadProfile::kBackground:
      config.max_connections = 2;
      config.min_chunk_size = 8 * kMiB;
      break;
  }
  return config;
}

// static
std::vector<Chunk> ChunkScheduler::Split(int64_t total_size,
                                         bool supports_ranges,
                                         const SchedulerConfig& config) {
  // An unknown length cannot be divided: we have no idea where to cut. Fetch it
  // as a single open-ended stream.
  if (total_size == kUnknownSize || total_size <= 0) {
    return {Chunk{0, kUnknownEnd}};
  }

  // Without range support every worker would have to start at byte zero, which
  // is strictly worse than one worker.
  if (!supports_ranges) {
    return {Chunk{0, total_size - 1}};
  }

  return SplitRange(0, total_size - 1, config.max_connections, config);
}

// static
std::vector<Chunk> ChunkScheduler::RemainingChunks(
    int64_t total_size,
    const std::vector<Chunk>& completed,
    const SchedulerConfig& config) {
  if (total_size == kUnknownSize || total_size <= 0) {
    return {Chunk{0, kUnknownEnd}};
  }

  // Drop anything malformed before doing arithmetic on it: a persisted chunk
  // map is read back from disk and is not inherently trustworthy.
  std::vector<Chunk> valid;
  valid.reserve(completed.size());
  for (const Chunk& range : completed) {
    if (range.start >= 0 && range.end >= range.start) {
      valid.push_back(range);
    }
  }
  const std::vector<Chunk> done = Coalesce(std::move(valid));

  // Walk the completed ranges and collect the holes between them.
  std::vector<Chunk> gaps;
  int64_t cursor = 0;
  for (const Chunk& range : done) {
    if (range.start > cursor) {
      gaps.push_back(Chunk{cursor, std::min(range.start - 1, total_size - 1)});
    }
    cursor = std::max(cursor, range.end + 1);
    if (cursor >= total_size) {
      break;
    }
  }
  if (cursor < total_size) {
    gaps.push_back(Chunk{cursor, total_size - 1});
  }

  if (gaps.empty()) {
    return {};
  }

  // A gap may begin mid-sector, because the completed ranges are wherever the
  // previous run happened to stop. The writer can only position on a sector
  // boundary, so pull each gap's start back to one. This re-downloads at most
  // sector_size - 1 bytes that are already on disk, which is a rounding error
  // against any real download and far cheaper than the alternative of a
  // read-modify-write on the tail sector.
  for (Chunk& gap : gaps) {
    gap.start = AlignDown(gap.start, config.sector_size);
  }
  // Aligning starts backwards can make a gap touch its predecessor; re-merge.
  gaps = Coalesce(std::move(gaps));

  int64_t remaining_bytes = 0;
  for (const Chunk& gap : gaps) {
    remaining_bytes += gap.size();
  }
  if (remaining_bytes <= 0) {
    return {};
  }

  // Spread the connection budget over the gaps in proportion to their size, so
  // resuming a download with one big hole and one small one puts most of the
  // workers on the big one. Every gap gets at least one connection.
  std::vector<Chunk> result;
  int budget_left = config.max_connections;
  for (size_t i = 0; i < gaps.size(); ++i) {
    const bool is_last = (i + 1 == gaps.size());
    int share;
    if (is_last) {
      share = std::max(1, budget_left);
    } else {
      const int64_t scaled =
          static_cast<int64_t>(config.max_connections) * gaps[i].size() /
          remaining_bytes;
      // Leave at least one connection for each gap still to come.
      const int reserve = static_cast<int>(gaps.size() - i - 1);
      share = std::max(1, std::min(static_cast<int>(scaled),
                                   std::max(1, budget_left - reserve)));
    }
    budget_left -= share;

    std::vector<Chunk> pieces = SplitRange(gaps[i].start, gaps[i].end, share, config);
    result.insert(result.end(), pieces.begin(), pieces.end());
  }
  return result;
}

}  // namespace tbp_download
