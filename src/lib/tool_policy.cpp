#include <aiforge/runtime/tool_policy.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto error(const ToolPolicyErrorCode code, std::string message,
                         const bool retryable = false) -> ToolPolicyError {
  return {code, std::move(message), retryable};
}

[[nodiscard]] auto contains_control(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

template <typename Value>
[[nodiscard]] auto unique(const std::vector<Value>& values) -> bool {
  for (auto current = values.begin(); current != values.end(); ++current) {
    if (std::find(std::next(current), values.end(), *current) != values.end()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto effect_allowed_for_kind(const domain::Effect effect,
                                           const std::string_view kind)
    -> bool {
  if (kind == "filesystem.root") {
    return effect == domain::Effect::read || effect == domain::Effect::write ||
           effect == domain::Effect::remove;
  }
  if (kind == "network.host") {
    return effect == domain::Effect::network ||
           effect == domain::Effect::communicate;
  }
  if (kind == "network.unrestricted") {
    return effect == domain::Effect::network;
  }
  if (kind == "process.command") return effect == domain::Effect::execute;
  if (kind == "cluster.resource") {
    return effect == domain::Effect::read || effect == domain::Effect::write ||
           effect == domain::Effect::remove ||
           effect == domain::Effect::change_infrastructure ||
           effect == domain::Effect::change_privileges;
  }
  if (kind == "artifact.id") {
    return effect == domain::Effect::read || effect == domain::Effect::write ||
           effect == domain::Effect::remove;
  }
  if (kind == "spend.microunits") return effect == domain::Effect::spend;
  return false;
}

[[nodiscard]] auto normalize_path(std::string value)
    -> std::expected<std::string, ToolPolicyError> {
  if (value.empty() || value.size() > 4096 || contains_control(value)) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "filesystem capability path is invalid"));
  }
  const std::filesystem::path path{value};
  if (!path.is_absolute()) {
    return std::unexpected(
        error(ToolPolicyErrorCode::invalid_request,
              "filesystem capability path must be absolute"));
  }
  const auto normalized = path.lexically_normal().generic_string();
  if (normalized.empty() || normalized != value ||
      normalized.find("/../") != std::string::npos ||
      normalized.ends_with("/..")) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "filesystem capability path is ambiguous"));
  }
  return normalized;
}

[[nodiscard]] auto normalize_host(std::string value)
    -> std::expected<std::string, ToolPolicyError> {
  if (value.empty() || value.size() > 253 || contains_control(value) ||
      value.contains('/') || value.contains('@') || value.contains('*') ||
      value.front() == '.' || value.back() == '.') {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "network capability host is invalid"));
  }
  std::ranges::transform(value, value.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (std::ranges::any_of(value, [](const unsigned char character) {
        return !(std::isalnum(character) || character == '.' ||
                 character == '-' || character == ':' || character == '[' ||
                 character == ']');
      })) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "network capability host is invalid"));
  }
  return value;
}

[[nodiscard]] auto normalize_exact(std::string value,
                                   const std::string_view label)
    -> std::expected<std::string, ToolPolicyError> {
  if (value.empty() || value.size() > 4096 || contains_control(value)) {
    return std::unexpected(
        error(ToolPolicyErrorCode::invalid_request,
              std::string{label} + " capability is invalid"));
  }
  return value;
}

[[nodiscard]] auto parse_microunits(const std::string_view value)
    -> std::optional<std::uint64_t> {
  if (value.empty() || value.front() == '+' || value.front() == '-') {
    return std::nullopt;
  }
  std::uint64_t result{};
  const auto [end, status] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (status != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] auto normalize_scopes(
    const std::vector<domain::CapabilityScope>& scopes)
    -> std::expected<std::vector<domain::CapabilityScope>, ToolPolicyError> {
  std::vector<domain::CapabilityScope> result;
  result.reserve(scopes.size());
  for (const auto& scope : scopes) {
    auto normalized = normalize_capability_scope(scope);
    if (!normalized) return std::unexpected(std::move(normalized.error()));
    result.push_back(std::move(*normalized));
  }
  if (!unique(result)) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "capability scopes must be unique"));
  }
  return result;
}

[[nodiscard]] auto effects_cover(const std::vector<domain::Effect>& grants,
                                 const std::vector<domain::Effect>& requested)
    -> bool {
  return std::ranges::all_of(requested, [&](const auto effect) {
    return std::ranges::find(grants, effect) != grants.end();
  });
}

