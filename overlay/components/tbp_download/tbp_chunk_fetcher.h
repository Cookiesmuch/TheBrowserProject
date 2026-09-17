// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TBP_CHUNK_FETCHER_H_
#define COMPONENTS_TBP_DOWNLOAD_TBP_CHUNK_FETCHER_H_

#include <stdint.h>

#include <string>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "components/tbp_download/tbp_chunk_scheduler.h"

namespace tbp_download {

enum class FetchOutcome {
  kSuccess,
  kNetworkError,
  // Non-2xx/206 status the caller should treat as fatal for this chunk
  // rather than retryable (e.g. 404).
  kServerError,
  // The server rejected our Range assumption (416) -- happens when a
  // resumed download's remote file changed size since the state was last
  // saved. The caller must not retry this chunk as-is; the whole download
  // needs to be re-validated against the server's current metadata.
  kRangeNotSatisfiable,
};

// Metadata that is only meaningful on a download's very first chunk fetch
// (before we know anything about the remote resource) but is delivered on
// every fetch for uniformity; later fetches repeat the same values.
struct FetchMetadata {
  int64_t total_size = kUnknownSize;
  bool supports_ranges = false;
  std::string etag;
  std::string last_modified;
};

struct FetchResult {
  FetchOutcome outcome = FetchOutcome::kNetworkError;
  FetchMetadata metadata;
};

// Called zero or more times as bytes arrive for one chunk, strictly in
// order and contiguous starting at the chunk's own start offset -- the
// receiver is not expected to handle out-of-order or overlapping spans.
using ChunkDataCallback =
    base::RepeatingCallback<void(base::span<const uint8_t> data)>;

// Called exactly once when a chunk's fetch concludes, successfully or not.
using ChunkDoneCallback = base::OnceCallback<void(FetchResult result)>;

// Abstracts "fetch these bytes over the network" away from the download
// orchestrator, so the orchestrator's scheduling/retry/persistence logic can
// be unit tested against a fake without a live network stack. The real
// implementation is backed by Chromium's network service.
class ChunkFetcher {
 public:
  virtual ~ChunkFetcher() = default;

  // Begins fetching `chunk` of `url`. `on_data` may be called any number of
  // times (including zero, for an empty chunk) before `on_done` fires
  // exactly once. Implementations must support multiple concurrent calls
  // for different chunks of the same download.
  virtual void FetchChunk(const std::string& url,
                           const Chunk& chunk,
                           ChunkDataCallback on_data,
                           ChunkDoneCallback on_done) = 0;
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TBP_CHUNK_FETCHER_H_
