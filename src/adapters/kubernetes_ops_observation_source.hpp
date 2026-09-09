#pragma once

#include "static_kubernetes_config.hpp"
#include <aiforge/runtime/ops_observation_source.hpp>
#include <array>
#include <memory>

namespace aiforge::adapters {

// Adapter-private immutable custody. Creation performs metadata validation
// only; SSL initialization and all IO belong to observe on the retained worker
// slot.
class KubernetesOpsObservationSource final
    : public runtime::OpsObservationSource {
 public:
  static constexpr std::array supported_operations{
      domain::OpsObservationOperation::kubernetes_workloads,
      domain::OpsObservationOperation::kubernetes_pod_health,
      domain::OpsObservationOperation::kubernetes_events};
  [[nodiscard]] static auto create(
      const domain::OpsTargetBinding& binding,
      StaticKubernetesConfig configuration) noexcept
      -> std::expected<std::shared_ptr<KubernetesOpsObservationSource>,
                       runtime::OpsObservationSourceError>;
  ~KubernetesOpsObservationSource() override;
  [[nodiscard]] auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override;
  [[nodiscard]] auto guarantees_bound_read_only_observations() const noexcept
      -> bool override;
  [[nodiscard]] auto observe(const domain::OpsObservationRequest& request,
                             std::stop_token stop = {})
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override;

 private:
  struct Impl;
  explicit KubernetesOpsObservationSource(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> m_impl;
};
} // namespace aiforge::adapters