[[nodiscard]] auto scopes_cover(
    const std::vector<domain::CapabilityScope>& grants,
    const std::vector<domain::CapabilityScope>& requested) -> bool {
  return std::ranges::all_of(requested, [&](const auto& scope) {
    return std::ranges::any_of(grants, [&](const auto& grant) {
      return capability_scope_covers(grant, scope);
    });
  });
}

[[nodiscard]] auto valid_request(const ToolPolicyRequest& request)
    -> std::expected<ToolPolicyRequest, ToolPolicyError> {
  if (request.tool_name.empty() || request.tool_name.size() > 128 ||
      contains_control(request.tool_name) || !unique(request.effects)) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "tool policy request is malformed"));
  }
  auto scopes = normalize_scopes(request.scopes);
  if (!scopes) return std::unexpected(std::move(scopes.error()));
  if (std::ranges::any_of(*scopes, [&](const auto& scope) {
        return std::ranges::find(request.effects, scope.effect) ==
               request.effects.end();
      })) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "capability scope has an undeclared effect"));
  }
  auto result = request;
  result.scopes = std::move(*scopes);
  return result;
}

[[nodiscard]] auto grant_covers(const storage::SavedPolicyGrant& grant,
                                const ToolPolicyRequest& request) -> bool {
  return grant.permission_profile_id == request.permission_profile_id &&
         grant.tool_name == request.tool_name &&
         effects_cover(grant.effects, request.effects) &&
         scopes_cover(grant.scopes, request.scopes);
}

[[nodiscard]] auto valid_saved_grant(
    const storage::SavedPolicyGrant& grant,
    const domain::PermissionProfileId& permission_profile_id) -> bool {
  if (grant.permission_profile_id != permission_profile_id ||
      grant.tool_name.empty() || grant.tool_name.size() > 128 ||
      contains_control(grant.tool_name) || !unique(grant.effects)) {
    return false;
  }
  auto scopes = normalize_scopes(grant.scopes);
  return scopes && std::ranges::all_of(*scopes, [&](const auto& scope) {
           return std::ranges::find(grant.effects, scope.effect) !=
                  grant.effects.end();
         });
}

[[nodiscard]] auto valid_lifetime(const domain::ApprovalGrantLifetime lifetime)
    -> bool {
  switch (lifetime) {
    case domain::ApprovalGrantLifetime::invocation:
    case domain::ApprovalGrantLifetime::session:
    case domain::ApprovalGrantLifetime::saved: return true;
  }
  return false;
}

class DefaultPolicy final : public ToolPolicy {
 public:
  auto evaluate(const ToolPolicyRequest& request)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> override {
    try {
      auto validated = valid_request(request);
      if (!validated) return std::unexpected(std::move(validated.error()));
      if (validated->effects.empty() && validated->scopes.empty()) {
        return ToolPolicyResolution{
            domain::PolicyDecision::allow,
            {},
            "the invocation declares no authority-bearing effects",
            domain::PolicyDecisionSource::fallback};
      }
      return ToolPolicyResolution{
          domain::PolicyDecision::deny,
          {},
          "no policy is configured for an effectful invocation",
          domain::PolicyDecisionSource::fallback};
    } catch (...) {
      return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                   "fallback policy failed internally"));
    }
  }

  auto approve(const ToolPolicyRequest&, ToolPolicyApproval)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> override {
    return std::unexpected(error(ToolPolicyErrorCode::scope_widening,
                                 "the fallback policy grants no authority"));
  }
};

} // namespace

namespace {

[[nodiscard]] auto normalize_capability_scope_impl(
    domain::CapabilityScope scope)
    -> std::expected<domain::CapabilityScope, ToolPolicyError> {
  if (scope.kind == "root") scope.kind = "filesystem.root";
  if (!effect_allowed_for_kind(scope.effect, scope.kind)) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "capability kind does not match its effect"));
  }
  std::expected<std::string, ToolPolicyError> normalized =
      std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                            "capability normalization failed"));
  if (scope.kind == "filesystem.root" || scope.kind == "process.command") {
    normalized = normalize_path(std::move(scope.value));
  } else if (scope.kind == "network.host") {
    normalized = normalize_host(std::move(scope.value));
  } else if (scope.kind == "network.unrestricted") {
    if (scope.value != "new-sockets") {
      return std::unexpected(
          error(ToolPolicyErrorCode::invalid_request,
                "unrestricted network capability is invalid"));
    }
    normalized = std::move(scope.value);
  } else if (scope.kind == "spend.microunits") {
    if (!parse_microunits(scope.value)) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                   "spend capability is invalid"));
    }
    normalized = std::move(scope.value);
  } else if (scope.kind == "artifact.id") {
    if (!domain::ArtifactId::from(scope.value)) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                   "artifact capability is invalid"));
    }
    normalized = std::move(scope.value);
  } else {
    normalized = normalize_exact(std::move(scope.value), scope.kind);
  }
  if (!normalized) return std::unexpected(std::move(normalized.error()));
  scope.value = std::move(*normalized);
  return scope;
}

} // namespace

