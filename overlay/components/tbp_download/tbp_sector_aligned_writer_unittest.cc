// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_sector_aligned_writer.h"

#include <numeric>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace tbp_download {
namespace {

std::vector<uint8_t> PatternBytes(int64_t offset, size_t length) {
  std::vector<uint8_t> out(length);
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>((offset + static_cast<int64_t>(i)) % 251);
  }
  return out;
}

std::string ToStringView(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

class TbpSectorAlignedWriterTest : public ::testing::Test {
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

TEST_F(TbpSectorAlignedWriterTest,
       ManySmallAppendsAcrossSectorsProduceExactBytes) {
  const int64_t sector = SectorSize();
  const int64_t total_size = sector * 4;  // Final chunk covers the whole file.
  base::FilePath path = PathFor("out.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total_size));

  std::vector<uint8_t> expected = PatternBytes(0, total_size);
  SectorAlignedWriter sector_writer(&writer, /*chunk_start=*/0, sector,
                                     /*is_final_chunk=*/true,
                                     /*target_buffer_bytes=*/sector * 8);

  // Deliberately awkward, sub-sector fragment sizes -- like real network
  // reads -- rather than one clean Append() per sector.
  size_t fragment = 37;
  base::span<const uint8_t> remaining(expected);
  while (!remaining.empty()) {
    size_t n = std::min(fragment, remaining.size());
    ASSERT_TRUE(sector_writer.Append(remaining.first(n)));
    remaining = remaining.subspan(n);
  }
  ASSERT_TRUE(sector_writer.Finish());
  ASSERT_TRUE(writer.Finish(total_size));

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  ASSERT_EQ(contents.size(), static_cast<size_t>(total_size));
  EXPECT_EQ(contents, ToStringView(expected));
}

TEST_F(TbpSectorAlignedWriterTest, FlushesAsSoonAsTargetBufferFills) {
  const int64_t sector = SectorSize();
  const int64_t total_size = sector * 10;
  base::FilePath path = PathFor("out.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total_size));

  // A tiny target buffer (2 sectors) forces multiple separate WriteAt
  // calls for a 6-sector chunk instead of one, exercising repeated
  // flush-and-continue rather than a single flush at Finish().
  std::vector<uint8_t> expected = PatternBytes(0, sector * 6);
  SectorAlignedWriter sector_writer(&writer, /*chunk_start=*/0, sector,
                                     /*is_final_chunk=*/false,
                                     /*target_buffer_bytes=*/sector * 2);
  ASSERT_TRUE(sector_writer.Append(expected));
  ASSERT_TRUE(sector_writer.Finish());

  // Not the final chunk, so pad the rest of the file for readback (WriteAt
  // was only asked to write the first 6 sectors).
  std::vector<uint8_t> tail(static_cast<size_t>(total_size - sector * 6), 0);
  ASSERT_TRUE(writer.WriteAt(sector * 6, tail));
  ASSERT_TRUE(writer.Finish(total_size));

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(contents.substr(0, expected.size()), ToStringView(expected));
}

TEST_F(TbpSectorAlignedWriterTest, FinalChunkAllowsShortTailWrite) {
  const int64_t sector = SectorSize();
  const int64_t total_size = sector * 3 + 100;  // Not a sector multiple.
  base::FilePath path = PathFor("out.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total_size));

  std::vector<uint8_t> expected = PatternBytes(0, total_size);
  SectorAlignedWriter sector_writer(&writer, /*chunk_start=*/0, sector,
                                     /*is_final_chunk=*/true);
  ASSERT_TRUE(sector_writer.Append(expected));
  ASSERT_TRUE(sector_writer.Finish());
  ASSERT_TRUE(writer.Finish(total_size));

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  ASSERT_EQ(contents.size(), static_cast<size_t>(total_size));
  EXPECT_EQ(contents, ToStringView(expected));
}

TEST_F(TbpSectorAlignedWriterTest,
       NonFinalChunkWithShortTailFailsRatherThanWritingPartialSector) {
  const int64_t sector = SectorSize();
  const int64_t total_size = sector * 4;
  base::FilePath path = PathFor("out.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total_size));

  // One full sector plus a short, non-sector-multiple remainder -- as if
  // the server closed the connection early, delivering fewer bytes than
  // this (non-final) chunk's range promised.
  std::vector<uint8_t> truncated = PatternBytes(0, sector + 10);
  SectorAlignedWriter sector_writer(&writer, /*chunk_start=*/0, sector,
                                     /*is_final_chunk=*/false);
  ASSERT_TRUE(sector_writer.Append(truncated));
  EXPECT_FALSE(sector_writer.Finish());
}

TEST_F(TbpSectorAlignedWriterTest, EmptyChunkFinishesTriviallySuccessfully) {
  const int64_t sector = SectorSize();
  base::FilePath path = PathFor("out.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, 0));

  SectorAlignedWriter sector_writer(&writer, /*chunk_start=*/0, sector,
                                     /*is_final_chunk=*/true);
  EXPECT_TRUE(sector_writer.Finish());
  EXPECT_EQ(sector_writer.next_offset(), 0);
}

TEST_F(TbpSectorAlignedWriterTest, NextOffsetTracksAcceptedBytes) {
  const int64_t sector = SectorSize();
  const int64_t total_size = sector * 2;
  base::FilePath path = PathFor("out.bin");

  DiskWriter writer;
  ASSERT_TRUE(writer.Open(path, total_size));

  SectorAlignedWriter sector_writer(&writer, /*chunk_start=*/1000, sector,
                                     /*is_final_chunk=*/false);
  EXPECT_EQ(sector_writer.next_offset(), 1000);
  std::vector<uint8_t> data(50, 7);
  ASSERT_TRUE(sector_writer.Append(data));
  EXPECT_EQ(sector_writer.next_offset(), 1050);
}

}  // namespace
}  // namespace tbp_download
