#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>

namespace {

using namespace aiforge;

template <typename IdType> auto id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto scope(const domain::Effect effect) -> domain::CapabilityScope {
  switch (effect) {
    case domain::Effect::read:
    case domain::Effect::write:
    case domain::Effect::remove: return {effect, "filesystem.root", "/repo"};
    case domain::Effect::execute:
      return {effect, "process.command", "/usr/bin/git"};
    case domain::Effect::network:
    case domain::Effect::communicate:
      return {effect, "network.host", "api.example.test"};
    case domain::Effect::spend: return {effect, "spend.microunits", "1000"};
    case domain::Effect::change_infrastructure:
    case domain::Effect::change_privileges:
      return {effect, "cluster.resource", "cluster/ns/resource"};
  }
  return {effect, "unknown", "unknown"};
}

auto add_tool(runtime::ToolRegistry& registry, std::string name,
              std::vector<domain::Effect> effects,
              const runtime::ToolCategory category =
                  runtime::ToolCategory::other) -> void {
  std::vector<domain::CapabilityScope> scopes;
  scopes.reserve(effects.size());
  for (const auto effect : effects)
    scopes.push_back(scope(effect));
  auto added = registry.register_tool(
      {std::move(name),
       "test tool",
       {"application/schema+json", R"({"type":"object"})"},
       std::move(effects),
       std::move(scopes)},
      std::make_shared<testing::ScriptedToolExecutor>(
          std::vector<testing::ScriptedToolExchange>{}),
      {}, std::nullopt, category);
  REQUIRE(added);
}

auto registry() -> runtime::ToolRegistrySnapshot {
  runtime::ToolRegistry registry;
  add_tool(registry, "interaction", {});
  add_tool(registry, "read", {domain::Effect::read});
  add_tool(registry, "write", {domain::Effect::write});
  add_tool(registry, "remove", {domain::Effect::remove});
  add_tool(registry, "execute", {domain::Effect::execute});
  add_tool(registry, "network", {domain::Effect::network});
  add_tool(registry, "communicate", {domain::Effect::communicate});
  add_tool(registry, "spend", {domain::Effect::spend});
  add_tool(registry, "infrastructure", {domain::Effect::change_infrastructure});
  add_tool(registry, "privileges", {domain::Effect::change_privileges});
  return registry.snapshot().value();
}

enum class ProcessNetworkMarker { absent, unrestricted, host, both };

auto process_registry(const ProcessNetworkMarker marker,
                      const bool include_read = false)
    -> runtime::ToolRegistrySnapshot {
  std::vector effects{domain::Effect::execute};
  std::vector scopes{scope(domain::Effect::execute)};
  if (marker != ProcessNetworkMarker::absent) {
    effects.push_back(domain::Effect::network);
    if (marker == ProcessNetworkMarker::unrestricted ||
        marker == ProcessNetworkMarker::both) {
      scopes.push_back(
          {domain::Effect::network, "network.unrestricted", "new-sockets"});
    }
    if (marker == ProcessNetworkMarker::host ||
        marker == ProcessNetworkMarker::both) {
      scopes.push_back(scope(domain::Effect::network));
    }
  }
  runtime::ToolRegistry registry;
  auto added = registry.register_tool(
      {"run_process",
       "test process tool",
       {"application/schema+json", R"({"type":"object"})"},
       std::move(effects),
       std::move(scopes)},
      std::make_shared<testing::ScriptedToolExecutor>(
          std::vector<testing::ScriptedToolExchange>{}),
      {}, std::nullopt, runtime::ToolCategory::process);
  REQUIRE(added);
  if (include_read) add_tool(registry, "read", {domain::Effect::read});
  return registry.snapshot().value();
}

auto launch_context(
    const runtime::RestrictionLevel restriction,
    const runtime::ApprovalMode approval, const bool available = true,
    std::optional<std::string> matcher_policy_identity = std::nullopt)
    -> runtime::ApplicationLaunchContext {
  runtime::ApplicationLaunchContextConfiguration configured;
  configured.selected_restriction = restriction;
  configured.achieved_restriction =
      available ? std::optional{restriction} : std::nullopt;
  configured.unavailable_reason =
      available ? std::nullopt
                : std::optional{
                      runtime::RestrictionUnavailableReason::mechanism_absent};
  configured.restriction_policy_identity =
      available ? std::optional<std::string>{"test.process-policy.v1"}
                : std::nullopt;
  configured.mechanism = {"aiforge.linux-restriction-levels", "0018"};
  configured.approval_mode = approval;
  configured.matcher_policy_identity = std::move(matcher_policy_identity);
  auto result = runtime::make_application_launch_context(std::move(configured));
  REQUIRE(result);
  return std::move(*result);
}

auto configuration(
    const runtime::RestrictionLevel restriction,
    const runtime::ApprovalMode approval = runtime::ApprovalMode::allow_all,
    std::vector<std::string> automatic = {}, const bool available = true)
    -> runtime::ToolLaunchPolicyConfiguration {
  std::shared_ptr<runtime::AutomaticApprovalMatcher> matcher;
  std::optional<std::string> matcher_policy_identity;
  if (approval == runtime::ApprovalMode::automatic) {
    std::vector<runtime::AutomaticApprovalRule> rules;
    for (auto& tool_name : automatic) {
      rules.emplace_back(runtime::ExactToolArgumentsApprovalRule{
          std::move(tool_name),
          runtime::canonicalize_validated_tool_arguments(
              {"application/json", "{}"})
              .value(),
          {{restriction}, 1024, std::nullopt, 0}});
    }
    auto compiled =
        runtime::compile_automatic_approval_matcher(std::move(rules));
    if (compiled) {
      matcher = std::move(*compiled);
      matcher_policy_identity = std::string{matcher->identity()};
    } else {
      matcher_policy_identity = "invalid.matcher.v1";
    }
  }
  return {id<domain::PermissionProfileId>("launch"),
          launch_context(restriction, approval, available,
                         std::move(matcher_policy_identity)),
          std::move(matcher)};
}

auto request(std::string tool, std::vector<domain::Effect> effects,
             std::vector<domain::CapabilityScope> scopes = {})
    -> runtime::ToolPolicyRequest {
  const auto invocation = "invocation-" + tool;
  return {
      id<domain::SessionId>("session"),
      id<domain::RunId>("run"),
      id<domain::InvocationId>(invocation),
      id<domain::PermissionProfileId>("launch"),
      std::move(tool),
      std::move(effects),
      std::move(scopes),
      runtime::canonicalize_validated_tool_arguments({"application/json", "{}"})
          .value(),
      std::nullopt};
}

auto decision(runtime::ToolPolicy& policy, std::string tool,
              const domain::Effect effect) -> domain::PolicyDecision {
  auto policy_request = request(std::move(tool), {effect}, {scope(effect)});
  policy_request.selected_restriction = policy.selected_restriction();
  auto result = policy.evaluate(policy_request);
  REQUIRE(result);
  return result->decision;
}

} // namespace