auto normalize_capability_scope(domain::CapabilityScope scope)
    -> std::expected<domain::CapabilityScope, ToolPolicyError> {
  try {
    return normalize_capability_scope_impl(std::move(scope));
  } catch (...) {
    return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                 "capability normalization failed internally"));
  }
}

auto capability_scope_covers(const domain::CapabilityScope& grant,
                             const domain::CapabilityScope& requested) -> bool {
  try {
    auto normalized_grant = normalize_capability_scope(grant);
    auto normalized_request = normalize_capability_scope(requested);
    if (!normalized_grant || !normalized_request ||
        normalized_grant->effect != normalized_request->effect ||
        normalized_grant->kind != normalized_request->kind) {
      return false;
    }
    if (normalized_grant->kind == "filesystem.root") {
      const std::filesystem::path parent{normalized_grant->value};
      const std::filesystem::path child{normalized_request->value};
      auto parent_part = parent.begin();
      auto child_part = child.begin();
      for (; parent_part != parent.end() && child_part != child.end();
           ++parent_part, ++child_part) {
        if (*parent_part != *child_part) return false;
      }
      return parent_part == parent.end();
    }
    if (normalized_grant->kind == "spend.microunits") {
      return *parse_microunits(normalized_grant->value) >=
             *parse_microunits(normalized_request->value);
    }
    return normalized_grant->value == normalized_request->value;
  } catch (...) {
    return false;
  }
}

auto intersect_capability_scopes(
    const std::vector<domain::CapabilityScope>& parent,
    const std::vector<domain::CapabilityScope>& requested)
    -> std::expected<std::vector<domain::CapabilityScope>, ToolPolicyError> {
  try {
    auto normalized_parent = normalize_scopes(parent);
    if (!normalized_parent) {
      return std::unexpected(std::move(normalized_parent.error()));
    }
    auto normalized_requested = normalize_scopes(requested);
    if (!normalized_requested) {
      return std::unexpected(std::move(normalized_requested.error()));
    }
    if (!scopes_cover(*normalized_parent, *normalized_requested)) {
      return std::unexpected(
          error(ToolPolicyErrorCode::scope_widening,
                "capability intersection would widen authority"));
    }
    return normalized_requested;
  } catch (...) {
    return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                 "capability intersection failed internally"));
  }
}

CapabilityPolicy::CapabilityPolicy(PermissionProfile profile,
                                   storage::PolicyGrantStore* saved_grants)
    : m_profile(std::move(profile)), m_saved_grants(saved_grants) {
}

