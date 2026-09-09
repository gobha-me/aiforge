#include "../../src/lib/ops_tool_binding.hpp"

#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_observation_tool.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
namespace binding = runtime::ops_binding_detail;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
class Executor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock& value) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{value};
  }
  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::unavailable, "not executed"});
  }
};
class CounterfeitPolicy final : public runtime::ToolPolicy {
 public:
  domain::ToolPolicyProvenance copied;
  unsigned calls{};
  explicit CounterfeitPolicy(domain::ToolPolicyProvenance value)
      : copied(std::move(value)) {}
  auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* override {
    return &copied;
  }
  auto evaluate(const runtime::ToolPolicyRequest&)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++calls;
    return runtime::ToolPolicyResolution{};
  }
  auto approve(const runtime::ToolPolicyRequest&, runtime::ToolPolicyApproval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++calls;
    return runtime::ToolPolicyResolution{};
  }
};
struct Fixture {
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::shared_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint{
      broker->activate_session(id<domain::SessionId>("session")).value()};
  auto native(std::string target = "first", std::uint64_t generation = 1)
      -> runtime::RegisteredTool {
    domain::OpsObservationAuthoritySpec spec{
        id<domain::OpsOwnerId>("owner"),
        id<domain::SessionId>("session"),
        {id<domain::OpsTargetId>(std::move(target)),
         id<domain::OpsConfigurationRevision>("revision"),
         domain::LinuxOpsIdentity{domain::LinuxExecutionScope::container,
                                  "12345678-1234-1234-1234-123456789abc", 42,
                                  43}},
        generation,
        {domain::OpsObservationOperation::linux_health},
        {},
        {}};
    runtime::ToolRegistry registry;
    REQUIRE(runtime::register_ops_observation_tool(
        registry,
        domain::OpsObservationAuthority::create(std::move(spec)).value(),
        endpoint));
    return *registry.snapshot().value().find("observe_target");
  }
};
auto add(runtime::ToolRegistry& registry, const runtime::RegisteredTool& tool)
    -> void {
  REQUIRE(registry.register_tool(tool.declaration, tool.executor, tool.limits,
                                 tool.executor_contract, tool.category));
}
auto unrelated(std::string root = "/memory") -> runtime::RegisteredTool {
  return {{"capture",
           "fixture unrelated capture",
           {"application/schema+json", R"({"type":"object"})"},
           {domain::Effect::read},
           {{domain::Effect::read, "filesystem.root", std::move(root)}}},
          {},
          std::make_shared<Executor>(),
          runtime::ToolExecutorContract{"fixture.capture", "1"},
          runtime::ToolCategory::memory};
}
auto policy(const runtime::ToolRegistrySnapshot& tools,
            runtime::ApprovalMode mode = runtime::ApprovalMode::allow_all,
            std::shared_ptr<runtime::AutomaticApprovalMatcher> matcher = {})
    -> std::shared_ptr<runtime::ToolPolicy> {
  runtime::ApplicationLaunchContextConfiguration config;
  config.approval_mode = mode;
  if (matcher)
    config.matcher_policy_identity = std::string{matcher->identity()};
  auto context = runtime::make_application_launch_context(config);
  REQUIRE(context);
  auto result = runtime::make_tool_launch_policy(
      tools, {id<domain::PermissionProfileId>("observe"), *context,
              std::move(matcher)});
  REQUIRE(result);
  return *result;
}
auto request(std::string invocation, std::string tool,
             domain::CapabilityScope scope) -> runtime::ToolPolicyRequest {
  return {id<domain::SessionId>("session"),
          id<domain::RunId>("run"),
          id<domain::InvocationId>(std::move(invocation)),
          id<domain::PermissionProfileId>("observe"),
          std::move(tool),
          {scope.effect},
          {std::move(scope)},
          {},
          runtime::RestrictionLevel::high};
}
} // namespace

TEST_CASE("Ops binding rejects ordinary and altered reserved registrations") {
  Fixture f;
  const auto replacement = f.native("second", 2);
  for (int mutation = 0; mutation != 6; ++mutation) {
    auto altered = f.native();
    switch (mutation) {
      case 0: altered.executor = std::make_shared<Executor>(); break;
      case 1: altered.declaration.description += " changed"; break;
      case 2: altered.limits.timeout += std::chrono::milliseconds{1}; break;
      case 3: altered.executor_contract->version = "2"; break;
      case 4: altered.category = runtime::ToolCategory::memory; break;
      case 5: altered.declaration.capability_scopes[0].value = "foreign"; break;
    }
    runtime::ToolRegistry registry;
    add(registry, altered);
    const auto snapshot = registry.snapshot().value();
    CHECK_FALSE(binding::replace_ops_registration(snapshot, replacement));
    CHECK_FALSE(binding::replace_ops_registration({}, altered));
    const auto original = policy(snapshot);
    CHECK_FALSE(binding::rebind_ops_launch_policy(*original, replacement));
    CHECK(snapshot.find("observe_target")->executor == altered.executor);
  }
}

TEST_CASE("Ops policy rebind rejects custom policy despite copied provenance") {
  Fixture f;
  runtime::ToolRegistry registry;
  add(registry, f.native());
  const auto original = policy(registry.snapshot().value());
  CounterfeitPolicy copied{*original->provenance()};
  CHECK_FALSE(binding::rebind_ops_launch_policy(copied, f.native("second", 2)));
  CHECK(copied.calls == 0);
}

