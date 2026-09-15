#pragma once
#include "kubernetes_https_control.hpp"
#include "static_kubernetes_config.hpp"
namespace aiforge::adapters {
[[nodiscard]] auto prepare_kubernetes_tls(httplib::SSLClient& client,
                                          StaticKubernetesTlsView material,
                                          const KubernetesHttpsControl& control)
    -> std::expected<void, runtime::OpsObservationSourceError>;
} // namespace aiforge::adapters
