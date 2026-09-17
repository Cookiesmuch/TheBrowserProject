// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_disk_writer.h"

#include <stdint.h>

#include <numeric>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace tbp_download {
namespace {

// Deterministic, position-dependent bytes, so a test failure tells you *where*
// the file went wrong rather than just that it did.
std::vector<uint8_t> PatternBytes(int64_t offset, size_t length) {
  std::vector<uint8_t> out(length);
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>((offset + static_cast<int64_t>(i)) % 251);
  }
  return out;
}

class TbpDiskWriterTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath PathFor(const std::string& name) {
    return temp_dir_.GetPath().AppendASCII(name);
  }

  int64_t SectorSize() {
    const int64_t sector = DiskWriter::QuerySectorSize(temp_dir_.GetPath());
    EXPECT_GT(sector, 0);
    return sector;
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(TbpDiskWriterTest, QuerySectorSizeReturnsPowerOfTwo) {
  const int64_t sector = SectorSize();
  EXPECT_GT(sector, 0);
  EXPECT_EQ(0, sector & (sector - 1)) << "sector size " << sector
                                      << " is not a power of two";
  // Every real volume is in this range; anything outside means we queried the
  // wrong thing (e.g. cluster size instead of sector size).
  EXPECT_GE(sector, 512);
  EXPECT_LE(sector, 65536);
}

TEST_F(TbpDiskWriterTest, AlignedBufferIsSectorAligned) {
  AlignedBuffer buffer;
  ASSERT_TRUE(buffer.Allocate(4096));
  ASSERT_TRUE(buffer.valid());
  EXPECT_GE(buffer.size(), 4096u);
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(buffer.data()) % 4096)
      << "buffer is not page-aligned, so unbuffered writes will be rejected";

  buffer.Reset();
  EXPECT_FALSE(buffer.valid());
}

TEST_F(TbpDiskWriterTest, AlignedBufferRejectsZeroSize) {
  AlignedBuffer buffer;
  EXPECT_FALSE(buffer.Allocate(0));
}