TEST_CASE("Ops rebinding preserves a broader unrelated policy ceiling") {
  Fixture f;
  runtime::ToolRegistry registry;
  const auto full_capture = unrelated();
  add(registry, full_capture);
  add(registry, f.native());
  REQUIRE(registry.declare_unavailable_tool(
      "unavailable", {runtime::ToolUnavailableReason::not_configured},
      runtime::ToolCategory::other));
  const auto original = registry.snapshot().value();
  const auto original_policy = policy(original);
  // This is the existing memory-capture pattern: an executor can narrow its
  // current declaration without replacing the launch policy's original ceiling.
  const auto narrow_capture = unrelated("/memory/off");
  const auto narrowed = original.replace(narrow_capture).value();
  const auto replacement = f.native("second", 2);
  const auto updated = binding::replace_ops_registration(narrowed, replacement);
  const auto updated_policy =
      binding::rebind_ops_launch_policy(*original_policy, replacement);
  REQUIRE(updated);
  REQUIRE(updated_policy);
  REQUIRE(updated->find("capture"));
  CHECK(updated->find("capture")->executor == narrow_capture.executor);
  CHECK(updated->find("capture")->declaration == narrow_capture.declaration);
  CHECK(updated->find("capture")->limits == narrow_capture.limits);
  CHECK(updated->find_unavailable("unavailable") != nullptr);
  CHECK(updated->declarations()[0].name == "capture");
  CHECK(updated->declarations()[1].name == "observe_target");
  CHECK(updated->find("observe_target")->executor == replacement.executor);
  auto enabled_again = updated->replace(full_capture);
  REQUIRE(enabled_again);
  const auto allowed =
      (*updated_policy)
          ->evaluate(
              request("capture-1", "capture",
                      {domain::Effect::read, "filesystem.root", "/memory/on"}));
  REQUIRE(allowed);
  CHECK(allowed->decision == domain::PolicyDecision::allow);
  CHECK((*updated_policy)->selected_restriction() ==
        original_policy->selected_restriction());
  CHECK((*updated_policy)->provenance()->approval_mode ==
        original_policy->provenance()->approval_mode);
  const auto wrong_target =
      (*updated_policy)
          ->evaluate(request("old", "observe_target",
                             {domain::Effect::read, "ops.target", "first"}));
  REQUIRE(wrong_target);
  CHECK(wrong_target->decision == domain::PolicyDecision::deny);
  const auto correct_target =
      (*updated_policy)
          ->evaluate(request("new", "observe_target",
                             {domain::Effect::read, "ops.target", "second"}));
  REQUIRE(correct_target);
  CHECK(correct_target->decision == domain::PolicyDecision::allow);
}

TEST_CASE("Ops rebinding retains consumed automatic rule counters") {
  Fixture f;
  runtime::ToolRegistry registry;
  add(registry, unrelated());
  add(registry, f.native());
  auto canonical = runtime::canonicalize_validated_tool_arguments(
      {"application/json", "{}"});
  REQUIRE(canonical);
  auto compiled = runtime::compile_automatic_approval_matcher(
      {runtime::ExactToolArgumentsApprovalRule{
          "capture",
          *canonical,
          {{runtime::RestrictionLevel::high}, 1, {}, 0}}});
  REQUIRE(compiled);
  std::shared_ptr<runtime::AutomaticApprovalMatcher> matcher{
      std::move(*compiled)};
  auto current = policy(registry.snapshot().value(),
                        runtime::ApprovalMode::automatic, matcher);
  auto first = request("once", "capture",
                       {domain::Effect::read, "filesystem.root", "/memory"});
  first.canonical_arguments = *canonical;
  const auto allowed = current->evaluate(first);
  REQUIRE(allowed);
  CHECK(allowed->decision == domain::PolicyDecision::allow);
  const auto matcher_identity = current->provenance()->matcher_policy_identity;
  for (std::uint64_t generation = 2; generation <= 4; ++generation) {
    auto rebound = binding::rebind_ops_launch_policy(
        *current, f.native("next", generation));
    REQUIRE(rebound);
    current = std::move(*rebound);
    first.invocation_id =
        id<domain::InvocationId>("after-" + std::to_string(generation));
    const auto denied = current->evaluate(first);
    REQUIRE(denied);
    CHECK(denied->decision == domain::PolicyDecision::deny);
    CHECK(current->provenance()->matcher_policy_identity == matcher_identity);
  }
}

TEST_CASE("Ops binding replaces explicit unavailability without collection") {
  Fixture f;
  runtime::ToolRegistry registry;
  add(registry, unrelated());
  REQUIRE(registry.declare_unavailable_tool(
      "observe_target", {runtime::ToolUnavailableReason::not_configured}));
  const auto snapshot = registry.snapshot().value();
  const auto original_policy = policy(snapshot, runtime::ApprovalMode::prompt);
  const auto replacement = f.native();
  auto updated = binding::replace_ops_registration(snapshot, replacement);
  auto updated_policy =
      binding::rebind_ops_launch_policy(*original_policy, replacement);
  REQUIRE(updated);
  REQUIRE(updated_policy);
  CHECK(updated->find_unavailable("observe_target") == nullptr);
  CHECK(updated->find("observe_target") != nullptr);
  CHECK(updated->find("capture")->executor ==
        snapshot.find("capture")->executor);
  const auto decision =
      (*updated_policy)
          ->evaluate(request("prompt", "observe_target",
                             {domain::Effect::read, "ops.target", "first"}));
  REQUIRE(decision);
  CHECK(decision->decision == domain::PolicyDecision::require_approval);
  CHECK_FALSE(f.broker->pending_work().value());
}
