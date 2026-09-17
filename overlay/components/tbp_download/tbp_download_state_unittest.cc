// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_download_state.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace tbp_download {
namespace {

DownloadState MakeSampleState() {
  DownloadState state;
  state.url = "https://example.com/file.iso";
  state.total_size = 123456789012LL;  // Exceeds 32-bit range on purpose.
  state.supports_ranges = true;
  state.profile = DownloadProfile::kMaxPerformance;
  state.etag = "\"abc123\"";
  state.last_modified = "Wed, 21 Oct 2026 07:28:00 GMT";
  state.completed_chunks = {
      {0, 1023},
      {1024, 2047},
      {8192, kUnknownEnd},
  };
  return state;
}

TEST(TbpDownloadStateTest, RoundTripsSerializeDeserialize) {
  DownloadState original = MakeSampleState();
  std::string serialized = SerializeDownloadState(original);
  ASSERT_FALSE(serialized.empty());

  std::optional<DownloadState> parsed = DeserializeDownloadState(serialized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, original);
}

TEST(TbpDownloadStateTest, RoundTripsEmptyChunksAndEmptyStrings) {
  DownloadState state;
  state.url = "https://example.com/x";
  state.total_size = kUnknownSize;
  state.supports_ranges = false;
  state.profile = DownloadProfile::kBackground;
  // etag/last_modified deliberately left empty -- server didn't send them.

  std::string serialized = SerializeDownloadState(state);
  std::optional<DownloadState> parsed = DeserializeDownloadState(serialized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, state);
  EXPECT_TRUE(parsed->completed_chunks.empty());
}

TEST(TbpDownloadStateTest, RoundTripsExtremeInt64Values) {
  DownloadState state;
  state.url = "https://example.com/huge";
  state.total_size = INT64_MAX;
  state.completed_chunks = {{0, INT64_MAX}, {INT64_MAX - 1, kUnknownEnd}};

  std::string serialized = SerializeDownloadState(state);
  std::optional<DownloadState> parsed = DeserializeDownloadState(serialized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, state);
}

TEST(TbpDownloadStateTest, DeserializeRejectsGarbage) {
  EXPECT_FALSE(DeserializeDownloadState("not json at all").has_value());
  EXPECT_FALSE(DeserializeDownloadState("").has_value());
  EXPECT_FALSE(DeserializeDownloadState("[1, 2, 3]").has_value());  // valid JSON, wrong shape
  EXPECT_FALSE(DeserializeDownloadState("42").has_value());
}

TEST(TbpDownloadStateTest, DeserializeRejectsMissingFields) {
  // Missing "url".
  EXPECT_FALSE(DeserializeDownloadState(
                   R"({"total_size":"1","supports_ranges":true,)"
                   R"("profile":0,"etag":"","last_modified":"",)"
                   R"("completed_chunks":[]})")
                   .has_value());
  // total_size is a JSON number, not the required string encoding.
  EXPECT_FALSE(DeserializeDownloadState(
                   R"({"url":"x","total_size":1,"supports_ranges":true,)"
                   R"("profile":0,"etag":"","last_modified":"",)"
                   R"("completed_chunks":[]})")
                   .has_value());
}

TEST(TbpDownloadStateTest, DeserializeRejectsInvalidProfile) {
  EXPECT_FALSE(DeserializeDownloadState(
                   R"({"url":"x","total_size":"1","supports_ranges":true,)"
                   R"("profile":99,"etag":"","last_modified":"",)"
                   R"("completed_chunks":[]})")
                   .has_value());
}

TEST(TbpDownloadStateTest, DeserializeRejectsMalformedChunkEntries) {
  // A chunk entry missing "end".
  EXPECT_FALSE(DeserializeDownloadState(
                   R"({"url":"x","total_size":"1","supports_ranges":true,)"
                   R"("profile":0,"etag":"","last_modified":"",)"
                   R"("completed_chunks":[{"start":"0"}]})")
                   .has_value());
  // A chunk entry that isn't even an object.
  EXPECT_FALSE(DeserializeDownloadState(
                   R"({"url":"x","total_size":"1","supports_ranges":true,)"
                   R"("profile":0,"etag":"","last_modified":"",)"
                   R"("completed_chunks":["not an object"]})")
                   .has_value());
}

TEST(TbpDownloadStateTest, StateFilePathAppendsExtension) {
  base::FilePath destination(FILE_PATH_LITERAL("C:\\Downloads\\a.iso"));
  EXPECT_EQ(StateFilePathFor(destination).value(),
            FILE_PATH_LITERAL("C:\\Downloads\\a.iso.tbpstate"));
}

class TbpDownloadStateDiskTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath DestinationPath() const {
    return temp_dir_.GetPath().Append(FILE_PATH_LITERAL("download.bin"));
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(TbpDownloadStateDiskTest, SaveAndLoadRoundTripOnRealDisk) {
  DownloadState state = MakeSampleState();
  ASSERT_TRUE(SaveDownloadState(DestinationPath(), state));

  std::optional<DownloadState> loaded = LoadDownloadState(DestinationPath());
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, state);
}

TEST_F(TbpDownloadStateDiskTest, SaveLeavesNoTemporaryFileBehind) {
  DownloadState state = MakeSampleState();
  ASSERT_TRUE(SaveDownloadState(DestinationPath(), state));

  base::FilePath temp_path = StateFilePathFor(DestinationPath())
                                  .AddExtension(FILE_PATH_LITERAL(".tmp"));
  EXPECT_FALSE(base::PathExists(temp_path));
}

TEST_F(TbpDownloadStateDiskTest, SaveOverwritesPreviousState) {
  DownloadState first = MakeSampleState();
  ASSERT_TRUE(SaveDownloadState(DestinationPath(), first));

  DownloadState second = first;
  second.completed_chunks.push_back({4096, 5119});
  ASSERT_TRUE(SaveDownloadState(DestinationPath(), second));

  std::optional<DownloadState> loaded = LoadDownloadState(DestinationPath());
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, second);
  EXPECT_NE(*loaded, first);
}

TEST_F(TbpDownloadStateDiskTest, LoadReturnsNulloptWhenNoStateFileExists) {
  EXPECT_FALSE(LoadDownloadState(DestinationPath()).has_value());
}

TEST_F(TbpDownloadStateDiskTest, DeleteDownloadStateRemovesFile) {
  DownloadState state = MakeSampleState();
  ASSERT_TRUE(SaveDownloadState(DestinationPath(), state));
  ASSERT_TRUE(base::PathExists(StateFilePathFor(DestinationPath())));

  DeleteDownloadState(DestinationPath());
  EXPECT_FALSE(base::PathExists(StateFilePathFor(DestinationPath())));
}

TEST_F(TbpDownloadStateDiskTest, DeleteDownloadStateOnMissingFileIsSafe) {
  // Must not crash or error when there was never a state file to begin with.
  DeleteDownloadState(DestinationPath());
}

}  // namespace
}  // namespace tbp_download
