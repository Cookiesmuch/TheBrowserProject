// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/network/tbp_egress_router_impl.h"

#include "base/strings/stringprintf.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/network_service_instance.h"
#include "net/base/net_errors.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace tbp_download {

namespace {

// TODO(crbug.com/tbp-traffic-annotations): register a real entry in
// tools/traffic_annotation's audited annotation list rather than a bare
// inline definition -- this is functionally correct today (every field
// Chromium's traffic-annotation auditor requires is filled in below) but
// hasn't gone through that separate registration/audit process yet.
net::NetworkTrafficAnnotationTag EgressTrafficAnnotation() {
  return net::DefineNetworkTrafficAnnotation("tbp_download_egress_context", R"(
    semantics {
      sender: "TheBrowserProject in-house download engine"
      description:
        "HTTP(S) requests issued by the in-house multi-connection download "
        "engine, optionally routed through a local SOCKS5 proxy in front of "
        "an embedded WireGuard tunnel when the user has enabled per-download "
        "VPN egress (Split Vector VPN Downloading)."
      trigger:
        "The user starts a download; each chunk worker issues its own "
        "ranged request through this context."
      data: "Whatever the download's own URL and headers require."
      destination: OTHER
      destination_other: "Whatever host the download URL points at."
    }
    policy {
      cookies_allowed: NO
      setting:
        "Users control this indirectly by choosing whether to enable VPN "
        "egress for a given download; there is no separate on/off switch "
        "for the request path itself."
      policy_exception_justification:
        "Not yet controlled by an enterprise policy."
    }
  )");
}

}  // namespace

EgressRouterImpl::Entry::Entry() = default;
EgressRouterImpl::Entry::~Entry() = default;

EgressRouterImpl::EgressRouterImpl(PortForTunnel port_for_tunnel)
    : port_for_tunnel_(std::move(port_for_tunnel)) {}

EgressRouterImpl::~EgressRouterImpl() = default;

scoped_refptr<network::SharedURLLoaderFactory>
EgressRouterImpl::GetURLLoaderFactory(EgressRoute route) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  Entry* entry = GetOrCreateEntry(route);
  if (!entry) {
    return nullptr;
  }

  mojo::PendingRemote<network::mojom::URLLoaderFactory> pending;
  entry->url_loader_factory->Clone(
      pending.InitWithNewPipeAndPassReceiver());
  return base::MakeRefCounted<
      network::WrapperSharedURLLoaderFactory>(std::move(pending));
}

EgressRouterImpl::Entry* EgressRouterImpl::GetOrCreateEntry(
    EgressRoute route) {
  auto it = entries_.find(route.tunnel_index);
  if (it != entries_.end() && it->second->network_context.is_connected() &&
      it->second->url_loader_factory.is_connected()) {
    return it->second.get();
  }

  std::unique_ptr<Entry> entry = CreateEntry(route);
  if (!entry) {
    entries_.erase(route.tunnel_index);
    return nullptr;
  }
  Entry* raw = entry.get();
  entries_[route.tunnel_index] = std::move(entry);
  return raw;
}

std::unique_ptr<EgressRouterImpl::Entry> EgressRouterImpl::CreateEntry(
    EgressRoute route) {
  std::optional<uint16_t> socks_port;
  if (route.tunnel_index != EgressRoute::kDirect) {
    socks_port = port_for_tunnel_.Run(route.tunnel_index);
    if (!socks_port.has_value()) {
      // The caller asked to route through a VPN tunnel that isn't (or is
      // no longer) up -- refuse rather than silently falling back to a
      // direct connection, which would defeat the whole point of asking
      // for VPN egress in the first place.
      return nullptr;
    }
  }

  auto entry = std::make_unique<Entry>();

  network::mojom::NetworkContextParamsPtr params =
      SystemNetworkContextManager::GetInstance()
          ->CreateDefaultNetworkContextParams();
  // Each egress context is small and short-lived relative to the profile's
  // main one; an on-disk HTTP cache shared across unrelated downloads (or
  // worse, across different VPN egress identities) buys nothing and is a
  // needless correlation vector between them.
  params->http_cache_enabled = false;

  if (socks_port.has_value()) {
    net::ProxyConfig proxy_config;
    proxy_config.proxy_rules().ParseFromString(
        base::StringPrintf("socks5://127.0.0.1:%u", *socks_port));
    params->initial_proxy_config = net::ProxyConfigWithAnnotation(
        proxy_config, EgressTrafficAnnotation());
  } else {
    params->initial_proxy_config = net::ProxyConfigWithAnnotation::CreateDirect();
  }

  content::CreateNetworkContextInNetworkService(
      entry->network_context.BindNewPipeAndPassReceiver(),
      std::move(params));

  network::mojom::URLLoaderFactoryParamsPtr factory_params =
      network::mojom::URLLoaderFactoryParams::New();
  factory_params->process_id = network::OriginatingProcessId::browser();
  factory_params->is_orb_enabled = false;
  factory_params->is_trusted = true;
  observer_receivers_.Add(
      this, factory_params->url_loader_network_observer
                .InitWithNewPipeAndPassReceiver());

  entry->network_context->CreateURLLoaderFactory(
      entry->url_loader_factory.BindNewPipeAndPassReceiver(),
      std::move(factory_params));

  return entry;
}

