// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_download_task.h"

#include <algorithm>
#include <map>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/test/bind.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace tbp_download {
namespace {

std::vector<uint8_t> PatternBytes(size_t length) {
  std::vector<uint8_t> out(length);
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>(i % 251);
  }
  return out;
}

std::string ToStringView(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// Simulates a remote resource of fixed content, delivering bytes for any
// requested chunk in small, deliberately awkward fragments -- like a real
// network read -- rather than one clean callback per chunk.
class FakeChunkFetcher : public ChunkFetcher {
 public:
  explicit FakeChunkFetcher(std::vector<uint8_t> content)
      : content_(std::move(content)) {
    metadata_.total_size = static_cast<int64_t>(content_.size());
    metadata_.supports_ranges = true;
  }

  FetchMetadata& metadata() { return metadata_; }
  void SetFragmentSize(size_t size) { fragment_size_ = size; }

  // The chunk starting at `chunk_start` reports `outcome` for its next
  // `times` FetchChunk calls before finally succeeding. Never matches the
  // metadata probe (always exactly {0, 0}), even when `chunk_start` is 0 --
  // see FailProbeWith for that.
  void FailChunkNTimes(int64_t chunk_start, FetchOutcome outcome, int times) {
    fail_counts_[chunk_start] = {outcome, times};
  }

  // Makes the metadata probe itself report `outcome`, unconditionally.
  void FailProbeWith(FetchOutcome outcome) { probe_failure_ = outcome; }

  int CallCountFor(int64_t chunk_start) const {
    auto it = call_counts_.find(chunk_start);
    return it == call_counts_.end() ? 0 : it->second;
  }

  // When true, FetchChunk stores its request instead of resolving it, so
  // tests can exercise "still in flight" behavior (e.g. Cancel()).
  void SetDeferred(bool deferred) { deferred_ = deferred; }
  size_t PendingCount() const { return pending_.size(); }
  void CompletePending(size_t index) {
    PendingFetch pending = std::move(pending_[index]);
    pending_.erase(pending_.begin() + static_cast<ptrdiff_t>(index));
    DeliverAndComplete(pending.chunk, pending.on_data,
                        std::move(pending.on_done));
  }

  void FetchChunk(const std::string& url,
                   const Chunk& chunk,
                   ChunkDataCallback on_data,
                   ChunkDoneCallback on_done) override {
    if (!IsProbe(chunk)) {
      call_counts_[chunk.start]++;
    }

    if (deferred_) {
      pending_.push_back({chunk, on_data, std::move(on_done)});
      return;
    }
    DeliverAndComplete(chunk, on_data, std::move(on_done));
  }

 private:
  struct PendingFetch {
    Chunk chunk;
    ChunkDataCallback on_data;
    ChunkDoneCallback on_done;
  };

  // The metadata probe is always exactly this 1-byte range -- see
  // DownloadTask::StartProbe. Distinguishing it matters because a small
  // file's one real chunk can itself legitimately start at 0, and probe
  // failures/counts must never be conflated with that real chunk's.
  static bool IsProbe(const Chunk& chunk) {
    return chunk.start == 0 && chunk.end == 0;
  }

  void DeliverAndComplete(const Chunk& chunk,
                           ChunkDataCallback on_data,
                           ChunkDoneCallback on_done) {
    if (IsProbe(chunk) && probe_failure_.has_value()) {
      std::move(on_done).Run(FetchResult{*probe_failure_, metadata_});
      return;
    }
    if (!IsProbe(chunk)) {
      auto fail_it = fail_counts_.find(chunk.start);
      if (fail_it != fail_counts_.end() && fail_it->second.second > 0) {
        fail_it->second.second--;
        std::move(on_done).Run(FetchResult{fail_it->second.first, metadata_});
        return;
      }
    }

    int64_t end = chunk.end == kUnknownEnd
                      ? static_cast<int64_t>(content_.size()) - 1
                      : chunk.end;
    int64_t pos = chunk.start;
    while (pos <= end) {
      size_t n = std::min(fragment_size_,
                           static_cast<size_t>(end - pos + 1));
      on_data.Run(base::span(content_).subspan(static_cast<size_t>(pos), n));
      pos += static_cast<int64_t>(n);
    }
    std::move(on_done).Run(FetchResult{FetchOutcome::kSuccess, metadata_});
  }

  std::vector<uint8_t> content_;
  FetchMetadata metadata_;
  size_t fragment_size_ = 7;
  std::optional<FetchOutcome> probe_failure_;
  std::map<int64_t, std::pair<FetchOutcome, int>> fail_counts_;
  std::map<int64_t, int> call_counts_;
  bool deferred_ = false;
  std::vector<PendingFetch> pending_;
};

class TbpDownloadTaskTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath DestinationPath() const {
    return temp_dir_.GetPath().Append(FILE_PATH_LITERAL("download.bin"));
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(TbpDownloadTaskTest, DownloadsSmallFileSuccessfully) {
  std::vector<uint8_t> content = PatternBytes(500);
  FakeChunkFetcher fetcher(content);

  DownloadTask task("https://example.com/small.bin", DestinationPath(),
                     &fetcher);

  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(),
             base::BindOnce(
                 [](std::optional<DownloadTask::Result>* out,
                    DownloadTask::Result r) { *out = r; },
                 &result));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kSuccess);

  std::string on_disk;
  ASSERT_TRUE(base::ReadFileToString(DestinationPath(), &on_disk));
  EXPECT_EQ(on_disk, ToStringView(content));
  EXPECT_FALSE(base::PathExists(StateFilePathFor(DestinationPath())));
}

