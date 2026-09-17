// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTER_H_
#define COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTER_H_

#include "base/memory/scoped_refptr.h"
#include "components/tbp_download/network/tbp_egress_route.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace tbp_download {

// Owns one network::mojom::NetworkContext per distinct EgressRoute,
// creating them lazily, so HTTP chunk workers -- and, later, torrent peer
// connections -- can each be assigned a different VPN egress IP without
// sharing proxy configuration. This is what makes "Split Vector VPN
// Downloading" possible: a download's connections differ not just in byte
// range, but potentially in egress IP too.
//
// Only HTTP is exposed today (GetURLLoaderFactory). Torrent peer
// connections are raw TCP, not HTTP, and will need their own accessor
// (something like GetNetworkContext(), returning a cross-thread-safe
// handle for network::mojom::NetworkContext::CreateTCPConnectedSocket) once
// the peer-wire networking layer that would actually call it exists. Not
// added speculatively ahead of that -- see issue #2's torrent architecture
// notes for the intended shape.
class EgressRouter {
 public:
  virtual ~EgressRouter() = default;

  // Must be called on the UI thread: creating a network::mojom::NetworkContext
  // is a UI-thread-only operation (see
  // content::CreateNetworkContextInNetworkService). The returned factory,
  // however, is a network::SharedURLLoaderFactory -- safe to Clone() onto and
  // use from any other sequence afterward, which is how a download's own
  // sequence actually issues requests through it.
  //
  // `route`'s NetworkContext is created on first use for that route and
  // reused for every subsequent call with an equal route.
  virtual scoped_refptr<network::SharedURLLoaderFactory> GetURLLoaderFactory(
      EgressRoute route) = 0;
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTER_H_