void EgressRouterImpl::OnSSLCertificateError(
    const GURL& url,
    int net_error,
    const net::SSLInfo& ssl_info,
    bool fatal,
    OnSSLCertificateErrorCallback response) {
  // No UI to prompt through, and no reason this engine should ever accept
  // a certificate the platform's own verifier rejected.
  std::move(response).Run(net::ERR_INSECURE_RESPONSE);
}

void EgressRouterImpl::OnCertificateRequested(
    const std::optional<base::UnguessableToken>& window_id,
    const scoped_refptr<net::SSLCertRequestInfo>& cert_info,
    mojo::PendingRemote<network::mojom::ClientCertificateResponder>
        cert_responder) {
  // This engine has no client certificate store of its own to offer one
  // from -- continuing without a certificate is the correct behavior for a
  // server that merely prefers one, and a correct failure for a server
  // that requires one (the connection fails downstream, surfaced to the
  // download as an ordinary network error).
  mojo::Remote<network::mojom::ClientCertificateResponder> responder(
      std::move(cert_responder));
  responder->ContinueWithoutCertificate();
}

void EgressRouterImpl::OnAuthRequired(
    const std::optional<base::UnguessableToken>& window_id,
    int32_t request_id,
    const GURL& url,
    bool first_auth_attempt,
    const net::AuthChallengeInfo& auth_info,
    const scoped_refptr<net::HttpResponseHeaders>& head_headers,
    mojo::PendingRemote<network::mojom::AuthChallengeResponder>
        auth_challenge_responder) {
  // No UI to prompt for credentials through.
  mojo::Remote<network::mojom::AuthChallengeResponder> responder(
      std::move(auth_challenge_responder));
  responder->OnAuthCredentials(std::nullopt);
}

void EgressRouterImpl::OnLocalNetworkAccessPermissionRequired(
    network::mojom::TransportType type,
    network::mojom::IPAddressSpace ip_address_space,
    OnLocalNetworkAccessPermissionRequiredCallback callback) {
  std::move(callback).Run(network::mojom::LocalNetworkAccessResult::kDenied);
}

void EgressRouterImpl::OnPlatformLocalNetworkPermissionRequired(
    OnPlatformLocalNetworkPermissionRequiredCallback callback) {
  std::move(callback).Run(/*granted=*/false);
}

void EgressRouterImpl::OnClearSiteData(
    const GURL& url,
    const std::string& header_value,
    int32_t load_flags,
    const std::optional<net::CookiePartitionKey>& cookie_partition_key,
    bool partitioned_state_allowed_only,
    OnClearSiteDataCallback callback) {
  std::move(callback).Run();
}

void EgressRouterImpl::OnLoadingStateUpdate(
    network::mojom::LoadInfoPtr info,
    OnLoadingStateUpdateCallback callback) {
  std::move(callback).Run();
}

void EgressRouterImpl::OnDataUseUpdate(int32_t network_traffic_annotation_id_hash,
                                        base::ByteSize recv_bytes,
                                        base::ByteSize sent_bytes) {}

void EgressRouterImpl::Clone(
    mojo::PendingReceiver<network::mojom::URLLoaderNetworkServiceObserver>
        listener) {
  observer_receivers_.Add(this, std::move(listener));
}

void EgressRouterImpl::OnWebSocketConnectedToLocalNetwork(
    const GURL& request_url,
    network::mojom::IPAddressSpace ip_address_space) {}

void EgressRouterImpl::OnUrlLoaderConnectedToLocalNetwork(
    const GURL& request_url,
    network::mojom::IPAddressSpace response_address_space,
    network::mojom::IPAddressSpace client_address_space,
    network::mojom::IPAddressSpace target_address_space) {}

}  // namespace tbp_download
