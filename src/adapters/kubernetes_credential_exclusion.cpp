#include "kubernetes_credential_exclusion.hpp"
#include <algorithm>

namespace aiforge::adapters {
KubernetesCredentialExclusion::KubernetesCredentialExclusion(
    StaticKubernetesTlsView material, std::stop_token stop) noexcept
    : m_material(material), m_stop(std::move(stop)) {
}
auto KubernetesCredentialExclusion::contains(
    std::string_view value) const noexcept -> bool {
  if (m_stop.stop_requested()) return true;
  if (value.find("-----BEGIN ") != std::string_view::npos ||
      value.find("-----END ") != std::string_view::npos)
    return true;
  if (const auto* token =
          std::get_if<StaticKubernetesTokenView>(&m_material.authentication))
    return !token->token.empty() &&
           value.find(token->token) != std::string_view::npos;
  auto key =
      std::get<StaticKubernetesClientCertificateView>(m_material.authentication)
          .private_key_pem;
  while (!key.empty()) {
    if (m_stop.stop_requested()) return true;
    const auto end = key.find('\n');
    auto line = key.substr(0, end);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.size() >= 16 && !line.starts_with("-----") &&
        value.find(line) != std::string_view::npos)
      return true;
    key = end == std::string_view::npos ? std::string_view{}
                                        : key.substr(end + 1);
  }
  return false;
}
auto KubernetesCredentialExclusion::contains(
    const domain::OpsTargetBinding& value) const noexcept -> bool {
  const auto* identity =
      std::get_if<domain::KubernetesOpsIdentity>(&value.identity);
  return identity == nullptr || contains(value.target_id.value()) ||
         contains(value.configuration_revision.value()) ||
         contains(identity->context_name) ||
         contains(identity->namespace_name) ||
         contains(identity->endpoint.host) ||
         contains(identity->trust_identity);
}
auto KubernetesCredentialExclusion::contains(
    const domain::KubernetesPodIdentity& value) const noexcept -> bool {
  return contains(value.namespace_name) || contains(value.name) ||
         contains(value.uid.value()) ||
         (value.container && (contains(value.container->name) ||
                              contains(value.container->runtime_identity)));
}
auto KubernetesCredentialExclusion::contains(
    const domain::KubernetesObservedResource& value) const noexcept -> bool {
  return contains(value.namespace_name) || contains(value.name) ||
         contains(value.uid.value());
}
auto KubernetesCredentialExclusion::contains(
    const domain::OpsObservationRequest& value) const noexcept -> bool {
  const auto* pod = std::get_if<domain::KubernetesPodIdentity>(&value.resource);
  return contains(value.owner_id.value()) ||
         contains(value.session_id.value()) ||
         contains(value.request_id.value()) || contains(value.target) ||
         (pod != nullptr && contains(*pod));
}
auto KubernetesCredentialExclusion::contains(
    const domain::OpsObservation& value) const noexcept -> bool {
  if (contains(value.request) ||
      (value.source_version && contains(*value.source_version)))
    return true;
  if (const auto* list =
          std::get_if<domain::KubernetesWorkloadsObservation>(&value.payload))
    return std::ranges::any_of(list->workloads, [this](const auto& row) {
      return contains(row.identity);
    });
  if (const auto* pod =
          std::get_if<domain::KubernetesPodObservation>(&value.payload))
    return contains(pod->identity) ||
           std::ranges::any_of(pod->containers, [this](const auto& row) {
             return contains(row.name) ||
                    (row.runtime_identity && contains(*row.runtime_identity));
           });
  if (const auto* events =
          std::get_if<domain::KubernetesEventsObservation>(&value.payload))
    return std::ranges::any_of(events->events, [this](const auto& row) {
      return contains(row.event_uid.value()) || contains(row.regarding);
    });
  return true;
}
} // namespace aiforge::adapters