TEST_F(TbpDiskWriterTest, WritesExactBytesForSectorMultipleFile) {
  const int64_t sector = SectorSize();
  const int64_t total = sector * 8;
  const base::FilePath path = PathFor("aligned.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total));
  EXPECT_TRUE(writer.is_open());

  // Write out of order, as parallel chunk workers would.
  const std::vector<int64_t> order = {sector * 4, 0, sector * 6, sector * 2};
  for (int64_t offset : order) {
    const std::vector<uint8_t> data =
        PatternBytes(offset, static_cast<size_t>(sector * 2));
    AlignedBuffer buffer;
    ASSERT_TRUE(buffer.Allocate(data.size()));
    buffer.span().first(data.size()).copy_from(data);
    ASSERT_TRUE(writer.WriteAt(offset, buffer.span().first(data.size())))
        << "write failed at offset " << offset;
  }

  ASSERT_TRUE(writer.Finish(total));
  EXPECT_FALSE(writer.is_open());

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  ASSERT_EQ(static_cast<size_t>(total), contents.size());

  const std::vector<uint8_t> expected =
      PatternBytes(0, static_cast<size_t>(total));
  EXPECT_EQ(base::span(expected), base::as_byte_span(contents))
      << "file contents do not match what was written";
}

TEST_F(TbpDiskWriterTest, HandlesRaggedTailNotOnSectorBoundary) {
  // The realistic case: almost no real file is an exact multiple of the sector
  // size, so the final write is short and must be padded then truncated away.
  const int64_t sector = SectorSize();
  const int64_t total = sector * 3 + 1234;
  const base::FilePath path = PathFor("ragged.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total));

  const std::vector<uint8_t> data =
      PatternBytes(0, static_cast<size_t>(total));
  AlignedBuffer buffer;
  ASSERT_TRUE(buffer.Allocate(data.size()));
  buffer.span().first(data.size()).copy_from(data);
  ASSERT_TRUE(writer.WriteAt(0, buffer.span().first(data.size())));
  ASSERT_TRUE(writer.Finish(total));

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(static_cast<size_t>(total), contents.size())
      << "sector padding was not truncated away";
  EXPECT_EQ(base::span(data), base::as_byte_span(contents));
}

TEST_F(TbpDiskWriterTest, AcceptsUnalignedCallerBuffer) {
  // Callers should use AlignedBuffer, but a plain vector must not silently
  // corrupt the file — the writer copies into an aligned buffer instead.
  const int64_t sector = SectorSize();
  const base::FilePath path = PathFor("unaligned_src.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, sector));

  std::vector<uint8_t> data = PatternBytes(0, static_cast<size_t>(sector));
  ASSERT_TRUE(writer.WriteAt(0, base::span<const uint8_t>(data)));
  ASSERT_TRUE(writer.Finish(sector));

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  ASSERT_EQ(static_cast<size_t>(sector), contents.size());
  EXPECT_EQ(base::span(data), base::as_byte_span(contents));
}

TEST_F(TbpDiskWriterTest, RejectsUnalignedOffset) {
  const int64_t sector = SectorSize();
  const base::FilePath path = PathFor("bad_offset.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, sector * 2));

  const std::vector<uint8_t> data = PatternBytes(0, 64);
  // An offset mid-sector is exactly what ChunkScheduler's alignment rules
  // exist to prevent; the writer must refuse rather than corrupt.
  EXPECT_FALSE(writer.WriteAt(1, base::span<const uint8_t>(data)));
  EXPECT_FALSE(writer.WriteAt(sector - 1, base::span<const uint8_t>(data)));
  EXPECT_FALSE(writer.WriteAt(sector + 7, base::span<const uint8_t>(data)));
}

TEST_F(TbpDiskWriterTest, PreallocationReservesSpaceUpFront) {
  const int64_t sector = SectorSize();
  const int64_t total = sector * 64;
  const base::FilePath path = PathFor("prealloc.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total));

  // Before anything is written, the file should already be at least the
  // requested length — that is the point of reserving.
  std::optional<int64_t> size_on_disk = base::GetFileSize(path);
  ASSERT_TRUE(size_on_disk.has_value());
  EXPECT_GE(size_on_disk.value(), total);

  // Whether the *fast* path was taken depends on whether this process holds
  // SeManageVolumePrivilege, which a dev/CI machine generally will not. Both
  // outcomes are correct; only the speed differs. Assert we can at least report
  // which happened rather than leaving it undefined.
  const bool fast = writer.used_fast_preallocation();
  SUCCEED() << "fast preallocation " << (fast ? "used" : "unavailable");

  writer.Close();
}

TEST_F(TbpDiskWriterTest, WorksWithoutKnownTotalSize) {
  // Chunked transfer encoding / no Content-Length: nothing to reserve.
  const int64_t sector = SectorSize();
  const base::FilePath path = PathFor("unknown_size.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, DiskWriter::kUnknownTotalSize));
  EXPECT_FALSE(writer.used_fast_preallocation());

  const std::vector<uint8_t> data =
      PatternBytes(0, static_cast<size_t>(sector));
  ASSERT_TRUE(writer.WriteAt(0, base::span<const uint8_t>(data)));
  ASSERT_TRUE(writer.Finish(sector));

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(static_cast<size_t>(sector), contents.size());
}

TEST_F(TbpDiskWriterTest, AbandonedDownloadDoesNotLeaveAPartialFile) {
  // SetFileValidData exposes whatever was previously in the reserved clusters.
  // An abandoned download must not leave that lying around on disk.
  const int64_t sector = SectorSize();
  const base::FilePath path = PathFor("abandoned.bin");

  {
    DiskWriter writer;
    ASSERT_TRUE(writer.Open(path, sector * 16));
    const std::vector<uint8_t> data =
        PatternBytes(0, static_cast<size_t>(sector));
    ASSERT_TRUE(writer.WriteAt(0, base::span<const uint8_t>(data)));
    // Destructed without Finish() — the cancel/crash path.
  }

  EXPECT_FALSE(base::PathExists(path))
      << "a partial download was left on disk after being abandoned";
}

TEST_F(TbpDiskWriterTest, FinishTruncatesPreallocationSlack) {
  const int64_t sector = SectorSize();
  const int64_t reserved = sector * 32;
  const int64_t actual = sector * 2 + 10;
  const base::FilePath path = PathFor("slack.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, reserved));
  const std::vector<uint8_t> data =
      PatternBytes(0, static_cast<size_t>(actual));
  ASSERT_TRUE(writer.WriteAt(0, base::span<const uint8_t>(data)));
  ASSERT_TRUE(writer.Finish(actual));

  std::optional<int64_t> size_on_disk = base::GetFileSize(path);
  ASSERT_TRUE(size_on_disk.has_value());
  EXPECT_EQ(actual, size_on_disk.value())
      << "reserved-but-unused space was not truncated";
}

TEST_F(TbpDiskWriterTest, WriteOnClosedWriterFails) {
  const base::FilePath path = PathFor("closed.bin");
  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, 4096));
  writer.Close();
  EXPECT_FALSE(writer.is_open());

  const std::vector<uint8_t> data = PatternBytes(0, 512);
  EXPECT_FALSE(writer.WriteAt(0, base::span<const uint8_t>(data)));
}

TEST_F(TbpDiskWriterTest, EnableFastPreallocationIsIdempotent) {
  // Whether it succeeds depends on the account's rights; what matters is that
  // asking twice gives the same answer and does not crash or leak.
  const bool first = DiskWriter::EnableFastPreallocation();
  const bool second = DiskWriter::EnableFastPreallocation();
  EXPECT_EQ(first, second);
}

}  // namespace
}  // namespace tbp_download
