// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/network/tbp_http_chunk_fetcher.h"

#include "base/containers/span.h"
#include "base/strings/stringprintf.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace tbp_download {

namespace {

// TODO(crbug.com/tbp-traffic-annotations): same registration gap noted in
// tbp_egress_router_impl.cc -- functionally complete, not yet run through
// Chromium's separate traffic-annotation audit process.
net::NetworkTrafficAnnotationTag ChunkFetchTrafficAnnotation() {
  return net::DefineNetworkTrafficAnnotation("tbp_download_chunk_fetch", R"(
    semantics {
      sender: "TheBrowserProject in-house download engine"
      description:
        "A single ranged HTTP(S) GET for one chunk of a multi-connection "
        "download."
      trigger: "One per chunk worker DownloadTask launches for a download."
      data: "The download URL and a Range request header."
      destination: OTHER
      destination_other: "Whatever host the download URL points at."
    }
    policy {
      cookies_allowed: NO
      setting: "Controlled by starting or cancelling the download itself."
      policy_exception_justification:
        "Not yet controlled by an enterprise policy."
    }
  )");
}

// Populates everything ParseFetchMetadata can determine from a response:
// total size (from Content-Range on a 206, or Content-Length on a 200),
// whether ranges actually worked (206, or an explicit Accept-Ranges: bytes
// on a 200), and the two cache validators.
FetchMetadata ParseFetchMetadata(const net::HttpResponseHeaders* headers) {
  FetchMetadata metadata;
  if (!headers) {
    return metadata;
  }

  int64_t first_byte = -1, last_byte = -1, instance_length = -1;
  if (headers->response_code() == 206 &&
      headers->GetContentRangeFor206(&first_byte, &last_byte,
                                      &instance_length) &&
      instance_length >= 0) {
    metadata.total_size = instance_length;
    metadata.supports_ranges = true;
  } else {
    std::optional<base::ByteSize> content_length = headers->GetContentLength();
    if (content_length.has_value()) {
      metadata.total_size = static_cast<int64_t>(content_length->InBytes());
    }
    std::optional<std::string> accept_ranges =
        headers->GetNormalizedHeader("Accept-Ranges");
    metadata.supports_ranges =
        accept_ranges.has_value() && *accept_ranges == "bytes";
  }

  std::optional<std::string> etag = headers->GetNormalizedHeader("ETag");
  if (etag.has_value()) {
    metadata.etag = *etag;
  }
  std::optional<std::string> last_modified =
      headers->GetNormalizedHeader("Last-Modified");
  if (last_modified.has_value()) {
    metadata.last_modified = *last_modified;
  }
  return metadata;
}

std::string RangeHeaderValue(const Chunk& chunk) {
  if (chunk.end == kUnknownEnd) {
    return base::StringPrintf("bytes=%lld-",
                               static_cast<long long>(chunk.start));
  }
  return base::StringPrintf("bytes=%lld-%lld",
                             static_cast<long long>(chunk.start),
                             static_cast<long long>(chunk.end));
}

}  // namespace

// Owns one SimpleURLLoader for one chunk's fetch and is its stream
// consumer -- SimpleURLLoaderStreamConsumer callbacks aren't tagged with
// which loader they're for, so each concurrent fetch genuinely needs its
// own consumer instance, not a shared dispatcher.
class HttpChunkFetcher::StreamAdapter
    : public network::SimpleURLLoaderStreamConsumer {
 public:
  StreamAdapter(HttpChunkFetcher* owner,
                ChunkDataCallback on_data,
                ChunkDoneCallback on_done)
      : owner_(owner), on_data_(std::move(on_data)), on_done_(std::move(on_done)) {}

  ~StreamAdapter() override = default;

  void Start(scoped_refptr<network::SharedURLLoaderFactory> factory,
             const std::string& url,
             const Chunk& chunk) {
    factory_ = std::move(factory);

    auto request = std::make_unique<network::ResourceRequest>();
    request->url = GURL(url);
    request->method = net::HttpRequestHeaders::kGetMethod;
    request->headers.SetHeader(net::HttpRequestHeaders::kRange,
                                RangeHeaderValue(chunk));

    loader_ = network::SimpleURLLoader::Create(std::move(request),
                                                ChunkFetchTrafficAnnotation());
    // Without this, a non-2xx/206 status fails the request before headers
    // are even readable -- but a 416 (kRangeNotSatisfiable) and a 404
    // (kServerError) need to be told apart, which requires reading the
    // actual status code below in OnComplete().
    loader_->SetAllowHttpErrorResults(true);
    loader_->DownloadAsStream(factory_.get(), this);
  }

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view string_piece,
                       base::OnceClosure resume) override {
    on_data_.Run(base::as_byte_span(string_piece));
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    FetchResult result;
    const network::mojom::URLResponseHead* response_info =
        loader_->ResponseInfo();
    const net::HttpResponseHeaders* headers =
        response_info ? response_info->headers.get() : nullptr;
    result.metadata = ParseFetchMetadata(headers);

    if (success) {
      result.outcome = FetchOutcome::kSuccess;
    } else {
      int status = headers ? headers->response_code() : 0;
      if (status == 416) {
        result.outcome = FetchOutcome::kRangeNotSatisfiable;
      } else if (status >= 400) {
        result.outcome = FetchOutcome::kServerError;
      } else {
        result.outcome = FetchOutcome::kNetworkError;
      }
    }

    ChunkDoneCallback done = std::move(on_done_);
    HttpChunkFetcher* owner = owner_;
    owner->RemoveAdapter(this);  // Deletes `this` -- touch no members after.
    std::move(done).Run(result);
  }

  void OnRetry(base::OnceClosure start_retry) override {
    // Retries are never enabled on this loader (no SetRetryOptions call),
    // so the network service should never invoke this. DownloadTask
    // already owns chunk-level retry decisions; letting SimpleURLLoader
    // retry underneath it too would double up that policy in a way
    // neither layer is coordinated for.
  }

 private:
  HttpChunkFetcher* owner_;
  ChunkDataCallback on_data_;
  ChunkDoneCallback on_done_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
};

HttpChunkFetcher::HttpChunkFetcher(EgressRouter* egress_router)
    : egress_router_(egress_router) {}

HttpChunkFetcher::~HttpChunkFetcher() = default;

void HttpChunkFetcher::SetEgressRoute(EgressRoute route) {
  route_ = route;
}

void HttpChunkFetcher::FetchChunk(const std::string& url,
                                   const Chunk& chunk,
                                   ChunkDataCallback on_data,
                                   ChunkDoneCallback on_done) {
  scoped_refptr<network::SharedURLLoaderFactory> factory =
      egress_router_->GetURLLoaderFactory(route_);
  if (!factory) {
    // The requested egress route (a VPN tunnel that isn't up) couldn't be
    // satisfied -- fail this fetch rather than silently falling back to a
    // direct connection, which would defeat the point of having asked.
    std::move(on_done).Run(
        FetchResult{FetchOutcome::kNetworkError, FetchMetadata()});
    return;
  }

  auto adapter = std::make_unique<StreamAdapter>(this, std::move(on_data),
                                                  std::move(on_done));
  StreamAdapter* raw = adapter.get();
  active_fetches_.push_back(std::move(adapter));
  raw->Start(std::move(factory), url, chunk);
}

void HttpChunkFetcher::RemoveAdapter(StreamAdapter* adapter) {
  for (auto it = active_fetches_.begin(); it != active_fetches_.end(); ++it) {
    if (it->get() == adapter) {
      active_fetches_.erase(it);
      return;
    }
  }
}

}  // namespace tbp_download
