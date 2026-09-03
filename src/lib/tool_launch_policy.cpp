#include <aiforge/runtime/tool_launch_policy.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {

constexpr std::size_t kMaximumAutomaticTools{256};

[[nodiscard]] auto error(const ToolPolicyErrorCode code, std::string message)
    -> ToolPolicyError {
  return {code, std::move(message), false};
}

[[nodiscard]] auto valid_restriction_level(
    const RestrictionLevel level) noexcept -> bool {
  switch (level) {
    case RestrictionLevel::high:
    case RestrictionLevel::medium:
    case RestrictionLevel::low:
    case RestrictionLevel::none: return true;
  }
  return false;
}

[[nodiscard]] auto valid_approval_mode(const ApprovalMode mode) noexcept
    -> bool {
  switch (mode) {
    case ApprovalMode::prompt:
    case ApprovalMode::automatic:
    case ApprovalMode::allow_all: return true;
  }
  return false;
}

[[nodiscard]] auto valid_effect(const domain::Effect effect) noexcept -> bool {
  switch (effect) {
    case domain::Effect::read:
    case domain::Effect::write:
    case domain::Effect::remove:
    case domain::Effect::execute:
    case domain::Effect::network:
    case domain::Effect::communicate:
    case domain::Effect::spend:
    case domain::Effect::change_infrastructure:
    case domain::Effect::change_privileges: return true;
  }
  return false;
}

[[nodiscard]] auto effect_allowed(const RestrictionLevel level,
                                  const domain::Effect effect) noexcept
    -> bool {
  if (!valid_effect(effect)) return false;
  switch (level) {
    case RestrictionLevel::high: return false;
    case RestrictionLevel::medium: return effect == domain::Effect::read;
    case RestrictionLevel::low:
      return effect != domain::Effect::change_infrastructure &&
             effect != domain::Effect::change_privileges;
    case RestrictionLevel::none: return true;
  }
  return false;
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

[[nodiscard]] auto normalize_scopes(
    const std::vector<domain::CapabilityScope>& scopes)
    -> std::expected<std::vector<domain::CapabilityScope>, ToolPolicyError> {
  std::vector<domain::CapabilityScope> normalized;
  normalized.reserve(scopes.size());
  for (const auto& scope : scopes) {
    auto current = normalize_capability_scope(scope);
    if (!current) return std::unexpected(std::move(current.error()));
    normalized.push_back(std::move(*current));
  }
  if (!unique(normalized)) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                 "capability scopes must be unique"));
  }
  return normalized;
}

[[nodiscard]] auto scopes_cover(
    const std::vector<domain::CapabilityScope>& ceiling,
    const std::vector<domain::CapabilityScope>& requested) -> bool {
  return std::ranges::all_of(requested, [&](const auto& request) {
    return std::ranges::any_of(ceiling, [&](const auto& grant) {
      return capability_scope_covers(grant, request);
    });
  });
}

[[nodiscard]] auto same_scopes(
    const std::vector<domain::CapabilityScope>& left,
    const std::vector<domain::CapabilityScope>& right) -> bool {
  return left.size() == right.size() && scopes_cover(left, right) &&
         scopes_cover(right, left);
}

[[nodiscard]] auto provenance_restriction(const RestrictionLevel level)
    -> domain::ToolRestrictionLevel {
  switch (level) {
    case RestrictionLevel::high: return domain::ToolRestrictionLevel::high;
    case RestrictionLevel::medium: return domain::ToolRestrictionLevel::medium;
    case RestrictionLevel::low: return domain::ToolRestrictionLevel::low;
    case RestrictionLevel::none: return domain::ToolRestrictionLevel::none;
  }
  return domain::ToolRestrictionLevel::high;
}

[[nodiscard]] auto provenance_approval(const ApprovalMode mode)
    -> domain::ToolApprovalMode {
  switch (mode) {
    case ApprovalMode::prompt: return domain::ToolApprovalMode::prompt;
    case ApprovalMode::automatic: return domain::ToolApprovalMode::automatic;
    case ApprovalMode::allow_all: return domain::ToolApprovalMode::allow_all;
  }
  return domain::ToolApprovalMode::prompt;
}

