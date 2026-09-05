#pragma once

#include <string>

namespace aiforge::domain {

struct AutomaticApprovalEvidence {
  std::string policy_identity;
  std::string rule_identity;
  auto operator==(const AutomaticApprovalEvidence&) const -> bool = default;
};

[[nodiscard]] auto valid_automatic_approval_evidence(
    const AutomaticApprovalEvidence& evidence) noexcept -> bool;

} // namespace aiforge::domain