auto CapabilityPolicy::evaluate(const ToolPolicyRequest& request)
    -> std::expected<ToolPolicyResolution, ToolPolicyError> {
  try {
    auto validated = valid_request(request);
    if (!validated) return std::unexpected(std::move(validated.error()));
    if (validated->permission_profile_id != m_profile.permission_profile_id ||
        !unique(m_profile.automatic_effects) ||
        !unique(m_profile.approvable_effects)) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                   "permission profile is invalid"));
    }
    auto automatic = normalize_scopes(m_profile.automatic_scopes);
    auto ceiling = normalize_scopes(m_profile.approval_ceiling);
    if (!automatic || !ceiling ||
        std::ranges::any_of(*automatic,
                            [&](const auto& scope) {
                              return std::ranges::find(
                                         m_profile.automatic_effects,
                                         scope.effect) ==
                                     m_profile.automatic_effects.end();
                            }) ||
        std::ranges::any_of(*ceiling, [&](const auto& scope) {
          return std::ranges::find(m_profile.approvable_effects,
                                   scope.effect) ==
                 m_profile.approvable_effects.end();
        })) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                   "permission profile scopes are invalid"));
    }
    if (effects_cover(m_profile.automatic_effects, validated->effects) &&
        scopes_cover(*automatic, validated->scopes)) {
      return ToolPolicyResolution{
          domain::PolicyDecision::allow, validated->scopes,
          "allowed by the active permission profile",
          domain::PolicyDecisionSource::permission_profile};
    }

    std::vector<storage::SavedPolicyGrant> session_grants;
    for (const auto& grant : m_session_grants) {
      if (grant.session_id == validated->session_id) {
        session_grants.push_back(grant.grant);
      }
    }
    if (std::ranges::any_of(session_grants, [&](const auto& grant) {
          return grant_covers(grant, *validated);
        })) {
      return ToolPolicyResolution{domain::PolicyDecision::allow,
                                  validated->scopes,
                                  "allowed by a bounded session grant",
                                  domain::PolicyDecisionSource::session_grant};
    }

    if (m_saved_grants != nullptr) {
      auto saved = m_saved_grants->load_grants(m_profile.permission_profile_id);
      if (!saved) {
        return std::unexpected(error(ToolPolicyErrorCode::persistence_failure,
                                     "saved policy grants are unavailable",
                                     saved.error().retryable));
      }
      if (std::ranges::any_of(*saved, [&](const auto& grant) {
            return !valid_saved_grant(grant, m_profile.permission_profile_id);
          })) {
        return std::unexpected(
            error(ToolPolicyErrorCode::persistence_failure,
                  "saved policy grants contain invalid capability data"));
      }
      if (std::ranges::any_of(*saved, [&](const auto& grant) {
            return grant_covers(grant, *validated) &&
                   effects_cover(m_profile.approvable_effects, grant.effects) &&
                   scopes_cover(*ceiling, grant.scopes);
          })) {
        return ToolPolicyResolution{domain::PolicyDecision::allow,
                                    validated->scopes,
                                    "allowed by an explicit saved grant",
                                    domain::PolicyDecisionSource::saved_grant};
      }
    }

    if (effects_cover(m_profile.approvable_effects, validated->effects) &&
        scopes_cover(*ceiling, validated->scopes)) {
      return ToolPolicyResolution{
          domain::PolicyDecision::require_approval, validated->scopes,
          "the invocation requires explicit user approval",
          domain::PolicyDecisionSource::permission_profile};
    }
    return ToolPolicyResolution{
        domain::PolicyDecision::deny,
        {},
        "the active permission profile denies this invocation",
        domain::PolicyDecisionSource::permission_profile};
  } catch (...) {
    return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                 "tool policy evaluation failed internally"));
  }
}

auto CapabilityPolicy::approve(const ToolPolicyRequest& request,
                               ToolPolicyApproval approval)
    -> std::expected<ToolPolicyResolution, ToolPolicyError> {
  try {
    if (!valid_lifetime(approval.lifetime)) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                   "approval lifetime is invalid"));
    }
    auto validated = valid_request(request);
    if (!validated) return std::unexpected(std::move(validated.error()));
    if (validated->permission_profile_id != m_profile.permission_profile_id) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                   "permission profile is invalid"));
    }
    auto granted = normalize_scopes(approval.granted_scopes);
    auto ceiling = normalize_scopes(m_profile.approval_ceiling);
    if (!granted || !ceiling) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                   "approval scopes are invalid"));
    }
    if (!effects_cover(m_profile.approvable_effects, validated->effects) ||
        !scopes_cover(*ceiling, *granted) ||
        !scopes_cover(*granted, validated->scopes) ||
        !scopes_cover(validated->scopes, *granted)) {
      return std::unexpected(
          error(ToolPolicyErrorCode::scope_widening,
                "approval cannot widen or omit required authority"));
    }

    storage::SavedPolicyGrant grant{validated->permission_profile_id,
                                    validated->tool_name, validated->effects,
                                    *granted};
    if (approval.lifetime == domain::ApprovalGrantLifetime::session) {
      m_session_grants.push_back({validated->session_id, grant});
    } else if (approval.lifetime == domain::ApprovalGrantLifetime::saved) {
      if (m_saved_grants == nullptr) {
        return std::unexpected(error(ToolPolicyErrorCode::persistence_failure,
                                     "saved policy storage is unavailable"));
      }
      auto saved = m_saved_grants->save_grant(grant);
      if (!saved) {
        return std::unexpected(
            error(ToolPolicyErrorCode::persistence_failure,
                  "saved policy grant could not be persisted",
                  saved.error().retryable));
      }
    }
    return ToolPolicyResolution{domain::PolicyDecision::allow, *granted,
                                "allowed by explicit user approval",
                                domain::PolicyDecisionSource::user_approval};
  } catch (...) {
    return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                 "tool policy approval failed internally"));
  }
}

auto default_tool_policy() -> std::shared_ptr<ToolPolicy> {
  return std::make_shared<DefaultPolicy>();
}

} // namespace aiforge::runtime
