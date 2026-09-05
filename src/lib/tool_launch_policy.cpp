#include <aiforge/runtime/tool_launch_policy.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <ranges>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto error(const ToolPolicyErrorCode code, std::string message)
    -> ToolPolicyError {
  return {code, std::move(message), false};
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

[[nodiscard]] auto process_registration_matches(
    const RegisteredTool& tool, const ApplicationLaunchContext& context)
    -> bool {
  if (tool.category != ToolCategory::process) return true;
  const auto network_effects =
      std::ranges::count(tool.declaration.effects, domain::Effect::network);
  std::vector<domain::CapabilityScope> network_scopes;
  std::ranges::copy_if(tool.declaration.capability_scopes,
                       std::back_inserter(network_scopes),
                       [](const auto& scope) {
                         return scope.effect == domain::Effect::network;
                       });
  const auto declares_unrestricted_network =
      network_effects == 1 && network_scopes.size() == 1 &&
      network_scopes.front() == domain::CapabilityScope{domain::Effect::network,
                                                        "network.unrestricted",
                                                        "new-sockets"};
  const auto declares_denied_network =
      network_effects == 0 && network_scopes.empty();
  if (!context.process_restriction_available()) {
    return declares_unrestricted_network || declares_denied_network;
  }

  const auto contract = context.process_network_contract().value_or(
      ProcessNetworkContract::deny_new_sockets);
  switch (contract) {
    case ProcessNetworkContract::unrestricted_new_sockets:
      return declares_unrestricted_network;
    case ProcessNetworkContract::deny_new_sockets:
      return declares_denied_network;
  }
  return false;
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

[[nodiscard]] auto provenance_unavailable_reason(
    const RestrictionUnavailableReason reason)
    -> domain::ToolRestrictionUnavailableReason {
  using DomainReason = domain::ToolRestrictionUnavailableReason;
  switch (reason) {
    case RestrictionUnavailableReason::unsupported_platform:
      return DomainReason::unsupported_platform;
    case RestrictionUnavailableReason::unsupported_architecture:
      return DomainReason::unsupported_architecture;
    case RestrictionUnavailableReason::unsupported_kernel:
      return DomainReason::unsupported_kernel;
    case RestrictionUnavailableReason::missing_delegation:
      return DomainReason::missing_delegation;
    case RestrictionUnavailableReason::missing_controller:
      return DomainReason::missing_controller;
    case RestrictionUnavailableReason::permission_denied:
      return DomainReason::permission_denied;
    case RestrictionUnavailableReason::privilege_changed:
      return DomainReason::privilege_changed;
    case RestrictionUnavailableReason::mechanism_absent:
      return DomainReason::mechanism_absent;
    case RestrictionUnavailableReason::unsupported_combination:
      return DomainReason::unsupported_combination;
    case RestrictionUnavailableReason::setup_race:
      return DomainReason::setup_race;
    case RestrictionUnavailableReason::enforcement_failed:
      return DomainReason::enforcement_failed;
    case RestrictionUnavailableReason::cleanup_failed:
      return DomainReason::cleanup_failed;
    case RestrictionUnavailableReason::internal_error:
      return DomainReason::internal_error;
  }
  return DomainReason::internal_error;
}

[[nodiscard]] auto make_provenance(
    const ToolRegistrySnapshot& registered_tools,
    const ToolLaunchPolicyConfiguration& configuration)
    -> std::expected<domain::ToolPolicyProvenance, ToolPolicyError> {
  domain::ToolPolicyProvenance provenance{
      "aiforge.tool-launch-policy.v2",
      configuration.permission_profile_id,
      provenance_restriction(
          configuration.launch_context.selected_restriction()),
      provenance_approval(configuration.launch_context.approval_mode()),
      {},
      {},
      {}};
  if (configuration.launch_context.achieved_restriction()) {
    provenance.achieved_restriction_level = provenance_restriction(
        *configuration.launch_context.achieved_restriction());
  }
  if (configuration.launch_context.unavailable_reason()) {
    provenance.restriction_unavailable_reason = provenance_unavailable_reason(
        *configuration.launch_context.unavailable_reason());
  }
  provenance.mechanism_identity =
      configuration.launch_context.mechanism().identity;
  provenance.mechanism_version =
      configuration.launch_context.mechanism().version;
  provenance.restriction_policy_identity =
      configuration.launch_context.restriction_policy_identity();
  provenance.matcher_policy_identity =
      configuration.launch_context.matcher_policy_identity();
  for (const auto& declaration : registered_tools.declarations()) {
    for (const auto effect : declaration.effects) {
      if (std::ranges::find(provenance.effect_ceiling, effect) ==
          provenance.effect_ceiling.end()) {
        provenance.effect_ceiling.push_back(effect);
      }
    }
    for (const auto& scope : declaration.capability_scopes) {
      auto normalized = normalize_capability_scope(scope);
      if (!normalized) return std::unexpected(std::move(normalized.error()));
      if (std::ranges::find(provenance.capability_ceiling, *normalized) ==
          provenance.capability_ceiling.end()) {
        provenance.capability_ceiling.push_back(std::move(*normalized));
      }
    }
  }
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
      switch (m_configuration.launch_context.approval_mode()) {
        case ApprovalMode::prompt:
          return ToolPolicyResolution{
              domain::PolicyDecision::require_approval,
              std::move(checked->request.scopes),
              "the invocation requires explicit user approval",
              domain::PolicyDecisionSource::permission_profile};
        case ApprovalMode::automatic:
          if (!checked->request.canonical_arguments ||
              !checked->request.selected_restriction ||
              !m_configuration.automatic_matcher) {
            return denied();
          }
          {
            auto matched = m_configuration.automatic_matcher->match(
                {checked->request.session_id, checked->request.run_id,
                 checked->request.invocation_id, checked->request.tool_name,
                 *checked->request.canonical_arguments,
                 *checked->request.selected_restriction,
                 checked->request.effects, checked->request.scopes});
            if (!matched || !matched->has_value()) return denied();
            return ToolPolicyResolution{
                domain::PolicyDecision::allow,
                std::move(checked->request.scopes),
                "allowed by a bounded automatic approval rule",
                domain::PolicyDecisionSource::automatic_matcher,
                std::move(*matched)};
          }
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
      if (m_configuration.launch_context.approval_mode() !=
          ApprovalMode::prompt) {
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

  [[nodiscard]] auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* override {
    return &m_provenance;
  }

  [[nodiscard]] auto selected_restriction() const noexcept
      -> std::optional<RestrictionLevel> override {
    return m_configuration.launch_context.selected_restriction();
  }

 private:
  [[nodiscard]] auto check_request(const ToolPolicyRequest& request) const
      -> std::expected<CheckedRequest, ToolPolicyError> {
    if (request.permission_profile_id !=
        m_configuration.permission_profile_id) {
      return std::unexpected(error(ToolPolicyErrorCode::invalid_profile,
                                   "permission profile is invalid"));
    }
    if (request.selected_restriction &&
        *request.selected_restriction !=
            m_configuration.launch_context.selected_restriction()) {
      return CheckedRequest{request, false};
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
    if (registered->category == ToolCategory::process &&
        !m_configuration.launch_context.process_restriction_available()) {
      return CheckedRequest{std::move(normalized), false};
    }

    auto declared_scopes =
        normalize_scopes(registered->declaration.capability_scopes);
    if (!declared_scopes) {
      return std::unexpected(error(ToolPolicyErrorCode::internal_failure,
                                   "registered launch policy data is invalid"));
    }
    const auto effects_within_declaration =
        std::ranges::all_of(normalized.effects, [&](const auto effect) {
          return std::ranges::find(registered->declaration.effects, effect) !=
                 registered->declaration.effects.end();
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
};

} // namespace

auto make_tool_launch_policy(const ToolRegistrySnapshot& registered_tools,
                             ToolLaunchPolicyConfiguration configuration)
    -> std::expected<std::shared_ptr<ToolPolicy>, ToolPolicyError> {
  try {
    const bool automatic =
        configuration.launch_context.approval_mode() == ApprovalMode::automatic;
    if (automatic != static_cast<bool>(configuration.automatic_matcher)) {
      return std::unexpected(
          error(ToolPolicyErrorCode::invalid_profile,
                "automatic approval mode requires one compiled matcher"));
    }
    if (std::ranges::any_of(
            registered_tools.declarations(), [&](const auto& declaration) {
              const auto* tool = registered_tools.find(declaration.name);
              return tool == nullptr ||
                     !process_registration_matches(
                         *tool, configuration.launch_context);
            })) {
      return std::unexpected(
          error(ToolPolicyErrorCode::invalid_profile,
                "process registration does not match the launch restriction"));
    }

    if (automatic && (!configuration.launch_context.matcher_policy_identity() ||
                      *configuration.launch_context.matcher_policy_identity() !=
                          configuration.automatic_matcher->identity())) {
      return std::unexpected(
          error(ToolPolicyErrorCode::invalid_profile,
                "automatic matcher identity does not match its policy"));
    }
    if (automatic && std::ranges::any_of(
                         configuration.automatic_matcher->tool_names(),
                         [&](const auto& tool_name) {
                           return registered_tools.find(tool_name) == nullptr;
                         })) {
      return std::unexpected(
          error(ToolPolicyErrorCode::invalid_profile,
                "automatic approval rules require registered tools"));
    }

    auto provenance = make_provenance(registered_tools, configuration);
    if (!provenance) return std::unexpected(std::move(provenance.error()));
    auto policy = std::make_shared<LaunchPolicy>(
        registered_tools, std::move(configuration), std::move(*provenance));
    return std::shared_ptr<ToolPolicy>{std::move(policy)};
  } catch (...) {
    return std::unexpected(
        error(ToolPolicyErrorCode::internal_failure,
              "tool launch policy creation failed internally"));
  }
}

} // namespace aiforge::runtime
