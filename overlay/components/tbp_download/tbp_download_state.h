// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TBP_DOWNLOAD_STATE_H_
#define COMPONENTS_TBP_DOWNLOAD_TBP_DOWNLOAD_STATE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "components/tbp_download/tbp_chunk_scheduler.h"

namespace tbp_download {

// The durable, on-disk record of one download's progress — the entire
// mechanism "resume from a killed process" rests on. Written after every
// chunk completes (see DownloadState::path_for), read back on startup.
//
// Deliberately does not persist connection count, worker identities, or
// anything about which chunk was assigned to which network request — only
// which byte ranges are durably on disk. On restart, ChunkScheduler::
// RemainingChunks recomputes a fresh work assignment from that alone, so a
// resumed download is never coupled to how many connections happened to be
// open when the process died.
struct DownloadState {
  std::string url;
  int64_t total_size = kUnknownSize;
  bool supports_ranges = false;
  DownloadProfile profile = DownloadProfile::kBalanced;

  // Validators for whether resuming against the same URL is even safe: if
  // the server's ETag or Last-Modified has changed since these were
  // recorded, the remote file is not the one we were downloading and the
  // bytes already on disk cannot be trusted. Empty when the server didn't
  // supply either header, in which case the caller has to make a policy
  // decision (this type only carries the data, it doesn't decide policy).
  std::string etag;
  std::string last_modified;

  // Byte ranges already durably written to disk. Not required to be sorted
  // or merged — the same tolerance ChunkScheduler::RemainingChunks already
  // has for this is relied on here too, so this struct never needs its own
  // normalization pass.
  std::vector<Chunk> completed_chunks;

  bool operator==(const DownloadState& other) const;
};

// The state file lives next to the download's destination file, not in some
// separate registry, so the two can never point at each other incorrectly
// after a move: `path_for("C:\Downloads\a.iso")` is
// `C:\Downloads\a.iso.tbpstate`.
base::FilePath StateFilePathFor(const base::FilePath& destination);

// Serializes to a compact JSON representation. Byte ranges round-trip
// exactly; this is not meant to be a stable cross-version format, only
// stable enough that a browser restart can read what it just wrote.
std::string SerializeDownloadState(const DownloadState& state);

// Parses what SerializeDownloadState produced. Returns nullopt for anything
// malformed — this is treated as "no resumable state," not an error to
// surface to the user, since the safe fallback is simply restarting the
// download from scratch.
std::optional<DownloadState> DeserializeDownloadState(
    const std::string& serialized);

// Writes the state file for `destination` atomically: the new content is
// written to a temporary file in the same directory and then renamed over
// the real path, so a crash mid-write can never leave a truncated, corrupt
// state file that would otherwise be mistaken for valid resume data.
// Returns false on any I/O failure.
bool SaveDownloadState(const base::FilePath& destination,
                        const DownloadState& state);

// Reads and parses the state file for `destination`, if one exists.
std::optional<DownloadState> LoadDownloadState(
    const base::FilePath& destination);

// Deletes the state file for `destination`, if any. Called once a download
// finishes successfully — a completed download has no resume state left to
// keep.
void DeleteDownloadState(const base::FilePath& destination);

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TBP_DOWNLOAD_STATE_H_
