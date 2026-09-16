#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include <aiforge/domain/ops_target.hpp>

namespace aiforge::runtime {

struct OpsLogConsentActivation {
  explicit OpsLogConsentActivation(domain::SessionId value)
      : session_id(std::move(value)) {}
  std::mutex mutex;
  domain::SessionId session_id;
  bool active{true};
  std::optional<domain::OpsObservationAuthoritySpec> current;
};

[[nodiscard]] auto ops_log_consent_matches(
    const std::shared_ptr<OpsLogConsentActivation>& activation,
    const domain::OpsObservationAuthoritySpec& authority) -> bool;
auto revoke_ops_log_consent(
    const std::shared_ptr<OpsLogConsentActivation>& activation) -> void;
auto revoke_stale_ops_log_consent(
    const std::shared_ptr<OpsLogConsentActivation>& activation,
    const domain::OpsObservationAuthoritySpec& selected) -> void;

} // namespace aiforge::runtime