TEST_F(TbpDownloadTaskTest, DownloadsLargeFileAcrossMultipleChunks) {
  // kMaxPerformance's real min_chunk_size is 1 MiB (see
  // ChunkScheduler::ConfigForProfile) -- the file has to comfortably clear
  // that or the scheduler correctly refuses to split it at all, which
  // would make this test pass for the wrong reason. 8 MiB guarantees
  // several whole chunks regardless of the real volume's sector size.
  const size_t file_size = 8 * 1024 * 1024;
  std::vector<uint8_t> content = PatternBytes(file_size);
  FakeChunkFetcher fetcher(content);

  DownloadTask task("https://example.com/large.bin", DestinationPath(),
                     &fetcher, DownloadProfile::kMaxPerformance);

  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(),
             base::BindLambdaForTesting(
                 [&](DownloadTask::Result r) { result = r; }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kSuccess);

  std::string on_disk;
  ASSERT_TRUE(base::ReadFileToString(DestinationPath(), &on_disk));
  EXPECT_EQ(on_disk, ToStringView(content));

  // The probe (chunk starting at 0, 1 byte) plus at least one more real
  // chunk fetch starting well past the beginning confirms this actually
  // parallelized rather than silently falling back to one connection.
  EXPECT_GT(fetcher.CallCountFor(static_cast<int64_t>(file_size) / 2), 0)
      << "expected a chunk starting well past the beginning of the file, "
         "indicating the download was actually split";
}

TEST_F(TbpDownloadTaskTest, RetriesTransientFailureAndStillSucceeds) {
  std::vector<uint8_t> content = PatternBytes(500);
  FakeChunkFetcher fetcher(content);
  // The whole file is one chunk starting at 0 (small file, single
  // connection) -- fail it once, then let it succeed.
  fetcher.FailChunkNTimes(0, FetchOutcome::kNetworkError, 1);

  DownloadTask task("https://example.com/flaky.bin", DestinationPath(),
                     &fetcher);
  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(), base::BindLambdaForTesting(
                                     [&](DownloadTask::Result r) {
                                       result = r;
                                     }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kSuccess);
  EXPECT_EQ(fetcher.CallCountFor(0), 2);  // one failure, one success

  std::string on_disk;
  ASSERT_TRUE(base::ReadFileToString(DestinationPath(), &on_disk));
  EXPECT_EQ(on_disk, ToStringView(content));
}

TEST_F(TbpDownloadTaskTest, FailsAfterExceedingRetryLimit) {
  std::vector<uint8_t> content = PatternBytes(500);
  FakeChunkFetcher fetcher(content);
  fetcher.FailChunkNTimes(0, FetchOutcome::kNetworkError, 100);

  DownloadTask task("https://example.com/broken.bin", DestinationPath(),
                     &fetcher);
  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(), base::BindLambdaForTesting(
                                     [&](DownloadTask::Result r) {
                                       result = r;
                                     }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kFailed);
}

TEST_F(TbpDownloadTaskTest, RangeNotSatisfiableFailsImmediatelyWithoutRetry) {
  std::vector<uint8_t> content = PatternBytes(500);
  FakeChunkFetcher fetcher(content);
  fetcher.FailChunkNTimes(0, FetchOutcome::kRangeNotSatisfiable, 100);

  DownloadTask task("https://example.com/moved.bin", DestinationPath(),
                     &fetcher);
  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(), base::BindLambdaForTesting(
                                     [&](DownloadTask::Result r) {
                                       result = r;
                                     }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kFailed);
  EXPECT_EQ(fetcher.CallCountFor(0), 1)
      << "a 416-equivalent should not be retried the way a network error is";
}

