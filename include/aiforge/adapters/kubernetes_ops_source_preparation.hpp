#pragma once

#include <aiforge/config/config.hpp>
#include <aiforge/runtime/ops_source_preparation.hpp>

namespace aiforge::adapters {

// Owns the exact configured path and selection metadata. Referenced bytes are
// opened, bounded, parsed and copied into the prepared source only on the
// shared local-source worker.
class KubernetesOpsSourcePreparationFactory final
    : public runtime::OpsSourcePreparationFactory {
 public:
  [[nodiscard]] static auto create(
      domain::OpsTargetId target, domain::OpsConfigurationRevision revision,
      config::StaticKubernetesTargetConfig configuration) noexcept
      -> std::expected<std::shared_ptr<KubernetesOpsSourcePreparationFactory>,
                       runtime::OpsObservationSourceError>;
  [[nodiscard]] auto guarantees_owned_read_only_preparation() const noexcept
      -> bool override {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
  }
  [[nodiscard]] auto preparation_identity() const noexcept
      -> const runtime::OpsSourcePreparationIdentity& override {
    return m_identity;
  }
  [[nodiscard]] auto prepare(
      const runtime::OpsSourcePreparationRequest& request,
      std::stop_token stop = {})
      -> std::expected<runtime::PreparedOpsSource,
                       runtime::OpsObservationSourceError> override;

 private:
  KubernetesOpsSourcePreparationFactory(
      runtime::OpsSourcePreparationIdentity identity,
      config::StaticKubernetesTargetConfig configuration);

  const runtime::OpsSourcePreparationIdentity m_identity;
  const config::StaticKubernetesTargetConfig m_configuration;
};

} // namespace aiforge::adapters
