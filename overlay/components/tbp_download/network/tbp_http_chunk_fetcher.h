// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_HTTP_CHUNK_FETCHER_H_
#define COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_HTTP_CHUNK_FETCHER_H_

#include <memory>
#include <vector>

#include "components/tbp_download/network/tbp_egress_route.h"
#include "components/tbp_download/network/tbp_egress_router.h"
#include "components/tbp_download/tbp_chunk_fetcher.h"

namespace tbp_download {

// The real ChunkFetcher, backed by Chromium's network service. Each
// FetchChunk() call gets its own network::SimpleURLLoader issuing a ranged
// GET, routed through whichever EgressRoute is currently set -- this is
// the actual mechanism behind "Split Vector VPN Downloading": DownloadTask
// (or whatever assigns egress routes per worker) can call SetEgressRoute()
// between chunks so different chunk workers exit through different VPN
// tunnels.
//
// Not unit-testable the way DownloadTask's own logic is (see
// FakeChunkFetcher in tbp_download_task_unittest.cc for how that's tested
// instead) -- this class's job is entirely to talk to a real network
// stack, which needs a real NetworkService and UI-thread-created
// NetworkContexts (see EgressRouter). Verified by direct compilation
// against the real Chromium headers and by manual runtime testing, not by
// a unit test.
class HttpChunkFetcher : public ChunkFetcher {
 public:
  // `egress_router` is not owned and must outlive this fetcher.
  explicit HttpChunkFetcher(EgressRouter* egress_router);
  ~HttpChunkFetcher() override;

  HttpChunkFetcher(const HttpChunkFetcher&) = delete;
  HttpChunkFetcher& operator=(const HttpChunkFetcher&) = delete;

  // Which egress route subsequent FetchChunk() calls use. Takes effect
  // immediately for new fetches; fetches already in flight keep whatever
  // route they started with.
  void SetEgressRoute(EgressRoute route);

  // ChunkFetcher:
  void FetchChunk(const std::string& url,
                   const Chunk& chunk,
                   ChunkDataCallback on_data,
                   ChunkDoneCallback on_done) override;

 private:
  class StreamAdapter;
  friend class StreamAdapter;

  void RemoveAdapter(StreamAdapter* adapter);

  EgressRouter* egress_router_;
  EgressRoute route_;
  std::vector<std::unique_ptr<StreamAdapter>> active_fetches_;
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_HTTP_CHUNK_FETCHER_H_
