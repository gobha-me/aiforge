#include <aiforge/domain/automatic_approval.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto valid_digest(const std::string_view value,
                                const std::string_view prefix) noexcept
    -> bool {
  return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
         std::ranges::all_of(value.substr(prefix.size()),
                             [](const unsigned char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'a' && character <= 'f');
                             });
}

} // namespace

auto valid_automatic_approval_evidence(
    const AutomaticApprovalEvidence& evidence) noexcept -> bool {
  constexpr std::string_view policy_prefix{"aiforge.auto-policy.v1.sha256:"};
  constexpr std::array rule_prefixes{
      std::string_view{"aiforge.auto-rule.exact.v1.sha256:"},
      std::string_view{"aiforge.auto-rule.repository.v1.sha256:"},
      std::string_view{"aiforge.auto-rule.process-executable.v1.sha256:"}};
  return valid_digest(evidence.policy_identity, policy_prefix) &&
         std::ranges::any_of(rule_prefixes, [&](const auto prefix) {
           return valid_digest(evidence.rule_identity, prefix);
         });
}

auto automatic_approval_rule_kind(
    const AutomaticApprovalEvidence& evidence) noexcept
    -> std::optional<AutomaticApprovalRuleKind> {
  if (!valid_automatic_approval_evidence(evidence)) return std::nullopt;
  if (evidence.rule_identity.starts_with(
          "aiforge.auto-rule.exact.v1.sha256:")) {
    return AutomaticApprovalRuleKind::exact_arguments;
  }
  if (evidence.rule_identity.starts_with(
          "aiforge.auto-rule.repository.v1.sha256:")) {
    return AutomaticApprovalRuleKind::repository_read_path;
  }
  return AutomaticApprovalRuleKind::process_executable;
}

} // namespace aiforge::domain