TEST_CASE("application launch context rejects ambiguous or unsafe state",
          "[launch-policy][context][failure]") {
  runtime::ApplicationLaunchContextConfiguration configured;

  configured.selected_restriction = static_cast<runtime::RestrictionLevel>(100);
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.approval_mode = static_cast<runtime::ApprovalMode>(100);
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.achieved_restriction = runtime::RestrictionLevel::high;
  configured.unavailable_reason =
      runtime::RestrictionUnavailableReason::mechanism_absent;
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.unavailable_reason.reset();
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.achieved_restriction = runtime::RestrictionLevel::high;
  configured.unavailable_reason.reset();
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.achieved_restriction = runtime::RestrictionLevel::high;
  configured.unavailable_reason.reset();
  configured.restriction_policy_identity = "/sys/fs/cgroup/task";
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.selected_restriction = runtime::RestrictionLevel::high;
  configured.achieved_restriction = runtime::RestrictionLevel::medium;
  configured.unavailable_reason.reset();
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.mechanism.identity = "bad identity";
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.approval_mode = runtime::ApprovalMode::automatic;
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));

  configured = {};
  configured.matcher_policy_identity = "unexpected.matcher.v1";
  REQUIRE_FALSE(
      runtime::make_application_launch_context(std::move(configured)));
}

