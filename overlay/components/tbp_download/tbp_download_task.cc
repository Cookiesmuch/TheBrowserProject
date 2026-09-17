// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_download_task.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"

namespace tbp_download {

DownloadTask::DownloadTask(std::string url,
                            base::FilePath destination,
                            ChunkFetcher* fetcher,
                            DownloadProfile profile)
    : url_(std::move(url)),
      destination_(std::move(destination)),
      fetcher_(fetcher),
      profile_(profile) {
  state_.url = url_;
  state_.profile = profile_;
}

DownloadTask::~DownloadTask() = default;

void DownloadTask::Start(ProgressCallback on_progress, DoneCallback on_done) {
  on_progress_ = std::move(on_progress);
  on_done_ = std::move(on_done);

  std::optional<DownloadState> loaded = LoadDownloadState(destination_);
  // Only trust resume state recorded for this exact URL -- a state file
  // left behind for a different download that happened to reuse the same
  // destination path is not ours to resume from.
  if (loaded.has_value() && loaded->url == url_) {
    state_ = *loaded;
    // The caller's requested profile always wins over whatever was in
    // effect on a previous run (e.g. resuming into Background mode after
    // game-mode throttling kicked in).
    state_.profile = profile_;
    resuming_ = true;
    BeginTransfer();
    return;
  }

  StartProbe();
}

void DownloadTask::Cancel() {
  if (finished_ || cancelled_) {
    return;
  }
  cancelled_ = true;
  // Deliberately does not touch disk_writer_ or the persisted state file:
  // whatever was last saved remains valid resume data for a future run.
  // Any fetches already in flight are left to run to completion in the
  // background and are discarded by the cancelled_ checks in the
  // callbacks below -- ChunkFetcher does not currently expose a way to
  // abort an in-progress fetch outright.
  Finish(Result::kCancelled);
}

void DownloadTask::StartProbe() {
  fetcher_->FetchChunk(url_, Chunk{0, 0}, base::DoNothing(),
                        base::BindOnce(&DownloadTask::OnProbeDone,
                                        weak_factory_.GetWeakPtr()));
}

void DownloadTask::OnProbeDone(FetchResult result) {
  if (cancelled_ || finished_) {
    return;
  }
  if (result.outcome != FetchOutcome::kSuccess) {
    Finish(Result::kFailed);
    return;
  }

  state_.total_size = result.metadata.total_size;
  state_.supports_ranges = result.metadata.supports_ranges;
  state_.etag = result.metadata.etag;
  state_.last_modified = result.metadata.last_modified;
  state_.completed_chunks.clear();

  BeginTransfer();
}

void DownloadTask::BeginTransfer() {
  SchedulerConfig config = ChunkScheduler::ConfigForProfile(profile_);
  // ConfigForProfile's sector_size is only a placeholder default -- every
  // chunk boundary it produces must match the *real* destination volume's
  // sector size, or DiskWriter::WriteAt will reject writes at boundaries
  // that looked aligned against the wrong assumption. Get it from the
  // actual volume before computing anything that depends on it.
  int64_t real_sector_size = DiskWriter::QuerySectorSize(destination_);
  if (real_sector_size > 0) {
    config.sector_size = real_sector_size;
  }

  std::vector<Chunk> remaining = ChunkScheduler::RemainingChunks(
      state_.total_size, state_.completed_chunks, config);

  // total_size is only kUnknownSize here if the probe never learned a
  // Content-Length -- in which case RemainingChunks always returns exactly
  // one chunk (never empty), so total_size is guaranteed known by the time
  // `remaining` is actually empty.
  if (remaining.empty()) {
    if (!disk_writer_.Open(destination_, state_.total_size,
                            /*preserve_existing_content=*/resuming_) ||
        !disk_writer_.Finish(state_.total_size)) {
      Finish(Result::kFailed);
      return;
    }
    DeleteDownloadState(destination_);
    Finish(Result::kSuccess);
    return;
  }

  // DiskWriter::kUnknownTotalSize and kUnknownSize are both -1; passed
  // through directly rather than translated, since they are the same
  // sentinel by construction. preserve_existing_content=resuming_ is what
  // stops a resumed download's already-written bytes from being truncated
  // away by a fresh CREATE_ALWAYS -- see DiskWriter::Open's own comment.
  if (!disk_writer_.Open(destination_, state_.total_size,
                          /*preserve_existing_content=*/resuming_)) {
    Finish(Result::kFailed);
    return;
  }

  workers_.clear();
  workers_.resize(remaining.size());
  for (size_t i = 0; i < remaining.size(); ++i) {
    workers_[i].chunk = remaining[i];
  }
  for (size_t i = 0; i < workers_.size(); ++i) {
    LaunchWorker(i);
  }
}

void DownloadTask::LaunchWorker(size_t worker_index) {
  Worker& worker = workers_[worker_index];
  bool is_final = worker.chunk.end == kUnknownEnd ||
                   worker.chunk.end == state_.total_size - 1;
  worker.writer = std::make_unique<SectorAlignedWriter>(
      &disk_writer_, worker.chunk.start, disk_writer_.sector_size(),
      is_final);
  worker.done = false;

  fetcher_->FetchChunk(
      url_, worker.chunk,
      base::BindRepeating(&DownloadTask::OnWorkerData,
                           weak_factory_.GetWeakPtr(), worker_index),
      base::BindOnce(&DownloadTask::OnWorkerDone,
                      weak_factory_.GetWeakPtr(), worker_index));
}

void DownloadTask::OnWorkerData(size_t worker_index,
                                 base::span<const uint8_t> data) {
  if (cancelled_ || finished_) {
    return;
  }
  Worker& worker = workers_[worker_index];
  // A write failure here is surfaced when the fetch itself concludes (see
  // OnWorkerDone): further data for an already-broken writer is simply
  // dropped rather than retried mid-stream.
  if (!worker.writer->Append(data)) {
    return;
  }

  int64_t bytes_written = 0;
  for (const Worker& w : workers_) {
    bytes_written += w.writer ? (w.writer->next_offset() - w.chunk.start) : 0;
  }
  if (on_progress_) {
    on_progress_.Run(Progress{bytes_written, state_.total_size});
  }
}

void DownloadTask::OnWorkerDone(size_t worker_index, FetchResult result) {
  if (cancelled_ || finished_) {
    return;
  }
  Worker& worker = workers_[worker_index];
  bool write_ok = worker.writer->Finish();

  if (result.outcome == FetchOutcome::kSuccess && write_ok) {
    HandleWorkerCompleted(worker_index);
    return;
  }

  if (result.outcome == FetchOutcome::kRangeNotSatisfiable) {
    // The remote resource's size or Range support changed since the probe
    // recorded it -- retrying this exact byte range cannot succeed. Left
    // as an outright failure (with resume state intact) rather than
    // silently re-probing and re-planning; that is a reasonable future
    // refinement, not required for this chunk to fail correctly now.
    Finish(Result::kFailed);
    return;
  }

  worker.retry_count++;
  if (worker.retry_count > kMaxRetriesPerChunk) {
    Finish(Result::kFailed);
    return;
  }
  // Retry the whole chunk from its original start. Any bytes this attempt
  // already wrote for it are overwritten by the fresh SectorAlignedWriter
  // constructed in LaunchWorker -- consistent with resume only ever
  // persisting whole completed chunks, never partial progress.
  LaunchWorker(worker_index);
}

void DownloadTask::HandleWorkerCompleted(size_t worker_index) {
  Worker& worker = workers_[worker_index];
  worker.done = true;
  state_.completed_chunks.push_back(worker.chunk);
  SaveDownloadState(destination_, state_);

  bool all_done =
      std::all_of(workers_.begin(), workers_.end(),
                  [](const Worker& w) { return w.done; });
  if (!all_done) {
    return;
  }

  int64_t final_size = state_.total_size != kUnknownSize
                            ? state_.total_size
                            : workers_[worker_index].writer->next_offset();
  if (!disk_writer_.Finish(final_size)) {
    Finish(Result::kFailed);
    return;
  }
  DeleteDownloadState(destination_);
  Finish(Result::kSuccess);
}

void DownloadTask::Finish(Result result) {
  if (finished_) {
    return;
  }
  finished_ = true;
  if (on_done_) {
    std::move(on_done_).Run(result);
  }
}

}  // namespace tbp_download
