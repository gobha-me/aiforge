#pragma once
#include "static_kubernetes_config.hpp"
#include <aiforge/domain/ops_observation.hpp>

namespace aiforge::adapters {
// Borrowed only within the immutable configuration owner's lifetime. Known
// material exclusion is not universal secret detection or fragment redaction.
class KubernetesCredentialExclusion final {
 public:
  explicit KubernetesCredentialExclusion(StaticKubernetesTlsView material,
                                         std::stop_token stop = {}) noexcept;
  [[nodiscard]] auto contains(std::string_view value) const noexcept -> bool;
  [[nodiscard]] auto contains(
      const domain::OpsTargetBinding& value) const noexcept -> bool;
  [[nodiscard]] auto contains(
      const domain::OpsObservationRequest& value) const noexcept -> bool;
  [[nodiscard]] auto contains(
      const domain::OpsObservation& value) const noexcept -> bool;

 private:
  [[nodiscard]] auto contains(
      const domain::KubernetesPodIdentity& value) const noexcept -> bool;
  [[nodiscard]] auto contains(
      const domain::KubernetesObservedResource& value) const noexcept -> bool;
  StaticKubernetesTlsView m_material;
  std::stop_token m_stop;
};
} // namespace aiforge::adapters