TEST_CASE("application launch context defaults high and unavailable",
          "[launch-policy][context][failure]") {
  auto context = runtime::make_application_launch_context({});
  REQUIRE(context);
  REQUIRE(context->selected_restriction() == runtime::RestrictionLevel::high);
  REQUIRE_FALSE(context->achieved_restriction());
  REQUIRE(context->unavailable_reason() ==
          runtime::RestrictionUnavailableReason::mechanism_absent);
  REQUIRE(context->approval_mode() == runtime::ApprovalMode::prompt);
  REQUIRE(context->mechanism().identity == "aiforge.linux-restriction-levels");
  REQUIRE(context->mechanism().version == "0018");
  REQUIRE_FALSE(context->matcher_policy_identity());
  REQUIRE_FALSE(context->restriction_policy_identity());
  REQUIRE_FALSE(context->process_network_contract());

  for (const auto restriction :
       {runtime::RestrictionLevel::none, runtime::RestrictionLevel::low,
        runtime::RestrictionLevel::medium, runtime::RestrictionLevel::high}) {
    const auto available =
        launch_context(restriction, runtime::ApprovalMode::prompt);
    REQUIRE(available.process_network_contract() ==
            (restriction == runtime::RestrictionLevel::none ||
                     restriction == runtime::RestrictionLevel::low
                 ? runtime::ProcessNetworkContract::unrestricted_new_sockets
                 : runtime::ProcessNetworkContract::deny_new_sockets));
  }
}

TEST_CASE("launch policy configuration rejects ambiguity before evaluation",
          "[launch-policy][failure]") {
  const auto tools = registry();

  auto ignored_allowlist = configuration(runtime::RestrictionLevel::none,
                                         runtime::ApprovalMode::prompt);
  ignored_allowlist.automatic_matcher =
      runtime::compile_automatic_approval_matcher({}).value();
  REQUIRE_FALSE(
      runtime::make_tool_launch_policy(tools, std::move(ignored_allowlist)));

  auto unknown = configuration(runtime::RestrictionLevel::none,
                               runtime::ApprovalMode::automatic, {"unknown"});
  REQUIRE_FALSE(runtime::make_tool_launch_policy(tools, std::move(unknown)));

  const auto duplicate_value = runtime::canonicalize_validated_tool_arguments(
      {"application/json", "{}"});
  REQUIRE(duplicate_value);
  const runtime::AutomaticApprovalRule duplicate_rule{
      runtime::ExactToolArgumentsApprovalRule{
          "read",
          *duplicate_value,
          {{runtime::RestrictionLevel::none}, 1, std::nullopt, 0}}};
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher(
      {duplicate_rule, duplicate_rule}));

  auto mismatched = runtime::ToolLaunchPolicyConfiguration{
      id<domain::PermissionProfileId>("launch"),
      launch_context(runtime::RestrictionLevel::none,
                     runtime::ApprovalMode::automatic, true,
                     "aiforge.wrong-matcher.v1"),
      runtime::compile_automatic_approval_matcher({}).value()};
  REQUIRE_FALSE(runtime::make_tool_launch_policy(tools, std::move(mismatched)));

  auto stale_matcher = configuration(runtime::RestrictionLevel::none,
                                     runtime::ApprovalMode::automatic);
  stale_matcher.automatic_matcher =
      runtime::compile_automatic_approval_matcher(
          {runtime::ExactToolArgumentsApprovalRule{
              "read",
              *duplicate_value,
              {{runtime::RestrictionLevel::none}, 1, std::nullopt, 0}}})
          .value();
  REQUIRE_FALSE(
      runtime::make_tool_launch_policy(tools, std::move(stale_matcher)));

  const auto empty_first = runtime::compile_automatic_approval_matcher({});
  const auto empty_second = runtime::compile_automatic_approval_matcher({});
  REQUIRE(empty_first);
  REQUIRE(empty_second);
  REQUIRE((*empty_first)->identity() == (*empty_second)->identity());
}

