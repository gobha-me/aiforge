#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
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
              std::vector<domain::Effect> effects) -> void {
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
          std::vector<testing::ScriptedToolExchange>{}));
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

auto configuration(
    const runtime::RestrictionLevel restriction,
    const runtime::ApprovalMode approval = runtime::ApprovalMode::allow_all,
    std::vector<std::string> automatic = {})
    -> runtime::ToolLaunchPolicyConfiguration {
  return {id<domain::PermissionProfileId>("launch"), restriction, approval,
          std::move(automatic)};
}

auto request(std::string tool, std::vector<domain::Effect> effects,
             std::vector<domain::CapabilityScope> scopes = {})
    -> runtime::ToolPolicyRequest {
  return {id<domain::SessionId>("session"),
          id<domain::RunId>("run"),
          id<domain::InvocationId>("invocation"),
          id<domain::PermissionProfileId>("launch"),
          std::move(tool),
          std::move(effects),
          std::move(scopes)};
}

auto decision(runtime::ToolPolicy& policy, std::string tool,
              const domain::Effect effect) -> domain::PolicyDecision {
  auto result =
      policy.evaluate(request(std::move(tool), {effect}, {scope(effect)}));
  REQUIRE(result);
  return result->decision;
}

} // namespace

TEST_CASE("launch policy configuration rejects ambiguity before evaluation",
          "[launch-policy][failure]") {
  const auto tools = registry();

  auto invalid_restriction = configuration(runtime::RestrictionLevel::high);
  invalid_restriction.restriction_level =
      static_cast<runtime::RestrictionLevel>(100);
  REQUIRE_FALSE(
      runtime::make_tool_launch_policy(tools, std::move(invalid_restriction)));

  auto invalid_mode = configuration(runtime::RestrictionLevel::high);
  invalid_mode.approval_mode = static_cast<runtime::ApprovalMode>(100);
  REQUIRE_FALSE(
      runtime::make_tool_launch_policy(tools, std::move(invalid_mode)));

  auto ignored_allowlist = configuration(
      runtime::RestrictionLevel::none, runtime::ApprovalMode::prompt, {"read"});
  REQUIRE_FALSE(
      runtime::make_tool_launch_policy(tools, std::move(ignored_allowlist)));

  auto unknown = configuration(runtime::RestrictionLevel::none,
                               runtime::ApprovalMode::automatic, {"unknown"});
  REQUIRE_FALSE(runtime::make_tool_launch_policy(tools, std::move(unknown)));

  auto duplicate =
      configuration(runtime::RestrictionLevel::none,
                    runtime::ApprovalMode::automatic, {"read", "read"});
  REQUIRE_FALSE(runtime::make_tool_launch_policy(tools, std::move(duplicate)));
}

TEST_CASE("restriction levels deterministically reduce effect authority",
          "[launch-policy][restriction][failure]") {
  const auto tools = registry();
  auto high = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::high));
  REQUIRE(high);
  REQUIRE(decision(**high, "read", domain::Effect::read) ==
          domain::PolicyDecision::deny);
  auto interaction = (*high)->evaluate(request("interaction", {}));
  REQUIRE(interaction);
  REQUIRE(interaction->decision == domain::PolicyDecision::allow);

  auto medium = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::medium));
  REQUIRE(medium);
  REQUIRE(decision(**medium, "read", domain::Effect::read) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**medium, "write", domain::Effect::write) ==
          domain::PolicyDecision::deny);

  auto low = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::low));
  REQUIRE(low);
  REQUIRE(decision(**low, "write", domain::Effect::write) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**low, "remove", domain::Effect::remove) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**low, "execute", domain::Effect::execute) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**low, "network", domain::Effect::network) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**low, "communicate", domain::Effect::communicate) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**low, "spend", domain::Effect::spend) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**low, "infrastructure",
                   domain::Effect::change_infrastructure) ==
          domain::PolicyDecision::deny);
  REQUIRE(decision(**low, "privileges", domain::Effect::change_privileges) ==
          domain::PolicyDecision::deny);

  auto none = runtime::make_tool_launch_policy(
      tools, configuration(runtime::RestrictionLevel::none));
  REQUIRE(none);
  REQUIRE(decision(**none, "infrastructure",
                   domain::Effect::change_infrastructure) ==
          domain::PolicyDecision::allow);
  REQUIRE(decision(**none, "privileges", domain::Effect::change_privileges) ==
          domain::PolicyDecision::allow);
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
      auto configured = configuration(restriction, mode);
      if (mode == runtime::ApprovalMode::automatic) {
        configured.automatically_eligible_tools = {"read"};
      }
      auto policy =
          runtime::make_tool_launch_policy(tools, std::move(configured));
      REQUIRE(policy);
      auto result = (*policy)->evaluate(request("read", {domain::Effect::read},
                                                {scope(domain::Effect::read)}));
      REQUIRE(result);

      const auto expected = restriction == runtime::RestrictionLevel::high
                                ? domain::PolicyDecision::deny
                            : mode == runtime::ApprovalMode::prompt
                                ? domain::PolicyDecision::require_approval
                                : domain::PolicyDecision::allow;
      REQUIRE(result->decision == expected);
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
    auto configured = configuration(runtime::RestrictionLevel::medium, mode);
    if (mode == runtime::ApprovalMode::automatic) {
      configured.automatically_eligible_tools = {"read"};
    }
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

TEST_CASE("automatic mode allows only configured registered tools",
          "[launch-policy][automatic][failure]") {
  const auto tools = registry();
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
          domain::PolicyDecision::deny);

  auto unnecessary = (*policy)->approve(
      request("read", {domain::Effect::read}, {scope(domain::Effect::read)}),
      {{scope(domain::Effect::read)},
       domain::ApprovalGrantLifetime::invocation});
  REQUIRE_FALSE(unnecessary);
  REQUIRE(unnecessary.error().code ==
          runtime::ToolPolicyErrorCode::invalid_request);
}
