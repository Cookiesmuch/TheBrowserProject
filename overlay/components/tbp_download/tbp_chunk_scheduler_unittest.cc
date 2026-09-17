// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_chunk_scheduler.h"

#include <numeric>

#include "testing/gtest/include/gtest/gtest.h"

namespace tbp_download {
namespace {

constexpr int64_t kMiB = 1024 * 1024;

SchedulerConfig TestConfig(int max_connections,
                           int64_t min_chunk_size,
                           int64_t sector_size = 4096) {
  SchedulerConfig config;
  config.max_connections = max_connections;
  config.min_chunk_size = min_chunk_size;
  config.sector_size = sector_size;
  return config;
}

// Asserts the invariants that must hold for *any* chunk list the scheduler
// produces for a known-size download, regardless of inputs:
//   - chunks are ordered, non-overlapping, and contiguous
//   - together they cover exactly [0, total_size - 1]
//   - every start is sector-aligned (the disk writer cannot position anywhere
//     else), with the final end exempt since it is the true end of the file
void ExpectCoversExactly(const std::vector<Chunk>& chunks,
                         int64_t total_size,
                         int64_t sector_size) {
  ASSERT_FALSE(chunks.empty());
  EXPECT_EQ(0, chunks.front().start);
  EXPECT_EQ(total_size - 1, chunks.back().end);

  for (size_t i = 0; i < chunks.size(); ++i) {
    EXPECT_EQ(0, chunks[i].start % sector_size)
        << "chunk " << i << " starts at " << chunks[i].start
        << ", which is not sector-aligned";
    EXPECT_LE(chunks[i].start, chunks[i].end) << "chunk " << i << " is empty";
    if (i > 0) {
      EXPECT_EQ(chunks[i - 1].end + 1, chunks[i].start)
          << "gap or overlap between chunk " << (i - 1) << " and " << i;
    }
  }

  const int64_t covered = std::accumulate(
      chunks.begin(), chunks.end(), int64_t{0},
      [](int64_t sum, const Chunk& c) { return sum + c.size(); });
  EXPECT_EQ(total_size, covered);
}

TEST(TbpChunkSchedulerTest, ChunkSizeIsInclusive) {
  // HTTP Range semantics: "bytes=0-1023" is 1024 bytes, not 1023.
  EXPECT_EQ(1024, (Chunk{0, 1023}).size());
  EXPECT_EQ(1, (Chunk{5, 5}).size());
  EXPECT_EQ(kUnknownSize, (Chunk{0, kUnknownEnd}).size());
}

TEST(TbpChunkSchedulerTest, UnknownSizeIsNotSplit) {
  const auto chunks =
      ChunkScheduler::Split(kUnknownSize, /*supports_ranges=*/true,
                            TestConfig(32, kMiB));
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(0, chunks[0].start);
  EXPECT_EQ(kUnknownEnd, chunks[0].end);
}

TEST(TbpChunkSchedulerTest, EmptyFileYieldsOneChunk) {
  const auto chunks =
      ChunkScheduler::Split(0, /*supports_ranges=*/true, TestConfig(32, kMiB));
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(0, chunks[0].start);
}

TEST(TbpChunkSchedulerTest, NoRangeSupportYieldsOneChunk) {
  // Splitting without range support would force every worker to start at byte
  // zero, which is strictly worse than a single connection.
  const auto chunks = ChunkScheduler::Split(
      100 * kMiB, /*supports_ranges=*/false, TestConfig(32, kMiB));
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(0, chunks[0].start);
  EXPECT_EQ(100 * kMiB - 1, chunks[0].end);
}

TEST(TbpChunkSchedulerTest, SmallFileIsNotShredded) {
  // 3 MiB with a 1 MiB floor may use at most 3 connections, not 32.
  const auto chunks = ChunkScheduler::Split(
      3 * kMiB, /*supports_ranges=*/true, TestConfig(32, kMiB));
  EXPECT_LE(chunks.size(), 3u);
  ExpectCoversExactly(chunks, 3 * kMiB, 4096);
}

TEST(TbpChunkSchedulerTest, FileBelowMinimumUsesSingleConnection) {
  const auto chunks = ChunkScheduler::Split(
      512 * 1024, /*supports_ranges=*/true, TestConfig(32, kMiB));
  ASSERT_EQ(1u, chunks.size());
  ExpectCoversExactly(chunks, 512 * 1024, 4096);
}

TEST(TbpChunkSchedulerTest, LargeFileSplitsAcrossAllConnections) {
  const auto chunks = ChunkScheduler::Split(
      1024 * kMiB, /*supports_ranges=*/true, TestConfig(32, kMiB));
  EXPECT_EQ(32u, chunks.size());
  ExpectCoversExactly(chunks, 1024 * kMiB, 4096);
}

TEST(TbpChunkSchedulerTest, BoundariesAlignedForAwkwardSizes) {
  // Sizes deliberately chosen to be prime-ish / not sector multiples, to catch
  // any split arithmetic that only happens to work on round numbers.
  const int64_t kAwkwardSizes[] = {
      4097, 5 * kMiB + 1, 100 * kMiB - 1, 7777777, 12345678901,
  };
  for (int64_t size : kAwkwardSizes) {
    const auto chunks =
        ChunkScheduler::Split(size, /*supports_ranges=*/true,
                              TestConfig(16, kMiB));
    SCOPED_TRACE(testing::Message() << "total_size=" << size);
    ExpectCoversExactly(chunks, size, 4096);
  }
}

TEST(TbpChunkSchedulerTest, RespectsNonDefaultSectorSize) {
  // Some NVMe drives report 512, others 4096; a few report larger.
  for (int64_t sector : {512, 4096, 65536}) {
    const auto chunks = ChunkScheduler::Split(
        512 * kMiB, /*supports_ranges=*/true, TestConfig(8, kMiB, sector));
    SCOPED_TRACE(testing::Message() << "sector_size=" << sector);
    ExpectCoversExactly(chunks, 512 * kMiB, sector);
  }
}

TEST(TbpChunkSchedulerTest, ProfilesDifferInAggression) {
  const auto max = ChunkScheduler::ConfigForProfile(
      DownloadProfile::kMaxPerformance);
  const auto balanced =
      ChunkScheduler::ConfigForProfile(DownloadProfile::kBalanced);
  const auto background =
      ChunkScheduler::ConfigForProfile(DownloadProfile::kBackground);

  EXPECT_GT(max.max_connections, balanced.max_connections);
  EXPECT_GT(balanced.max_connections, background.max_connections);
  EXPECT_TRUE(max.IsValid());
  EXPECT_TRUE(balanced.IsValid());
  EXPECT_TRUE(background.IsValid());
}

TEST(TbpChunkSchedulerTest, BackgroundProfileUsesFewerConnections) {
  const int64_t kSize = 1024 * kMiB;
  const auto fast = ChunkScheduler::Split(
      kSize, true, ChunkScheduler::ConfigForProfile(
                       DownloadProfile::kMaxPerformance));
  const auto slow = ChunkScheduler::Split(
      kSize, true,
      ChunkScheduler::ConfigForProfile(DownloadProfile::kBackground));
  EXPECT_GT(fast.size(), slow.size());
}

TEST(TbpChunkSchedulerTest, ConfigValidityRejectsNonPowerOfTwoSector) {
  EXPECT_FALSE(TestConfig(8, kMiB, 4095).IsValid());
  EXPECT_FALSE(TestConfig(8, kMiB, 0).IsValid());
  EXPECT_FALSE(TestConfig(0, kMiB, 4096).IsValid());
  EXPECT_TRUE(TestConfig(8, kMiB, 4096).IsValid());
}

// --- resume ---------------------------------------------------------------

TEST(TbpChunkSchedulerTest, NothingCompletedMeansEverythingRemains) {
  const auto chunks = ChunkScheduler::RemainingChunks(
      100 * kMiB, /*completed=*/{}, TestConfig(8, kMiB));
  ExpectCoversExactly(chunks, 100 * kMiB, 4096);
}

TEST(TbpChunkSchedulerTest, FullyCompletedMeansNothingRemains) {
  const std::vector<Chunk> done = {{0, 100 * kMiB - 1}};
  const auto chunks =
      ChunkScheduler::RemainingChunks(100 * kMiB, done, TestConfig(8, kMiB));
  EXPECT_TRUE(chunks.empty());
}

TEST(TbpChunkSchedulerTest, ResumesOnlyTheMissingTail) {
  // Classic resume: the first half landed before the process died.
  const int64_t kTotal = 100 * kMiB;
  const std::vector<Chunk> done = {{0, 50 * kMiB - 1}};
  const auto chunks =
      ChunkScheduler::RemainingChunks(kTotal, done, TestConfig(8, kMiB));

  ASSERT_FALSE(chunks.empty());
  EXPECT_EQ(50 * kMiB, chunks.front().start);
  EXPECT_EQ(kTotal - 1, chunks.back().end);
  for (const Chunk& c : chunks) {
    EXPECT_EQ(0, c.start % 4096);
    EXPECT_GE(c.start, 50 * kMiB);
  }
}

TEST(TbpChunkSchedulerTest, ResumeParallelizesTheRemainder) {
  // A large remaining tail should still be spread over several connections
  // rather than being handed to a single worker.
  const int64_t kTotal = 1024 * kMiB;
  const std::vector<Chunk> done = {{0, 100 * kMiB - 1}};
  const auto chunks =
      ChunkScheduler::RemainingChunks(kTotal, done, TestConfig(8, kMiB));
  EXPECT_GT(chunks.size(), 1u) << "resume collapsed to a single connection";
}

TEST(TbpChunkSchedulerTest, FillsHolesBetweenCompletedRanges) {
  // Parallel workers finish out of order, leaving interior holes.
  const int64_t kTotal = 100 * kMiB;
  const std::vector<Chunk> done = {
      {0, 10 * kMiB - 1},
      {20 * kMiB, 30 * kMiB - 1},
      {90 * kMiB, kTotal - 1},
  };
  const auto chunks =
      ChunkScheduler::RemainingChunks(kTotal, done, TestConfig(8, kMiB));

  ASSERT_FALSE(chunks.empty());
  // Nothing already on disk should be rescheduled (modulo sector alignment,
  // which is exact here because every boundary is already MiB-aligned).
  for (const Chunk& c : chunks) {
    const bool in_first = c.start < 10 * kMiB;
    const bool in_second = c.start >= 20 * kMiB && c.start < 30 * kMiB;
    const bool in_third = c.start >= 90 * kMiB;
    EXPECT_FALSE(in_first || in_second || in_third)
        << "rescheduled an already-complete range at " << c.start;
  }
}

TEST(TbpChunkSchedulerTest, UnsortedAndOverlappingCompletedRangesAreCoalesced) {
  const int64_t kTotal = 100 * kMiB;
  // Deliberately out of order, overlapping, and touching.
  const std::vector<Chunk> done = {
      {40 * kMiB, 60 * kMiB - 1},
      {0, 20 * kMiB - 1},
      {10 * kMiB, 45 * kMiB - 1},  // overlaps both of the above
  };
  const auto chunks =
      ChunkScheduler::RemainingChunks(kTotal, done, TestConfig(8, kMiB));

  // Everything below 60 MiB is covered once the three are merged.
  for (const Chunk& c : chunks) {
    EXPECT_GE(c.start, 60 * kMiB);
  }
  EXPECT_EQ(kTotal - 1, chunks.back().end);
}

TEST(TbpChunkSchedulerTest, MisalignedResumePointIsPulledBackToSector) {
  // A previous run stopped mid-sector. The writer can only position on a
  // boundary, so the resume must start at or before that point, never after —
  // starting after would silently leave a hole in the file.
  const int64_t kTotal = 100 * kMiB;
  const std::vector<Chunk> done = {{0, 5000}};  // ends inside sector 1
  const auto chunks =
      ChunkScheduler::RemainingChunks(kTotal, done, TestConfig(4, kMiB));

  ASSERT_FALSE(chunks.empty());
  EXPECT_EQ(0, chunks.front().start % 4096);
  EXPECT_LE(chunks.front().start, 5001)
      << "resume skipped past the end of completed data, leaving a hole";
}

TEST(TbpChunkSchedulerTest, MalformedCompletedRangesAreIgnored) {
  // The chunk map is read back from disk and is not inherently trustworthy.
  const int64_t kTotal = 10 * kMiB;
  const std::vector<Chunk> done = {
      {-5, 100},                 // negative start
      {500, 499},                // inverted
      {0, kUnknownEnd},          // unknown end
      {0, 1 * kMiB - 1},         // the one real entry
  };
  const auto chunks =
      ChunkScheduler::RemainingChunks(kTotal, done, TestConfig(4, kMiB));

  ASSERT_FALSE(chunks.empty());
  EXPECT_EQ(1 * kMiB, chunks.front().start);
  EXPECT_EQ(kTotal - 1, chunks.back().end);
}

TEST(TbpChunkSchedulerTest, AlignDownHandlesEdges) {
  EXPECT_EQ(0, ChunkScheduler::AlignDown(0, 4096));
  EXPECT_EQ(0, ChunkScheduler::AlignDown(4095, 4096));
  EXPECT_EQ(4096, ChunkScheduler::AlignDown(4096, 4096));
  EXPECT_EQ(4096, ChunkScheduler::AlignDown(8191, 4096));
  EXPECT_EQ(0, ChunkScheduler::AlignDown(-1, 4096));
}

}  // namespace
}  // namespace tbp_download
