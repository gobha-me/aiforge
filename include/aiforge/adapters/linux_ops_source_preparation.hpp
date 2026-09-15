#pragma once
#include <aiforge/runtime/ops_source_preparation.hpp>

namespace aiforge::adapters {
class LinuxOpsSourcePreparationFactory final
    : public runtime::OpsSourcePreparationFactory {
 public:
  // Metadata only: no source/namespace/procfs construction on this caller.
  [[nodiscard]] static auto create(domain::OpsTargetId target,
                                   domain::OpsConfigurationRevision revision)
      -> std::expected<std::shared_ptr<LinuxOpsSourcePreparationFactory>,
                       runtime::OpsObservationSourceError>;
  [[nodiscard]] auto guarantees_owned_read_only_preparation() const noexcept
      -> bool override {
    return true;
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
  explicit LinuxOpsSourcePreparationFactory(
      runtime::OpsSourcePreparationIdentity identity);
  const runtime::OpsSourcePreparationIdentity m_identity;
};
// Existing Linux source construction owns its internal bound. The original work
// deadline is checked around it, not threaded through it: stalled or additional
// physical work may outlast that logical deadline while retaining the slot.
} // namespace aiforge::adapters