[[nodiscard]] auto make_provenance(
    const ToolRegistrySnapshot& registered_tools,
    const ToolLaunchPolicyConfiguration& configuration,
    const std::set<std::string, std::less<>>& automatic_tools)
    -> std::expected<domain::ToolPolicyProvenance, ToolPolicyError> {
  domain::ToolPolicyProvenance provenance{
      "aiforge.tool-launch-policy.v1",
      configuration.permission_profile_id,
      provenance_restriction(configuration.restriction_level),
      provenance_approval(configuration.approval_mode),
      {},
      {},
      {}};
  for (const auto& declaration : registered_tools.declarations()) {
    for (const auto effect : declaration.effects) {
      if (effect_allowed(configuration.restriction_level, effect) &&
          std::ranges::find(provenance.effect_ceiling, effect) ==
              provenance.effect_ceiling.end()) {
        provenance.effect_ceiling.push_back(effect);
      }
    }
    for (const auto& scope : declaration.capability_scopes) {
      if (!effect_allowed(configuration.restriction_level, scope.effect)) {
        continue;
      }
      auto normalized = normalize_capability_scope(scope);
      if (!normalized) return std::unexpected(std::move(normalized.error()));
      if (std::ranges::find(provenance.capability_ceiling, *normalized) ==
          provenance.capability_ceiling.end()) {
        provenance.capability_ceiling.push_back(std::move(*normalized));
      }
    }
  }
  provenance.automatically_eligible_tools.assign(automatic_tools.begin(),
                                                 automatic_tools.end());
  if (auto valid = domain::validate_tool_policy_provenance(provenance);
      !valid) {
    return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                 "tool launch policy provenance is unbounded"));
  }
  return provenance;
}

struct CheckedRequest {
  ToolPolicyRequest request;
  bool within_ceiling{};
};

class LaunchPolicy final : public ToolPolicy {
 public:
  LaunchPolicy(ToolRegistrySnapshot registered_tools,
               ToolLaunchPolicyConfiguration configuration,
               domain::ToolPolicyProvenance provenance)
      : m_registered_tools(std::move(registered_tools)),
        m_configuration(std::move(configuration)),
        m_provenance(std::move(provenance)) {}

  [[nodiscard]] auto evaluate(const ToolPolicyRequest& request)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> override {
    try {
      auto checked = check_request(request);
      if (!checked) return std::unexpected(std::move(checked.error()));
      if (!checked->within_ceiling) return denied();
      if (checked->request.effects.empty()) {
        return ToolPolicyResolution{
            domain::PolicyDecision::allow,
            {},
            "the invocation declares no authority-bearing effects",
            domain::PolicyDecisionSource::permission_profile};
      }
      switch (m_configuration.approval_mode) {
        case ApprovalMode::prompt:
          return ToolPolicyResolution{
              domain::PolicyDecision::require_approval,
              std::move(checked->request.scopes),
              "the invocation requires explicit user approval",
              domain::PolicyDecisionSource::permission_profile};
        case ApprovalMode::automatic:
          if (!m_automatic_tools.contains(checked->request.tool_name)) {
            return denied();
          }
          break;
        case ApprovalMode::allow_all: break;
      }
      return ToolPolicyResolution{
          domain::PolicyDecision::allow, std::move(checked->request.scopes),
          "allowed by the active launch policy",
          domain::PolicyDecisionSource::permission_profile};
    } catch (...) {
      return std::unexpected(
          error(ToolPolicyErrorCode::internal_failure,
                "launch policy evaluation failed internally"));
    }
  }