TEST_CASE("process registration matches the restriction network contract",
          "[launch-policy][restriction][network][failure]") {
  for (const auto restriction :
       {runtime::RestrictionLevel::none, runtime::RestrictionLevel::low}) {
    CAPTURE(restriction);
    REQUIRE(runtime::make_tool_launch_policy(
        process_registry(ProcessNetworkMarker::unrestricted),
        configuration(restriction)));
    REQUIRE_FALSE(runtime::make_tool_launch_policy(
        process_registry(ProcessNetworkMarker::absent),
        configuration(restriction)));
    REQUIRE_FALSE(runtime::make_tool_launch_policy(
        process_registry(ProcessNetworkMarker::host),
        configuration(restriction)));
    REQUIRE_FALSE(runtime::make_tool_launch_policy(
        process_registry(ProcessNetworkMarker::both),
        configuration(restriction)));
  }

  for (const auto restriction :
       {runtime::RestrictionLevel::medium, runtime::RestrictionLevel::high}) {
    CAPTURE(restriction);
    REQUIRE(runtime::make_tool_launch_policy(
        process_registry(ProcessNetworkMarker::absent),
        configuration(restriction)));
    REQUIRE_FALSE(runtime::make_tool_launch_policy(
        process_registry(ProcessNetworkMarker::unrestricted),
        configuration(restriction)));
    REQUIRE_FALSE(runtime::make_tool_launch_policy(
        process_registry(ProcessNetworkMarker::host),
        configuration(restriction)));
  }

  REQUIRE(runtime::make_tool_launch_policy(
      process_registry(ProcessNetworkMarker::absent),
      configuration(runtime::RestrictionLevel::high,
                    runtime::ApprovalMode::allow_all, {}, false)));
  REQUIRE(runtime::make_tool_launch_policy(
      process_registry(ProcessNetworkMarker::unrestricted),
      configuration(runtime::RestrictionLevel::high,
                    runtime::ApprovalMode::allow_all, {}, false)));
  REQUIRE_FALSE(runtime::make_tool_launch_policy(
      process_registry(ProcessNetworkMarker::host),
      configuration(runtime::RestrictionLevel::high,
                    runtime::ApprovalMode::allow_all, {}, false)));
  REQUIRE_FALSE(runtime::make_tool_launch_policy(
      process_registry(ProcessNetworkMarker::both),
      configuration(runtime::RestrictionLevel::high,
                    runtime::ApprovalMode::allow_all, {}, false)));
}

TEST_CASE("restriction is orthogonal to authority and gates only process",
          "[launch-policy][restriction][failure]") {
  const auto tools = registry();
  const std::array restrictions{
      runtime::RestrictionLevel::high, runtime::RestrictionLevel::medium,
      runtime::RestrictionLevel::low, runtime::RestrictionLevel::none};
  const std::array cases{
      std::pair{"read", domain::Effect::read},
      std::pair{"write", domain::Effect::write},
      std::pair{"remove", domain::Effect::remove},
      std::pair{"execute", domain::Effect::execute},
      std::pair{"network", domain::Effect::network},
      std::pair{"communicate", domain::Effect::communicate},
      std::pair{"spend", domain::Effect::spend},
      std::pair{"infrastructure", domain::Effect::change_infrastructure},
      std::pair{"privileges", domain::Effect::change_privileges}};
  for (const auto restriction : restrictions) {
    auto policy =
        runtime::make_tool_launch_policy(tools, configuration(restriction));
    REQUIRE(policy);
    for (const auto& [tool_name, effect] : cases) {
      CAPTURE(restriction, tool_name);
      REQUIRE(decision(**policy, tool_name, effect) ==
              domain::PolicyDecision::allow);
    }
  }

  auto unavailable = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::high,
                           runtime::ApprovalMode::allow_all, {}, false));
  REQUIRE(unavailable);
  REQUIRE(decision(**unavailable, "read", domain::Effect::read) ==
          domain::PolicyDecision::allow);
  auto unavailable_process = runtime::make_tool_launch_policy(
      process_registry(ProcessNetworkMarker::unrestricted, true),
      configuration(runtime::RestrictionLevel::high,
                    runtime::ApprovalMode::allow_all, {}, false));
  REQUIRE(unavailable_process);
  REQUIRE(decision(**unavailable_process, "read", domain::Effect::read) ==
          domain::PolicyDecision::allow);
  auto process =
      (*unavailable_process)
          ->evaluate(request("run_process",
                             {domain::Effect::execute, domain::Effect::network},
                             {scope(domain::Effect::execute),
                              {domain::Effect::network, "network.unrestricted",
                               "new-sockets"}}));
  REQUIRE(process);
  REQUIRE(process->decision == domain::PolicyDecision::deny);
}

