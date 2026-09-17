// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTE_H_
#define COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTE_H_

namespace tbp_download {

// Identifies which network path a connection should use -- the mechanism
// behind "Split Vector VPN Downloading": a download's connections don't
// just differ in which byte range they carry, they can each exit through a
// different VPN egress IP too. kDirect (the default) is the ordinary,
// unproxied path; any other value names one of a pool of embedded
// WireGuard tunnels, each exposed locally as its own SOCKS5 proxy port (see
// issue #1's per-page VPN egress design).
//
// Deliberately just an int, not an opaque handle: the tunnel pool that
// assigns these indices to real WireGuard endpoints lives elsewhere (not
// yet built) and owns the actual mapping from index to SOCKS5 port: this
// type only names "which one," it doesn't know what it connects to.
struct EgressRoute {
  static constexpr int kDirect = -1;

  int tunnel_index = kDirect;

  bool operator==(const EgressRoute& other) const {
    return tunnel_index == other.tunnel_index;
  }
  bool operator<(const EgressRoute& other) const {
    return tunnel_index < other.tunnel_index;
  }
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTE_H_
