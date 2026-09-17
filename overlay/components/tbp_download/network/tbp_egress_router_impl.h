// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTER_IMPL_H_
#define COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTER_IMPL_H_

#include <map>
#include <memory>
#include <optional>

#include "base/functional/callback.h"
#include "base/unguessable_token.h"
#include "components/tbp_download/network/tbp_egress_route.h"
#include "components/tbp_download/network/tbp_egress_router.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/auth.h"
#include "net/cookies/cookie_partition_key.h"
#include "net/http/http_response_headers.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_info.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"
#include "url/gurl.h"

namespace tbp_download {

// Real implementation of EgressRouter, modeled directly on
// chrome/browser/ash/bruschetta/bruschetta_network_context.cc -- Chromium's
// own pattern for a standalone, non-Profile-tied NetworkContext -- extended
// to own a whole pool of them, one per EgressRoute, rather than a single
// fixed one.
//
// Not unit-testable the way the rest of this engine is: creating a real
// network::mojom::NetworkContext requires a live NetworkService and must
// run on the UI thread, neither of which our lightweight
// tbp_download_unittests harness provides. Correctness here can only be
// verified via a real browser_test or manual runtime testing -- see the
// class comment on EgressRouter for the threading contract this exists to
// satisfy.
class EgressRouterImpl : public EgressRouter,
                          public network::mojom::URLLoaderNetworkServiceObserver {
 public:
  // Maps a tunnel index to its local SOCKS5 proxy port (e.g. the embedded
  // WireGuard tunnel pool binds tunnel 0 to 127.0.0.1:1080, tunnel 1 to
  // :1081, ...). Not this class's concern how that pool is populated --
  // it only asks "what port does this index mean right now."
  using PortForTunnel = base::RepeatingCallback<std::optional<uint16_t>(int)>;

  explicit EgressRouterImpl(PortForTunnel port_for_tunnel);
  ~EgressRouterImpl() override;

  EgressRouterImpl(const EgressRouterImpl&) = delete;
  EgressRouterImpl& operator=(const EgressRouterImpl&) = delete;

  // EgressRouter:
  scoped_refptr<network::SharedURLLoaderFactory> GetURLLoaderFactory(
      EgressRoute route) override;

 private:
  struct Entry {
    Entry();
    ~Entry();
    mojo::Remote<network::mojom::NetworkContext> network_context;
    mojo::Remote<network::mojom::URLLoaderFactory> url_loader_factory;
  };

  // Creates `route`'s NetworkContext + URLLoaderFactory if they don't
  // already exist (or the pipes were disconnected), and returns the entry.
  // Returns nullptr if `route` names a tunnel index with no known port --
  // a caller asking to route through a VPN tunnel that isn't (or is no
  // longer) up.
  Entry* GetOrCreateEntry(EgressRoute route);
  std::unique_ptr<Entry> CreateEntry(EgressRoute route);

  // network::mojom::URLLoaderNetworkServiceObserver:
  //
  // This engine has no UI to prompt through and no reason to trust a
  // server whose certificate doesn't validate or that asks for a client
  // certificate we were never given one for -- every one of these is a
  // safe-refusal default, not a placeholder to fill in later.
  void OnSSLCertificateError(const GURL& url,
                              int net_error,
                              const net::SSLInfo& ssl_info,
                              bool fatal,
                              OnSSLCertificateErrorCallback response) override;
  void OnCertificateRequested(
      const std::optional<base::UnguessableToken>& window_id,
      const scoped_refptr<net::SSLCertRequestInfo>& cert_info,
      mojo::PendingRemote<network::mojom::ClientCertificateResponder>
          cert_responder) override;
  void OnAuthRequired(
      const std::optional<base::UnguessableToken>& window_id,
      int32_t request_id,
      const GURL& url,
      bool first_auth_attempt,
      const net::AuthChallengeInfo& auth_info,
      const scoped_refptr<net::HttpResponseHeaders>& head_headers,
      mojo::PendingRemote<network::mojom::AuthChallengeResponder>
          auth_challenge_responder) override;
  void OnLocalNetworkAccessPermissionRequired(
      network::mojom::TransportType type,
      network::mojom::IPAddressSpace ip_address_space,
      OnLocalNetworkAccessPermissionRequiredCallback callback) override;
  void OnPlatformLocalNetworkPermissionRequired(
      OnPlatformLocalNetworkPermissionRequiredCallback callback) override;
  void OnClearSiteData(
      const GURL& url,
      const std::string& header_value,
      int32_t load_flags,
      const std::optional<net::CookiePartitionKey>& cookie_partition_key,
      bool partitioned_state_allowed_only,
      OnClearSiteDataCallback callback) override;
  void OnLoadingStateUpdate(network::mojom::LoadInfoPtr info,
                             OnLoadingStateUpdateCallback callback) override;
  void OnDataUseUpdate(int32_t network_traffic_annotation_id_hash,
                        base::ByteSize recv_bytes,
                        base::ByteSize sent_bytes) override;
  void Clone(
      mojo::PendingReceiver<network::mojom::URLLoaderNetworkServiceObserver>
          listener) override;
  void OnWebSocketConnectedToLocalNetwork(
      const GURL& request_url,
      network::mojom::IPAddressSpace ip_address_space) override;
  void OnUrlLoaderConnectedToLocalNetwork(
      const GURL& request_url,
      network::mojom::IPAddressSpace response_address_space,
      network::mojom::IPAddressSpace client_address_space,
      network::mojom::IPAddressSpace target_address_space) override;

  PortForTunnel port_for_tunnel_;
  std::map<int, std::unique_ptr<Entry>> entries_;
  mojo::ReceiverSet<network::mojom::URLLoaderNetworkServiceObserver>
      observer_receivers_;
};

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_NETWORK_TBP_EGRESS_ROUTER_IMPL_H_