TEST_CASE("every restriction and approval mode combination is closed",
          "[launch-policy][restriction][approval]") {
  const auto tools = registry();
  const std::array restrictions{
      runtime::RestrictionLevel::high, runtime::RestrictionLevel::medium,
      runtime::RestrictionLevel::low, runtime::RestrictionLevel::none};
  const std::array modes{runtime::ApprovalMode::prompt,
                         runtime::ApprovalMode::automatic,
                         runtime::ApprovalMode::allow_all};
  for (const auto restriction : restrictions) {
    for (const auto mode : modes) {
      auto configured = configuration(restriction, mode,
                                      mode == runtime::ApprovalMode::automatic
                                          ? std::vector<std::string>{"read"}
                                          : std::vector<std::string>{});
      auto policy =
          runtime::make_tool_launch_policy(tools, std::move(configured));
      REQUIRE(policy);
      auto policy_request = request("read", {domain::Effect::read},
                                    {scope(domain::Effect::read)});
      policy_request.selected_restriction = restriction;
      auto result = (*policy)->evaluate(policy_request);
      REQUIRE(result);

      const auto expected = mode == runtime::ApprovalMode::prompt
                                ? domain::PolicyDecision::require_approval
                                : domain::PolicyDecision::allow;
      REQUIRE(result->decision == expected);

      policy_request.invocation_id =
          id<domain::InvocationId>("invocation-without-canonical-arguments");
      policy_request.canonical_arguments.reset();
      result = (*policy)->evaluate(policy_request);
      REQUIRE(result);
      const auto missing_expected = mode == runtime::ApprovalMode::automatic
                                        ? domain::PolicyDecision::deny
                                        : expected;
      REQUIRE(result->decision == missing_expected);
    }
  }
}

TEST_CASE("declarations remain the exact authority ceiling",
          "[launch-policy][scope][failure]") {
  const auto tools = registry();
  auto policy = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::none));
  REQUIRE(policy);

  auto narrowed = (*policy)->evaluate(
      request("read", {domain::Effect::read},
              {{domain::Effect::read, "filesystem.root", "/repo/src"}}));
  REQUIRE(narrowed);
  REQUIRE(narrowed->decision == domain::PolicyDecision::allow);

  auto widened = (*policy)->evaluate(
      request("read", {domain::Effect::read},
              {{domain::Effect::read, "filesystem.root", "/outside"}}));
  REQUIRE(widened);
  REQUIRE(widened->decision == domain::PolicyDecision::deny);

  auto undeclared_effect = (*policy)->evaluate(
      request("read", {domain::Effect::write}, {scope(domain::Effect::write)}));
  REQUIRE(undeclared_effect);
  REQUIRE(undeclared_effect->decision == domain::PolicyDecision::deny);

  auto unregistered = (*policy)->evaluate(request(
      "missing", {domain::Effect::read}, {scope(domain::Effect::read)}));
  REQUIRE(unregistered);
  REQUIRE(unregistered->decision == domain::PolicyDecision::deny);

  auto malformed =
      request("read", {domain::Effect::read},
              {{domain::Effect::write, "filesystem.root", "/repo"}});
  auto malformed_result = (*policy)->evaluate(malformed);
  REQUIRE_FALSE(malformed_result);
  REQUIRE(malformed_result.error().code ==
          runtime::ToolPolicyErrorCode::invalid_request);

  auto wrong_profile =
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)});
  wrong_profile.permission_profile_id =
      id<domain::PermissionProfileId>("different");
  auto wrong_profile_result = (*policy)->evaluate(wrong_profile);
  REQUIRE_FALSE(wrong_profile_result);
  REQUIRE(wrong_profile_result.error().code ==
          runtime::ToolPolicyErrorCode::invalid_profile);
}

TEST_CASE("every approval mode rejects effectful requests without exact scope",
          "[launch-policy][scope][failure]") {
  const auto tools = registry();
  const std::array modes{runtime::ApprovalMode::prompt,
                         runtime::ApprovalMode::automatic,
                         runtime::ApprovalMode::allow_all};
  for (const auto mode : modes) {
    auto configured = configuration(runtime::RestrictionLevel::medium, mode,
                                    mode == runtime::ApprovalMode::automatic
                                        ? std::vector<std::string>{"read"}
                                        : std::vector<std::string>{});
    auto policy =
        runtime::make_tool_launch_policy(tools, std::move(configured));
    REQUIRE(policy);

    auto missing_scope =
        (*policy)->evaluate(request("read", {domain::Effect::read}));
    REQUIRE_FALSE(missing_scope);
    REQUIRE(missing_scope.error().code ==
            runtime::ToolPolicyErrorCode::invalid_request);
  }
}

