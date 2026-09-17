// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TBP_DOWNLOAD_TASK_H_
#define COMPONENTS_TBP_DOWNLOAD_TBP_DOWNLOAD_TASK_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "components/tbp_download/tbp_chunk_fetcher.h"
#include "components/tbp_download/tbp_chunk_scheduler.h"
#include "components/tbp_download/tbp_disk_writer.h"
#include "components/tbp_download/tbp_download_state.h"
#include "components/tbp_download/tbp_sector_aligned_writer.h"

namespace tbp_download {

// Drives one download end to end: discovers the remote resource's size and
// Range support, splits it into chunks, fetches them in parallel through an
// injected ChunkFetcher, writes each chunk's bytes to disk through a
// SectorAlignedWriter, and persists resume state as chunks complete.
//
// Lives entirely on one sequence -- there is no internal locking, because
// every method is expected to be called from (and every callback fires on)
// the same sequence, matching how DownloadFile's own contract already
// works. See components/download/public/common/download_file.h upstream
// for the pattern this mirrors.
class DownloadTask {
 public:
  struct Progress {
    int64_t bytes_written = 0;
    // kUnknownSize until the probe fetch (or resumed state) establishes it.
    int64_t total_size = kUnknownSize;
  };

  enum class Result {
    kSuccess,
    kFailed,
    kCancelled,
  };

  using ProgressCallback = base::RepeatingCallback<void(Progress)>;
  using DoneCallback = base::OnceCallback<void(Result)>;

  // `fetcher` is not owned and must outlive this task.
  DownloadTask(std::string url,
               base::FilePath destination,
               ChunkFetcher* fetcher,
               DownloadProfile profile = DownloadProfile::kBalanced);
  ~DownloadTask();

  DownloadTask(const DownloadTask&) = delete;
  DownloadTask& operator=(const DownloadTask&) = delete;

  // Begins the download. If a valid, matching resume state file exists next
  // to `destination`, resumes from it (skipping the probe fetch entirely,
  // since a prior successful probe already recorded what's needed);
  // otherwise probes the server for size/Range support before splitting
  // the work. `on_progress` may fire any number of times; `on_done` fires
  // exactly once, whether this call resumed or started fresh.
  void Start(ProgressCallback on_progress, DoneCallback on_done);

  // Stops this task from acting on any further fetch callbacks and leaves
  // whatever resume state was last persisted on disk -- a cancelled
  // download is resumable, not discarded. on_done still fires, with
  // Result::kCancelled. Note this does not abort in-flight network
  // fetches themselves (ChunkFetcher has no cancellation channel yet);
  // any already in progress simply run to completion in the background
  // and are silently dropped when they report back.
  void Cancel();

 private:
  struct Worker {
    Chunk chunk;
    std::unique_ptr<SectorAlignedWriter> writer;
    bool done = false;
    int retry_count = 0;
  };

  // A tiny (1-byte) fetch issued purely to read response metadata --
  // Content-Length, Accept-Ranges, ETag, Last-Modified -- before committing
  // to a split. Its actual byte is discarded rather than written, and the
  // real chunks fetched afterward cover the full [0, total_size) range
  // uncontaminated by it. Skipped entirely when resuming, since a previous
  // run's probe already recorded this in the state file.
  void StartProbe();
  void OnProbeDone(FetchResult result);

  // Common continuation for both the resumed and freshly-probed paths:
  // compute the outstanding chunks, open the disk writer, and launch one
  // worker per chunk.
  void BeginTransfer();
  void LaunchWorker(size_t worker_index);
  void OnWorkerData(size_t worker_index, base::span<const uint8_t> data);
  void OnWorkerDone(size_t worker_index, FetchResult result);

  // Called after a worker completes successfully: records the finished
  // chunk, persists state, and checks whether every worker is now done.
  void HandleWorkerCompleted(size_t worker_index);

  void Finish(Result result);

  std::string url_;
  base::FilePath destination_;
  ChunkFetcher* fetcher_;
  DownloadProfile profile_;

  DownloadState state_;
  DiskWriter disk_writer_;
  std::vector<Worker> workers_;
  // Set once, in Start(), based on whether a matching resume state file
  // was found. Determines whether BeginTransfer() opens the destination
  // file preserving its existing bytes or starts it fresh -- getting this
  // wrong for a resume would silently destroy the very bytes being
  // resumed from.
  bool resuming_ = false;

  bool cancelled_ = false;
  bool finished_ = false;

  ProgressCallback on_progress_;
  DoneCallback on_done_;

  // Bounds how many times a single chunk is retried before the whole
  // download is reported as failed. A chunk that keeps failing past this
  // is almost certainly a real server/network problem, not transient
  // packet loss -- the resume state is left intact either way, so a
  // later manual retry starts from wherever this one got to, not zero.
  static constexpr int kMaxRetriesPerChunk = 3;

  // Must be the last member: guards every callback this class hands to
  // ChunkFetcher, so a DownloadTask destroyed with fetches still in
  // flight never has a callback run against a dangling this.
  base::WeakPtrFactory<DownloadTask> weak_factory_{this};
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TBP_DOWNLOAD_TASK_H_