  [[nodiscard]] auto approve(const ToolPolicyRequest& request,
                             ToolPolicyApproval approval)
      -> std::expected<ToolPolicyResolution, ToolPolicyError> override {
    try {
      if (m_configuration.approval_mode != ApprovalMode::prompt) {
        return std::unexpected(
            error(ToolPolicyErrorCode::invalid_request,
                  "the active launch policy does not accept approvals"));
      }
      if (approval.lifetime != domain::ApprovalGrantLifetime::invocation) {
        return std::unexpected(
            error(ToolPolicyErrorCode::scope_widening,
                  "launch approval is limited to the current invocation"));
      }
      auto checked = check_request(request);
      if (!checked) return std::unexpected(std::move(checked.error()));
      if (!checked->within_ceiling || checked->request.effects.empty()) {
        return std::unexpected(error(
            ToolPolicyErrorCode::scope_widening,
            "launch approval cannot exceed the active authority ceiling"));
      }
      auto granted = normalize_scopes(approval.granted_scopes);
      if (!granted || !same_scopes(*granted, checked->request.scopes)) {
        return std::unexpected(error(
            ToolPolicyErrorCode::scope_widening,
            "launch approval must match the requested capability scopes"));
      }
      return ToolPolicyResolution{domain::PolicyDecision::allow,
                                  std::move(*granted),
                                  "allowed by explicit user approval",
                                  domain::PolicyDecisionSource::user_approval};
    } catch (...) {
      return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                   "launch policy approval failed internally"));
    }
  }

  void set_automatic_tools(std::set<std::string, std::less<>> tools) {
    m_automatic_tools = std::move(tools);
  }

  [[nodiscard]] auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* override {
    return &m_provenance;
  }

 private:
  [[nodiscard]] auto check_request(const ToolPolicyRequest& request) const
      -> std::expected<CheckedRequest, ToolPolicyError> {
    if (request.permission_profile_id !=
        m_configuration.permission_profile_id) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                   "permission profile is invalid"));
    }
    if (request.tool_name.empty() || !unique(request.effects)) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                   "tool launch policy request is malformed"));
    }
    if (std::ranges::any_of(request.effects, [](const auto effect) {
          return !valid_effect(effect);
        })) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_request,
                                   "tool launch policy request is malformed"));
    }
    auto scopes = normalize_scopes(request.scopes);
    if (!scopes) return std::unexpected(std::move(scopes.error()));
    if (std::ranges::any_of(*scopes, [&](const auto& scope) {
          return std::ranges::find(request.effects, scope.effect) ==
                 request.effects.end();
        })) {
      return std::unexpected(
          error(ToolPolicyErrorCode::invalid_request,
                "capability scope has an undeclared requested effect"));
    }
    if (std::ranges::any_of(request.effects, [&](const auto effect) {
          return std::ranges::none_of(*scopes, [effect](const auto& scope) {
            return scope.effect == effect;
          });
        })) {
      return std::unexpected(error(
          ToolPolicyErrorCode::invalid_request,
          "every requested effect requires an explicit capability scope"));
    }

    auto normalized = request;
    normalized.scopes = std::move(*scopes);
    const auto* registered = m_registered_tools.find(request.tool_name);
    if (registered == nullptr)
      return CheckedRequest{std::move(normalized), false};

    auto declared_scopes =
        normalize_scopes(registered->declaration.capability_scopes);
    if (!declared_scopes) {
      return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                   "registered launch policy data is invalid"));
    }
    const auto effects_within_declaration =
        std::ranges::all_of(normalized.effects, [&](const auto effect) {
          return std::ranges::find(registered->declaration.effects, effect) !=
                     registered->declaration.effects.end() &&
                 effect_allowed(m_configuration.restriction_level, effect);
        });
    const auto within_ceiling =
        effects_within_declaration &&
        scopes_cover(*declared_scopes, normalized.scopes);
    return CheckedRequest{std::move(normalized), within_ceiling};
  }

  [[nodiscard]] static auto denied() -> ToolPolicyResolution {
    return {domain::PolicyDecision::deny,
            {},
            "the active launch policy denies this invocation",
            domain::PolicyDecisionSource::permission_profile};
  }

  ToolRegistrySnapshot m_registered_tools;
  ToolLaunchPolicyConfiguration m_configuration;
  domain::ToolPolicyProvenance m_provenance;
  std::set<std::string, std::less<>> m_automatic_tools;
};

} // namespace

auto make_tool_launch_policy(const ToolRegistrySnapshot& registered_tools,
                             ToolLaunchPolicyConfiguration configuration)
    -> std::expected<std::shared_ptr<ToolPolicy>, ToolPolicyError> {
  try {
    if (!valid_restriction_level(configuration.restriction_level) ||
        !valid_approval_mode(configuration.approval_mode)) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                   "tool launch policy mode is invalid"));
    }
    if (configuration.automatically_eligible_tools.size() >
        kMaximumAutomaticTools) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                   "automatic tool allowlist is too large"));
    }
    if (configuration.approval_mode != ApprovalMode::automatic &&
        !configuration.automatically_eligible_tools.empty()) {
      return std::unexpected(
          error(ToolPolicyErrorCode::invalid_profile,
                "automatic tools require automatic approval mode"));
    }

    std::set<std::string, std::less<>> automatic_tools;
    for (const auto& tool_name : configuration.automatically_eligible_tools) {
      if (registered_tools.find(tool_name) == nullptr ||
          !automatic_tools.insert(tool_name).second) {
        return std::unexpected(
            error(ToolPolicyErrorCode::invalid_profile,
                  "automatic tools must be unique registered tool names"));
      }
    }

    auto provenance =
        make_provenance(registered_tools, configuration, automatic_tools);
    if (!provenance) return std::unexpected(std::move(provenance.error()));
    auto policy = std::make_shared<LaunchPolicy>(
        registered_tools, std::move(configuration), std::move(*provenance));
    policy->set_automatic_tools(std::move(automatic_tools));
    return std::shared_ptr<ToolPolicy>{std::move(policy)};
  } catch (...) {
    return std::unexpected(
        error(ToolPolicyErrorCode::internal_failure,
              "tool launch policy creation failed internally"));
  }
}

} // namespace aiforge::runtime