TEST_F(TbpDownloadTaskTest, ProbeFailureFailsTheWholeDownloadImmediately) {
  std::vector<uint8_t> content = PatternBytes(500);
  FakeChunkFetcher fetcher(content);
  fetcher.FailProbeWith(FetchOutcome::kServerError);

  DownloadTask task("https://example.com/404.bin", DestinationPath(),
                     &fetcher);
  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(), base::BindLambdaForTesting(
                                     [&](DownloadTask::Result r) {
                                       result = r;
                                     }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kFailed);
  EXPECT_FALSE(base::PathExists(DestinationPath()));
}

TEST_F(TbpDownloadTaskTest, CancelDuringInFlightFetchStopsTheTaskCleanly) {
  std::vector<uint8_t> content = PatternBytes(500);
  FakeChunkFetcher fetcher(content);
  fetcher.SetDeferred(true);

  DownloadTask task("https://example.com/slow.bin", DestinationPath(),
                     &fetcher);
  int done_calls = 0;
  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(),
             base::BindLambdaForTesting([&](DownloadTask::Result r) {
               result = r;
               done_calls++;
             }));

  // The probe is deferred, so Start() returns without the task having
  // finished yet.
  ASSERT_FALSE(result.has_value());
  ASSERT_EQ(fetcher.PendingCount(), 1u);

  task.Cancel();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kCancelled);
  EXPECT_EQ(done_calls, 1);

  // The probe eventually "arrives" after cancellation -- must be silently
  // dropped, not double-fire on_done or crash on a torn-down task.
  fetcher.CompletePending(0);
  EXPECT_EQ(done_calls, 1);
}

TEST_F(TbpDownloadTaskTest, ResumesFromPersistedStateAfterSimulatedRestart) {
  const int64_t sector = DiskWriter::QuerySectorSize(temp_dir_.GetPath());
  std::vector<uint8_t> content = PatternBytes(static_cast<size_t>(sector) * 4);
  const int64_t split_point = sector * 2;

  // Simulate a previous run that successfully finished the first half and
  // was then killed before the second half completed: the destination file
  // already holds the first half's real bytes, and a state file records
  // exactly that as the one completed chunk.
  {
    DiskWriter writer;
    ASSERT_TRUE(writer.Open(DestinationPath(),
                             static_cast<int64_t>(content.size())));
    ASSERT_TRUE(writer.WriteAt(
        0, base::span(content).first(static_cast<size_t>(split_point))));
    // Leave the file open at its preallocated size rather than calling
    // Finish() -- the download never completed, so nothing should have
    // truncated it to a final size yet either.
    writer.Close();
  }
  DownloadState prior_state;
  prior_state.url = "https://example.com/resumable.bin";
  prior_state.total_size = static_cast<int64_t>(content.size());
  prior_state.supports_ranges = true;
  prior_state.completed_chunks = {{0, split_point - 1}};
  ASSERT_TRUE(SaveDownloadState(DestinationPath(), prior_state));

  // "Restart": a brand new DownloadTask and a brand new fetcher (as if the
  // whole process had been relaunched), pointed at the same URL and
  // destination.
  FakeChunkFetcher fetcher(content);
  DownloadTask task(prior_state.url, DestinationPath(), &fetcher);

  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(), base::BindLambdaForTesting(
                                     [&](DownloadTask::Result r) {
                                       result = r;
                                     }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kSuccess);

  // The already-completed first half must never have been re-fetched.
  EXPECT_EQ(fetcher.CallCountFor(0), 0);

  std::string on_disk;
  ASSERT_TRUE(base::ReadFileToString(DestinationPath(), &on_disk));
  EXPECT_EQ(on_disk, ToStringView(content));
  EXPECT_FALSE(base::PathExists(StateFilePathFor(DestinationPath())));
}

TEST_F(TbpDownloadTaskTest, MismatchedUrlInStateFileIsIgnoredNotResumed) {
  std::vector<uint8_t> content = PatternBytes(500);
  DownloadState stale_state;
  stale_state.url = "https://example.com/a-different-file.bin";
  stale_state.total_size = static_cast<int64_t>(content.size());
  stale_state.completed_chunks = {{0, 249}};
  ASSERT_TRUE(SaveDownloadState(DestinationPath(), stale_state));

  FakeChunkFetcher fetcher(content);
  DownloadTask task("https://example.com/the-real-file.bin",
                     DestinationPath(), &fetcher);
  std::optional<DownloadTask::Result> result;
  task.Start(base::DoNothing(), base::BindLambdaForTesting(
                                     [&](DownloadTask::Result r) {
                                       result = r;
                                     }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, DownloadTask::Result::kSuccess);
  // A fresh probe must have happened rather than trusting the mismatched
  // state's completed_chunks.
  EXPECT_EQ(fetcher.CallCountFor(0), 1);

  std::string on_disk;
  ASSERT_TRUE(base::ReadFileToString(DestinationPath(), &on_disk));
  EXPECT_EQ(on_disk, ToStringView(content));
}

}  // namespace
}  // namespace tbp_download
