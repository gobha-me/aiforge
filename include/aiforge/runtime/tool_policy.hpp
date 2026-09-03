#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/storage/policy_grant_store.hpp>

namespace aiforge::runtime {

enum class ToolPolicyErrorCode {
  invalid_profile,
  invalid_request,
  scope_widening,
  persistence_failure,
  internal_failure,
};

struct ToolPolicyError {
  ToolPolicyErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const ToolPolicyError&) const -> bool = default;
};

struct PermissionProfile {
  domain::PermissionProfileId permission_profile_id;
  std::vector<domain::Effect> automatic_effects;
  std::vector<domain::CapabilityScope> automatic_scopes;
  std::vector<domain::Effect> approvable_effects;
  std::vector<domain::CapabilityScope> approval_ceiling;
  auto operator==(const PermissionProfile&) const -> bool = default;
};

struct ToolPolicyRequest {
  domain::SessionId session_id;
  domain::RunId run_id;
  domain::InvocationId invocation_id;
  domain::PermissionProfileId permission_profile_id;
  std::string tool_name;
  std::vector<domain::Effect> effects;
  std::vector<domain::CapabilityScope> scopes;
  auto operator==(const ToolPolicyRequest&) const -> bool = default;
};

struct ToolPolicyResolution {
  domain::PolicyDecision decision{domain::PolicyDecision::deny};
  std::vector<domain::CapabilityScope> scopes;
  std::optional<std::string> redacted_reason;
  domain::PolicyDecisionSource source{domain::PolicyDecisionSource::fallback};
  auto operator==(const ToolPolicyResolution&) const -> bool = default;
};

struct ToolPolicyApproval {
  std::vector<domain::CapabilityScope> granted_scopes;
  domain::ApprovalGrantLifetime lifetime{
      domain::ApprovalGrantLifetime::invocation};
  auto operator==(const ToolPolicyApproval&) const -> bool = default;
};

class ToolPolicy {
 public:
  virtual ~ToolPolicy() = default;

  [[nodiscard]] virtual auto evaluate(const ToolPolicyRequest& request)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> = 0;

  [[nodiscard]] virtual auto approve(const ToolPolicyRequest& request,
                                     ToolPolicyApproval approval)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> = 0;

  // A null result means this policy cannot support durable recovery of
  // nonterminal authority-bearing work. Implementations that do support it
  // expose a stable, bounded neutral description owned by the policy and must
  // evaluate identical requests deterministically for that description.
  [[nodiscard]] virtual auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* {
    return nullptr;
  }
};

class CapabilityPolicy final : public ToolPolicy {
 public:
  explicit CapabilityPolicy(PermissionProfile profile,
                            storage::PolicyGrantStore* saved_grants = nullptr);

  [[nodiscard]] auto evaluate(const ToolPolicyRequest& request)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> override;
  [[nodiscard]] auto approve(const ToolPolicyRequest& request,
                             ToolPolicyApproval approval)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> override;

 private:
  struct SessionGrant {
    domain::SessionId session_id;
    storage::SavedPolicyGrant grant;
  };

  PermissionProfile m_profile;
  storage::PolicyGrantStore* m_saved_grants{};
  std::vector<SessionGrant> m_session_grants;
};

[[nodiscard]] auto normalize_capability_scope(domain::CapabilityScope scope)
    -> std::expected<domain::CapabilityScope, ToolPolicyError>;
[[nodiscard]] auto capability_scope_covers(
    const domain::CapabilityScope& grant,
    const domain::CapabilityScope& requested) -> bool;
[[nodiscard]] auto intersect_capability_scopes(
    const std::vector<domain::CapabilityScope>& parent,
    const std::vector<domain::CapabilityScope>& requested)
    -> std::expected<std::vector<domain::CapabilityScope>, ToolPolicyError>;

[[nodiscard]] auto default_tool_policy() -> std::shared_ptr<ToolPolicy>;

} // namespace aiforge::runtime
