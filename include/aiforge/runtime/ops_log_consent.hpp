#pragma once

#include <expected>
#include <memory>

#include <aiforge/domain/ops_target.hpp>

namespace aiforge::runtime {

class OpsObservationEndpoint;
struct OpsLogConsentActivation;

struct OpsLogConsentChange {
  domain::SessionId session_id;
  domain::OpsTargetBinding target;
  std::uint64_t selection_generation{};
  std::uint64_t log_policy_revision{};
  domain::OpsResourceIdentity source;
  bool enabled{};
};

// Process-local authority for one exact broker activation. Construction only
// accepts a disabled policy, so replayed observations, restored configuration
// and a reused SessionId cannot recreate live permission. Every effective
// change advances both revisions and immediately invalidates the prior live
// snapshot; the returned authority must be rebound before new work can use it.
class OpsSessionLogConsent final {
 public:
  [[nodiscard]] static auto start(
      const OpsObservationEndpoint& endpoint,
      domain::OpsObservationAuthoritySpec disabled_authority)
      -> std::expected<OpsSessionLogConsent, domain::OpsTargetError>;

  [[nodiscard]] auto authority() const
      -> std::expected<domain::OpsObservationAuthority, domain::OpsTargetError>;
  [[nodiscard]] auto apply(const OpsLogConsentChange& change)
      -> std::expected<domain::OpsObservationAuthority, domain::OpsTargetError>;
  [[nodiscard]] auto replace_selection(
      domain::OpsObservationAuthoritySpec disabled_authority)
      -> std::expected<domain::OpsObservationAuthority, domain::OpsTargetError>;

 private:
  explicit OpsSessionLogConsent(
      std::shared_ptr<OpsLogConsentActivation> activation);
  std::shared_ptr<OpsLogConsentActivation> m_activation;
};

} // namespace aiforge::runtime