TEST_CASE("prompt mode requires exact invocation approval for effectful tools",
          "[launch-policy][approval][failure]") {
  const auto tools = registry();
  auto policy = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::medium,
                           runtime::ApprovalMode::prompt));
  REQUIRE(policy);
  const auto pending =
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)});
  auto evaluated = (*policy)->evaluate(pending);
  REQUIRE(evaluated);
  REQUIRE(evaluated->decision == domain::PolicyDecision::require_approval);

  auto approved =
      (*policy)->approve(pending, {{scope(domain::Effect::read)},
                                   domain::ApprovalGrantLifetime::invocation});
  REQUIRE(approved);
  REQUIRE(approved->decision == domain::PolicyDecision::allow);
  REQUIRE(approved->source == domain::PolicyDecisionSource::user_approval);

  auto persistent =
      (*policy)->approve(pending, {{scope(domain::Effect::read)},
                                   domain::ApprovalGrantLifetime::session});
  REQUIRE_FALSE(persistent);
  REQUIRE(persistent.error().code ==
          runtime::ToolPolicyErrorCode::scope_widening);

  auto widened = (*policy)->approve(
      pending, {{{domain::Effect::read, "filesystem.root", "/"}},
                domain::ApprovalGrantLifetime::invocation});
  REQUIRE_FALSE(widened);
  REQUIRE(widened.error().code == runtime::ToolPolicyErrorCode::scope_widening);
  REQUIRE((*policy)->evaluate(pending)->decision ==
          domain::PolicyDecision::require_approval);
}

TEST_CASE("automatic mode allows only matching registered rules",
          "[launch-policy][automatic][failure]") {
  const auto tools = registry();
  auto deny_all = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::low,
                           runtime::ApprovalMode::automatic));
  REQUIRE(deny_all);
  REQUIRE(decision(**deny_all, "read", domain::Effect::read) ==
          domain::PolicyDecision::deny);

  auto policy = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::low,
                           runtime::ApprovalMode::automatic,
                           {"read", "infrastructure"}));
  REQUIRE(policy);
  REQUIRE(decision(**policy, "read", domain::Effect::read) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**policy, "write", domain::Effect::write) ==
          domain::PolicyDecision::deny);
  REQUIRE(decision(**policy, "infrastructure",
                   domain::Effect::change_infrastructure) ==
          domain::PolicyDecision::allow);

  auto unnecessary = (*policy)->approve(
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)}),
      {{scope(domain::Effect::read)},
       domain::ApprovalGrantLifetime::invocation});
  REQUIRE_FALSE(unnecessary);
  REQUIRE(unnecessary.error().code ==
          runtime::ToolPolicyErrorCode::invalid_request);
}

