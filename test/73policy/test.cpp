#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/testing/scripted_policy_grant_store.hpp>

namespace {

using namespace aiforge;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto root(std::string value = "/repo") -> domain::CapabilityScope {
  return {domain::Effect::read, "filesystem.root", std::move(value)};
}

auto profile() -> runtime::PermissionProfile {
  return {make_id<domain::PermissionProfileId>("observe"),
          {domain::Effect::read},
          {root("/repo/public")},
          {domain::Effect::read, domain::Effect::network,
           domain::Effect::execute, domain::Effect::spend},
          {root(),
           {domain::Effect::network, "network.host", "api.example.com:443"},
           {domain::Effect::execute, "process.command", "/usr/bin/git"},
           {domain::Effect::spend, "spend.microunits", "5000000"}}};
}

auto request(std::string tool, std::vector<domain::Effect> effects,
             std::vector<domain::CapabilityScope> scopes,
             std::string session = "session") -> runtime::ToolPolicyRequest {
  return {make_id<domain::SessionId>(session),
          make_id<domain::RunId>("run"),
          make_id<domain::InvocationId>("call"),
          make_id<domain::PermissionProfileId>("observe"),
          std::move(tool),
          std::move(effects),
          std::move(scopes)};
}

} // namespace

TEST_CASE("capability scopes reject ambiguity and preserve containment",
          "[policy][scope][failure]") {
  auto normalized = runtime::normalize_capability_scope(
      {domain::Effect::read, "root", "/repo"});
  REQUIRE(normalized);
  REQUIRE(normalized->kind == "filesystem.root");
  REQUIRE(runtime::capability_scope_covers(*normalized, root("/repo/src")));
  REQUIRE_FALSE(
      runtime::capability_scope_covers(*normalized, root("/repository")));

  REQUIRE_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::read, "filesystem.root", "repo"}));
  REQUIRE_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::read, "filesystem.root", "/repo/../outside"}));
  REQUIRE_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::execute, "network.host", "api.example.com"}));
  REQUIRE_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::network, "network.host", "*.example.com"}));
  REQUIRE(runtime::normalize_capability_scope(
      {domain::Effect::network, "network.unrestricted", "new-sockets"}));
  REQUIRE_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::network, "network.unrestricted", "all"}));
  REQUIRE_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::communicate, "network.unrestricted", "new-sockets"}));
  REQUIRE_FALSE(runtime::capability_scope_covers(
      {domain::Effect::network, "network.host", "api.example.com"},
      {domain::Effect::network, "network.host", "attacker.example.com"}));
  REQUIRE_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::spend, "spend.microunits", "18446744073709551616"}));

  const auto intersection = runtime::intersect_capability_scopes(
      {root()}, {root("/repo/src"), root("/outside")});
  REQUIRE_FALSE(intersection);
  REQUIRE(intersection.error().code ==
          runtime::ToolPolicyErrorCode::scope_widening);
}

TEST_CASE("permission profile allows, asks, and denies without scope widening",
          "[policy][profile][failure]") {
  runtime::CapabilityPolicy policy{profile()};

  auto allowed = policy.evaluate(request("read_file", {domain::Effect::read},
                                         {root("/repo/public/file")}));
  REQUIRE(allowed);
  REQUIRE(allowed->decision == domain::PolicyDecision::allow);
  REQUIRE(allowed->source == domain::PolicyDecisionSource::permission_profile);

  auto approval = policy.evaluate(
      request("read_file", {domain::Effect::read}, {root("/repo/private")}));
  REQUIRE(approval);
  REQUIRE(approval->decision == domain::PolicyDecision::require_approval);

  auto denied = policy.evaluate(
      request("read_file", {domain::Effect::read}, {root("/outside")}));
  REQUIRE(denied);
  REQUIRE(denied->decision == domain::PolicyDecision::deny);

  auto malformed =
      request("read_file", {domain::Effect::read},
              {{domain::Effect::write, "filesystem.root", "/repo"}});
  REQUIRE_FALSE(policy.evaluate(malformed));

  auto invalid_profile = profile();
  invalid_profile.automatic_effects.clear();
  runtime::CapabilityPolicy invalid_policy{std::move(invalid_profile)};
  auto invalid = invalid_policy.evaluate(request(
      "read_file", {domain::Effect::read}, {root("/repo/public/file")}));
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code ==
          runtime::ToolPolicyErrorCode::invalid_profile);
}

TEST_CASE("default policy allows no-effect interaction and denies authority",
          "[policy][default][failure]") {
  auto policy = runtime::default_tool_policy();
  auto interaction = policy->evaluate(request("ask_user", {}, {}));
  REQUIRE(interaction);
  REQUIRE(interaction->decision == domain::PolicyDecision::allow);

  auto effectful =
      policy->evaluate(request("read_file", {domain::Effect::read}, {root()}));
  REQUIRE(effectful);
  REQUIRE(effectful->decision == domain::PolicyDecision::deny);
}

