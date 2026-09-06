#pragma once

#include <optional>
#include <string>

namespace aiforge::domain {

struct AutomaticApprovalEvidence {
  std::string policy_identity;
  std::string rule_identity;
  auto operator==(const AutomaticApprovalEvidence&) const -> bool = default;
};

enum class AutomaticApprovalRuleKind {
  exact_arguments,
  repository_read_path,
  process_executable,
};

[[nodiscard]] auto valid_automatic_approval_evidence(
    const AutomaticApprovalEvidence& evidence) noexcept -> bool;
[[nodiscard]] auto automatic_approval_rule_kind(
    const AutomaticApprovalEvidence& evidence) noexcept
    -> std::optional<AutomaticApprovalRuleKind>;

} // namespace aiforge::domain