TEST_CASE("automatic matching cannot bypass launch authority checks",
          "[launch-policy][automatic][scope][restriction][failure]") {
  const auto tools = registry();
  auto matcher = runtime::compile_automatic_approval_matcher(
      {runtime::ExactToolArgumentsApprovalRule{
          "read",
          runtime::canonicalize_validated_tool_arguments(
              {"application/json", "{}"})
              .value(),
          {{runtime::RestrictionLevel::low}, 1, std::nullopt, 0}}});
  REQUIRE(matcher);
  const auto matcher_identity = std::string{(*matcher)->identity()};
  auto configured = runtime::ToolLaunchPolicyConfiguration{
      id<domain::PermissionProfileId>("launch"),
      launch_context(runtime::RestrictionLevel::low,
                     runtime::ApprovalMode::automatic, true, matcher_identity),
      *matcher};
  auto policy = runtime::make_tool_launch_policy(tools, std::move(configured));
  REQUIRE(policy);

  auto stale_restriction =
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)});
  stale_restriction.selected_restriction = runtime::RestrictionLevel::medium;
  REQUIRE((*policy)->evaluate(stale_restriction)->decision ==
          domain::PolicyDecision::deny);

  auto widened = request("read", {domain::Effect::read},
                         {{domain::Effect::read, "filesystem.root", "/"}});
  widened.selected_restriction = runtime::RestrictionLevel::low;
  REQUIRE((*policy)->evaluate(widened)->decision ==
          domain::PolicyDecision::deny);

  auto stale_canonicalization =
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)});
  stale_canonicalization.selected_restriction = runtime::RestrictionLevel::low;
  stale_canonicalization.canonical_arguments->canonicalization_identity =
      "aiforge.canonical-tool-json.v2";
  const auto stale = (*policy)->evaluate(stale_canonicalization);
  REQUIRE(stale);
  REQUIRE(stale->decision == domain::PolicyDecision::deny);

  auto missing_canonicalization =
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)});
  missing_canonicalization.selected_restriction =
      runtime::RestrictionLevel::low;
  missing_canonicalization.canonical_arguments.reset();
  const auto missing = (*policy)->evaluate(missing_canonicalization);
  REQUIRE(missing);
  REQUIRE(missing->decision == domain::PolicyDecision::deny);

  auto valid =
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)});
  valid.selected_restriction = runtime::RestrictionLevel::low;
  const auto allowed = (*policy)->evaluate(valid);
  REQUIRE(allowed);
  REQUIRE(allowed->decision == domain::PolicyDecision::allow);
  REQUIRE(allowed->source == domain::PolicyDecisionSource::automatic_matcher);
  REQUIRE(allowed->automatic_approval.has_value());

  valid.invocation_id = id<domain::InvocationId>("second-read");
  REQUIRE((*policy)->evaluate(valid)->decision == domain::PolicyDecision::deny);
}

TEST_CASE("v2 policy provenance is bounded and omits raw automatic rules",
          "[launch-policy][provenance][failure]") {
  const auto tools = registry();
  auto policy = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::low,
                           runtime::ApprovalMode::automatic,
                           {"read", "infrastructure"}));
  REQUIRE(policy);
  const auto* provenance = (*policy)->provenance();
  REQUIRE(provenance != nullptr);
  REQUIRE(provenance->identity == "aiforge.tool-launch-policy.v2");
  REQUIRE(provenance->restriction_level == domain::ToolRestrictionLevel::low);
  REQUIRE(provenance->achieved_restriction_level ==
          domain::ToolRestrictionLevel::low);
  REQUIRE_FALSE(provenance->restriction_unavailable_reason);
  REQUIRE(provenance->mechanism_identity == "aiforge.linux-restriction-levels");
  REQUIRE(provenance->mechanism_version == "0018");
  REQUIRE(provenance->restriction_policy_identity == "test.process-policy.v1");
  REQUIRE(provenance->approval_mode == domain::ToolApprovalMode::automatic);
  REQUIRE(provenance->matcher_policy_identity.has_value());
  REQUIRE(provenance->automatically_eligible_tools.empty());
  REQUIRE(std::ranges::find(provenance->effect_ceiling,
                            domain::Effect::change_privileges) !=
          provenance->effect_ceiling.end());
  REQUIRE(domain::validate_tool_policy_provenance(*provenance));

  auto unavailable = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::high,
                           runtime::ApprovalMode::prompt, {}, false));
  REQUIRE(unavailable);
  provenance = (*unavailable)->provenance();
  REQUIRE_FALSE(provenance->achieved_restriction_level);
  REQUIRE_FALSE(provenance->restriction_policy_identity);
  REQUIRE(provenance->restriction_unavailable_reason ==
          domain::ToolRestrictionUnavailableReason::mechanism_absent);
}

TEST_CASE("launch policy evaluation is deterministic under concurrency",
          "[launch-policy][concurrency]") {
  const auto tools = registry();
  auto policy = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::medium,
                           runtime::ApprovalMode::allow_all));
  REQUIRE(policy);
  std::atomic<int> allowed{};
  std::array<std::jthread, 8> workers;
  for (auto& worker : workers) {
    worker = std::jthread([&] {
      for (int attempt = 0; attempt < 100; ++attempt) {
        const auto result = (*policy)->evaluate(request(
            "write", {domain::Effect::write}, {scope(domain::Effect::write)}));
        if (result && result->decision == domain::PolicyDecision::allow) {
          ++allowed;
        }
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  REQUIRE(allowed == 800);
}