TEST_CASE("approval lifetimes are invocation, session, or explicitly saved",
          "[policy][approval][storage]") {
  testing::ScriptedPolicyGrantStore store;
  runtime::CapabilityPolicy policy{profile(), &store};
  const auto pending =
      request("read_file", {domain::Effect::read}, {root("/repo/private")});

  auto once =
      policy.approve(pending, {{root("/repo/private")},
                               domain::ApprovalGrantLifetime::invocation});
  REQUIRE(once);
  REQUIRE(policy.evaluate(pending)->decision ==
          domain::PolicyDecision::require_approval);

  auto session =
      policy.approve(pending, {{root("/repo/private")},
                               domain::ApprovalGrantLifetime::session});
  REQUIRE(session);
  auto reused = policy.evaluate(pending);
  REQUIRE(reused);
  REQUIRE(reused->decision == domain::PolicyDecision::allow);
  REQUIRE(reused->source == domain::PolicyDecisionSource::session_grant);
  REQUIRE(policy
              .evaluate(request("read_file", {domain::Effect::read},
                                {root("/repo/private")}, "other-session"))
              ->decision == domain::PolicyDecision::require_approval);

  auto saved = policy.approve(
      request(
          "network", {domain::Effect::network},
          {{domain::Effect::network, "network.host", "API.EXAMPLE.COM:443"}}),
      {{{domain::Effect::network, "network.host", "api.example.com:443"}},
       domain::ApprovalGrantLifetime::saved});
  REQUIRE(saved);
  REQUIRE(store.saved_grants().size() == 1);

  runtime::CapabilityPolicy restored{profile(), &store};
  auto restored_decision = restored.evaluate(request(
      "network", {domain::Effect::network},
      {{domain::Effect::network, "network.host", "api.example.com:443"}},
      "restored-session"));
  REQUIRE(restored_decision);
  REQUIRE(restored_decision->source ==
          domain::PolicyDecisionSource::saved_grant);

  auto restricted_profile = profile();
  restricted_profile.approval_ceiling = {root("/repo/public")};
  runtime::CapabilityPolicy restricted{std::move(restricted_profile), &store};
  auto stale = restricted.evaluate(request(
      "network", {domain::Effect::network},
      {{domain::Effect::network, "network.host", "api.example.com:443"}}));
  REQUIRE(stale);
  REQUIRE(stale->decision == domain::PolicyDecision::deny);

  auto escalated = restored.evaluate(
      request("network", {domain::Effect::network, domain::Effect::write},
              {{domain::Effect::network, "network.host", "api.example.com:443"},
               {domain::Effect::write, "filesystem.root", "/repo/private"}}));
  REQUIRE(escalated);
  REQUIRE(escalated->decision == domain::PolicyDecision::deny);

  auto widened = policy.approve(
      pending, {{root()}, domain::ApprovalGrantLifetime::invocation});
  REQUIRE_FALSE(widened);
  REQUIRE(widened.error().code == runtime::ToolPolicyErrorCode::scope_widening);
}

TEST_CASE("saved policy failures are typed and never become grants",
          "[policy][storage][failure]") {
  testing::ScriptedPolicyGrantStore store;
  store.fail_save({storage::PolicyGrantStoreErrorCode::permission_denied,
                   "secret-bearing filesystem detail", false});
  runtime::CapabilityPolicy policy{profile(), &store};
  const auto pending =
      request("read_file", {domain::Effect::read}, {root("/repo/private")});
  auto result = policy.approve(
      pending, {{root("/repo/private")}, domain::ApprovalGrantLifetime::saved});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolPolicyErrorCode::persistence_failure);
  REQUIRE(result.error().message.find("secret-bearing") == std::string::npos);
  REQUIRE(store.saved_grants().empty());

  store.fail_load({storage::PolicyGrantStoreErrorCode::unavailable,
                   "secret-bearing database detail", true});
  auto load = policy.evaluate(pending);
  REQUIRE_FALSE(load);
  REQUIRE(load.error().retryable);
  REQUIRE(load.error().message.find("secret-bearing") == std::string::npos);
}

TEST_CASE("malformed saved policy grants fail closed",
          "[policy][storage][failure]") {
  testing::ScriptedPolicyGrantStore store{{
      {make_id<domain::PermissionProfileId>("observe"),
       "read_file",
       {domain::Effect::read},
       {{domain::Effect::read, "filesystem.root", "/repo/../outside"}}},
  }};
  runtime::CapabilityPolicy policy{profile(), &store};

  auto result = policy.evaluate(
      request("read_file", {domain::Effect::read}, {root("/repo/private")}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolPolicyErrorCode::persistence_failure);
}
