#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_policy_grant_store.hpp>
#include <aiforge/testing/scripted_session_store.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto declaration(std::string name = "lookup") -> backend::ToolDeclaration {
  return backend::ToolDeclaration{
      std::move(name),
      "Look up a value",
      {"application/schema+json", R"({"type":"object"})"},
      {domain::Effect::read},
      {{domain::Effect::read, "filesystem.root", "/repo"}}};
}

auto context(std::vector<domain::Message> tool_messages = {})
    -> domain::ConstructedContext {
  std::vector<domain::ContextEntry> entries{domain::ContextEntry{
      make_id<domain::ContextEntryId>("runtime-context"),
      domain::ContextEntryKind::instruction,
      domain::InstructionLayer::application_runtime,
      domain::Message{make_id<domain::MessageId>("runtime-message"),
                      domain::Role::system,
                      {domain::TextBlock{"runtime contract"}},
                      std::nullopt},
      {make_id<domain::ContextSourceId>("runtime-source"), std::nullopt,
       std::nullopt},
      0,
      1,
      2}};
  std::uint64_t order{2};
  for (auto& message : tool_messages) {
    entries.push_back(domain::ContextEntry{
        make_id<domain::ContextEntryId>("tool-entry-" + std::to_string(order)),
        domain::ContextEntryKind::tool_result,
        std::nullopt,
        std::move(message),
        {make_id<domain::ContextSourceId>("tool-source-" +
                                          std::to_string(order)),
         std::nullopt, std::nullopt},
        0,
        order++,
        1});
  }
  return {std::move(entries), {}, {4096, 512, 0}, 2};
}

auto request(std::string inference, std::string assistant,
             std::vector<backend::ToolDeclaration> tools,
             std::vector<domain::Message> tool_messages = {})
    -> backend::BackendRequest {
  return {make_id<domain::InferenceId>(inference),
          make_id<domain::MessageId>(assistant),
          make_id<domain::ModelId>("model"),
          context(std::move(tool_messages)),
          std::move(tools),
          {0.25, 128, 42, {}}};
}

auto run_start(backend::BackendRequest backend_request) -> runtime::RunStart {
  return runtime::RunStart{
      make_id<domain::RunId>("run"),
      domain::RunStarted{make_id<domain::SurfaceId>("test"),
                         make_id<domain::WorkspaceId>("chat"),
                         make_id<domain::PermissionProfileId>("observe"),
                         std::nullopt},
      domain::Message{make_id<domain::MessageId>("user"),
                      domain::Role::user,
                      {domain::TextBlock{"hello"}},
                      std::nullopt},
      std::move(backend_request)};
}

auto pricing_observation() -> domain::PricingObservation {
  domain::TextPricing pricing;
  pricing.base.input = domain::PriceRate{
      domain::DecimalAmount::from("1.42").value(), std::nullopt};
  return domain::make_pricing_observation(
             make_id<domain::ModelId>("model"), "test.models", std::nullopt,
             domain::EventTimestamp{1ms}, domain::PricingCatalogOrigin::live,
             std::move(pricing))
      .value();
}

auto step(backend::BackendEvent event) -> testing::ScriptedStep {
  return testing::ScriptedStep{std::move(event)};
}

auto tool_step(runtime::ToolExecutionEvent event) -> testing::ScriptedToolStep {
  return testing::ScriptedToolStep{std::move(event)};
}

class WakeCounter final : public runtime::RunWakeSink {
 public:
  auto wake() noexcept -> void override {
    {
      std::lock_guard lock(m_mutex);
      ++m_count;
    }
    m_changed.notify_all();
  }

  auto wait_for_change(const std::size_t previous) -> void {
    std::unique_lock lock(m_mutex);
    static_cast<void>(
        m_changed.wait_for(lock, 1s, [&] { return m_count > previous; }));
  }

  auto count() -> std::size_t {
    std::lock_guard lock(m_mutex);
    return m_count;
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::size_t m_count{};
};

auto drain_to_inference_boundary(runtime::RunKernel& kernel, WakeCounter& wake)
    -> void {
  std::size_t observed{};
  for (int attempt = 0; attempt < 100 && kernel.active_inference_id();
       ++attempt) {
    REQUIRE(kernel.drain());
    if (kernel.active_inference_id()) wake.wait_for_change(observed);
    observed = wake.count();
  }
  REQUIRE_FALSE(kernel.active_inference_id());
  REQUIRE(kernel.active_run_id());
}

auto drain_to_run_end(runtime::RunKernel& kernel, WakeCounter& wake) -> void {
  std::size_t observed{};
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    REQUIRE(kernel.drain());
    if (kernel.active_run_id()) wake.wait_for_change(observed);
    observed = wake.count();
  }
  REQUIRE_FALSE(kernel.active_run_id());
}

class RejectingExecutor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock&) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::invalid_arguments,
        "secret-bearing parser detail", false});
  }

  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::internal_failure, "must not start",
        false});
  }
};

class BlockingStream final : public runtime::ToolExecutionStream {
 public:
  auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any changed;
    changed.wait(lock, stop_token, [] { return false; });
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::cancelled, "deadline detail", false});
  }
};

class BlockingExecutor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{arguments};
  }

  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return std::make_unique<BlockingStream>();
  }
};

class CountingExecutor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    ++validations;
    return runtime::ValidatedToolArguments{arguments};
  }

  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    ++starts;
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::unavailable, "unused", false});
  }

  mutable std::size_t validations{};
  std::size_t starts{};
};

class ImmediateStream final : public runtime::ToolExecutionStream {
 public:
  explicit ImmediateStream(runtime::ToolResult result)
      : m_result(std::move(result)) {}

  auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(runtime::ToolExecutionError{
          runtime::ToolExecutionErrorCode::cancelled,
          "immediate tool cancelled", false});
    }
    if (!m_result) return std::optional<runtime::ToolExecutionEvent>{};
    auto result = std::move(*m_result);
    m_result.reset();
    return std::optional<runtime::ToolExecutionEvent>{std::move(result)};
  }

 private:
  std::optional<runtime::ToolResult> m_result;
};

class ImmediateExecutor final : public runtime::ToolExecutor {
 public:
  ImmediateExecutor(std::vector<domain::CapabilityScope> scopes,
                    std::vector<domain::Effect> effects,
                    runtime::ToolResult result)
      : m_scopes(std::move(scopes)), m_effects(std::move(effects)),
        m_result(std::move(result)) {}

  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{arguments, m_scopes, m_effects};
  }

  auto start(runtime::ToolInvocation invocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    m_invocations.push_back(std::move(invocation));
    return std::make_unique<ImmediateStream>(m_result);
  }

  [[nodiscard]] auto invocations() const
      -> const std::vector<runtime::ToolInvocation>& {
    return m_invocations;
  }

 private:
  std::vector<domain::CapabilityScope> m_scopes;
  std::vector<domain::Effect> m_effects;
  runtime::ToolResult m_result;
  std::vector<runtime::ToolInvocation> m_invocations;
};

class RecordingPolicy final : public runtime::ToolPolicy {
 public:
  auto evaluate(const runtime::ToolPolicyRequest& request)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    requests.push_back(request);
    return runtime::ToolPolicyResolution{
        domain::PolicyDecision::allow, request.scopes, "allowed by test",
        domain::PolicyDecisionSource::permission_profile};
  }

  auto approve(const runtime::ToolPolicyRequest&, runtime::ToolPolicyApproval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    return std::unexpected(
        runtime::ToolPolicyError{runtime::ToolPolicyErrorCode::invalid_request,
                                 "approval is not expected", false});
  }

  std::vector<runtime::ToolPolicyRequest> requests;
};

template <typename Payload>
auto persisted_event(const std::uint64_t sequence, Payload payload,
                     std::optional<domain::InvocationId> invocation_id =
                         std::nullopt) -> domain::RunEvent {
  return {{make_id<domain::EventId>("event-" + std::to_string(sequence)),
           make_id<domain::RunId>("run"), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::move(invocation_id)},
          std::move(payload)};
}

auto tool_call_script(const domain::InvocationId& invocation)
    -> testing::StreamScript {
  return testing::StreamScript{{
      step(backend::ResponseStarted{"response"}),
      step(backend::ToolCallDelta{invocation, "lookup", "{}"}),
      step(backend::ResponseFinished{domain::FinishReason::tool_call}),
      testing::EndOfStream{},
  }};
}

auto snapshot_of(const runtime::ToolRegistry& registry)
    -> runtime::ToolRegistrySnapshot {
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  return std::move(*snapshot);
}

auto scope() -> domain::CapabilityScope {
  return {domain::Effect::read, "filesystem.root", "/repo"};
}

auto allow_policy() -> std::shared_ptr<runtime::ToolPolicy> {
  return std::make_shared<runtime::CapabilityPolicy>(runtime::PermissionProfile{
      make_id<domain::PermissionProfileId>("observe"),
      {domain::Effect::read},
      {scope()},
      {},
      {}});
}

auto approval_policy() -> std::shared_ptr<runtime::ToolPolicy> {
  return std::make_shared<runtime::CapabilityPolicy>(runtime::PermissionProfile{
      make_id<domain::PermissionProfileId>("observe"),
      {},
      {},
      {domain::Effect::read},
      {scope()}});
}

class OrderedPolicy final : public runtime::ToolPolicy {
 public:
  auto evaluate(const runtime::ToolPolicyRequest& request)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    if (request.tool_name == "first") {
      return runtime::ToolPolicyResolution{
          domain::PolicyDecision::deny,
          {},
          "first is denied",
          domain::PolicyDecisionSource::permission_profile};
    }
    return runtime::ToolPolicyResolution{
        domain::PolicyDecision::allow, request.scopes, "second is allowed",
        domain::PolicyDecisionSource::permission_profile};
  }

  auto approve(const runtime::ToolPolicyRequest&, runtime::ToolPolicyApproval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    return std::unexpected(
        runtime::ToolPolicyError{runtime::ToolPolicyErrorCode::invalid_request,
                                 "approval is not expected", false});
  }
};

} // namespace

TEST_CASE(
    "tool registry rejects malformed declarations and snapshots are stable",
    "[tools][registry][failure]") {
  runtime::ToolRegistry registry;
  auto executor = std::make_shared<RejectingExecutor>();

  auto invalid = declaration();
  invalid.name = "bad\nname";
  auto result = registry.register_tool(invalid, executor);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolRegistryErrorCode::invalid_declaration);

  invalid = declaration();
  invalid.input_schema.data = "{";
  result = registry.register_tool(invalid, executor);
  REQUIRE_FALSE(result);

  invalid = declaration();
  invalid.capability_scopes.front().effect = domain::Effect::write;
  result = registry.register_tool(invalid, executor);
  REQUIRE_FALSE(result);

  REQUIRE(registry.register_tool(declaration(), executor));
  const auto snapshot = snapshot_of(registry);
  REQUIRE(snapshot.size() == 1);
  REQUIRE(snapshot.declarations() ==
          std::vector<backend::ToolDeclaration>{declaration()});

  result = registry.register_tool(declaration(), executor);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolRegistryErrorCode::duplicate_name);
  REQUIRE(registry.register_tool(declaration("second"), executor));
  REQUIRE(snapshot.size() == 1);
  REQUIRE(snapshot_of(registry).size() == 2);
}

TEST_CASE("allowed tool results continue the same run in registry order",
          "[tools][runtime]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  const domain::CapabilityScope scope{domain::Effect::read, "filesystem.root",
                                      "/repo"};
  const runtime::ToolExecutionLimits limits{4096, 8, 1s};
  const auto tool = declaration();
  auto expected_invocation = runtime::ToolInvocation{
      invocation, std::nullopt,
      "lookup",   runtime::ValidatedToolArguments{{"application/json", "{}"}},
      {scope},    limits};
  auto executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{
          {expected_invocation,
           testing::ToolStreamScript{{
               tool_step(runtime::ToolProgress{{domain::TextBlock{"working"}}}),
               tool_step(runtime::ToolResult{{domain::TextBlock{"done"}}}),
               testing::ToolEndOfStream{},
           }}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(tool, executor, limits));
  const auto snapshot = snapshot_of(registry);

  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  const auto tool_message =
      domain::Message{make_id<domain::MessageId>("tool-message-7"),
                      domain::Role::tool,
                      {domain::TextBlock{"done"}},
                      invocation};
  auto continuation = request("inference-2", "assistant-2",
                              snapshot.declarations(), {tool_message});
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
      {continuation,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response-2"}),
           step(backend::ContentDelta{continuation.assistant_message_id,
                                      domain::TextBlock{"final"}}),
           step(backend::ResponseFinished{domain::FinishReason::stop}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            allow_policy()};

  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  REQUIRE(kernel.start(std::move(start)));
  drain_to_inference_boundary(kernel, wake);
  bool continued{};
  std::size_t observed{};
  for (int attempt = 0; attempt < 100 && !continued; ++attempt) {
    auto next = kernel.continue_run(make_id<domain::RunId>("run"), continuation,
                                    pricing_observation());
    if (next) {
      continued = true;
      break;
    }
    REQUIRE(next.error().code ==
            runtime::RunKernelErrorCode::continuation_not_ready);
    REQUIRE(kernel.drain());
    wake.wait_for_change(observed);
    observed = wake.count();
  }
  REQUIRE(continued);
  drain_to_run_end(kernel, wake);

  REQUIRE(executor->recorded_invocations() ==
          std::vector<runtime::ToolInvocation>{expected_invocation});
  const auto messages =
      runtime::tool_result_messages(kernel.event_log().events());
  REQUIRE(messages);
  REQUIRE(*messages == std::vector<domain::Message>{tool_message});
  const auto& events = kernel.event_log().events();
  const auto policy = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolPolicyDecided>(event.payload);
  });
  const auto started = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  });
  const auto result = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolResultRecorded>(event.payload);
  });
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::InferencePricingObserved>(
                event.payload);
          }) == 2);
  REQUIRE(policy < started);
  REQUIRE(started < result);
  REQUIRE(kernel.projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::completed);
}

TEST_CASE("policy and approval decisions are one-shot and cannot widen scope",
          "[tools][policy][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  const domain::CapabilityScope scope{domain::Effect::read, "filesystem.root",
                                      "/repo"};
  const auto tool = declaration();
  auto executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(tool, executor));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            approval_policy()};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);

  const domain::CapabilityScope widened{domain::Effect::read, "filesystem.root",
                                        "/outside"};
  auto decision =
      kernel.decide_approval(make_id<domain::RunId>("run"), invocation,
                             {domain::ApprovalDecision::approved,
                              {widened},
                              domain::ApprovalGrantLifetime::invocation});
  REQUIRE_FALSE(decision);
  REQUIRE(decision.error().code ==
          runtime::RunKernelErrorCode::policy_scope_widening);

  REQUIRE(kernel.decide_approval(make_id<domain::RunId>("run"), invocation,
                                 {domain::ApprovalDecision::denied, {}}));
  decision = kernel.decide_approval(make_id<domain::RunId>("run"), invocation,
                                    {domain::ApprovalDecision::denied, {}});
  REQUIRE_FALSE(decision);
  REQUIRE(decision.error().code ==
          runtime::RunKernelErrorCode::invalid_tool_state);
  REQUIRE(executor->recorded_invocations().empty());

  const auto messages =
      runtime::tool_result_messages(kernel.event_log().events());
  REQUIRE(messages);
  REQUIRE(messages->size() == 1);
  REQUIRE(std::get<domain::TextBlock>(messages->front().content.front()).text ==
          "tool invocation denied");
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "test cleanup"));
}

TEST_CASE("invalid arguments fail before policy without leaking validator text",
          "[tools][validation][failure][redaction]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(),
                                 std::make_shared<RejectingExecutor>()));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{
      make_id<domain::SessionId>("session"), backend, &wake, {}, {}, snapshot};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);

  const auto messages =
      runtime::tool_result_messages(kernel.event_log().events());
  REQUIRE(messages);
  REQUIRE(messages->size() == 1);
  const auto text =
      std::get<domain::TextBlock>(messages->front().content.front()).text;
  REQUIRE(text == "tool arguments are invalid");
  REQUIRE(text.find("secret-bearing") == std::string::npos);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "test cleanup"));
}

TEST_CASE("saved approval failure is durable and does not start the tool",
          "[tools][policy][storage][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(), executor));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  testing::ScriptedPolicyGrantStore grants;
  grants.fail_save({storage::PolicyGrantStoreErrorCode::permission_denied,
                    "secret-bearing storage detail", false});
  auto policy = std::make_shared<runtime::CapabilityPolicy>(
      runtime::PermissionProfile{
          make_id<domain::PermissionProfileId>("observe"),
          {},
          {},
          {domain::Effect::read},
          {scope()}},
      &grants);
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            policy};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);

  auto result =
      kernel.decide_approval(make_id<domain::RunId>("run"), invocation,
                             {domain::ApprovalDecision::approved,
                              {scope()},
                              domain::ApprovalGrantLifetime::saved});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == runtime::RunKernelErrorCode::policy_failure);
  REQUIRE(executor->starts == 0);
  REQUIRE(grants.saved_grants().empty());
  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ToolPolicyFailed>(
                event.payload);
          }) == 1);
  const auto failure = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolPolicyFailed>(event.payload);
  });
  REQUIRE(failure != events.end());
  REQUIRE(std::get<domain::ToolPolicyFailed>(failure->payload)
              .error.message.find("secret-bearing") == std::string::npos);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("tool output and deadline limits become one bounded terminal error",
          "[tools][limits][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto tool = declaration();

  SECTION("output bytes") {
    const runtime::ToolExecutionLimits limits{3, 2, 1s};
    auto executor = std::make_shared<testing::ScriptedToolExecutor>(
        std::vector<testing::ScriptedToolExchange>{
            {runtime::ToolInvocation{
                 invocation,
                 std::nullopt,
                 "lookup",
                 runtime::ValidatedToolArguments{{"application/json", "{}"}},
                 {scope()},
                 limits},
             testing::ToolStreamScript{{
                 tool_step(runtime::ToolResult{{domain::TextBlock{"four"}}}),
                 testing::ToolEndOfStream{},
             }}}});
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(tool, executor, limits));
    const auto snapshot = snapshot_of(registry);
    auto initial =
        request("inference-1", "assistant-1", snapshot.declarations());
    testing::ScriptedBackend backend{{
        {initial, tool_call_script(invocation)},
    }};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                              backend,
                              &wake,
                              {},
                              {},
                              snapshot,
                              allow_policy()};
    REQUIRE(kernel.start(run_start(initial)));
    drain_to_inference_boundary(kernel, wake);
    for (int attempt = 0; attempt < 100; ++attempt) {
      REQUIRE(kernel.drain());
      if (std::ranges::any_of(
              kernel.event_log().events(), [](const auto& event) {
                return std::holds_alternative<domain::ToolErrored>(
                    event.payload);
              })) {
        break;
      }
      wake.wait_for_change(wake.count());
    }
    const auto messages =
        runtime::tool_result_messages(kernel.event_log().events());
    REQUIRE(messages);
    REQUIRE(
        std::get<domain::TextBlock>(messages->front().content.front()).text ==
        "tool output limit exceeded");
    REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
  }

  SECTION("executor failure") {
    const runtime::ToolExecutionLimits limits{128, 2, 1s};
    auto executor = std::make_shared<testing::ScriptedToolExecutor>(
        std::vector<testing::ScriptedToolExchange>{
            {runtime::ToolInvocation{
                 invocation,
                 std::nullopt,
                 "lookup",
                 runtime::ValidatedToolArguments{{"application/json", "{}"}},
                 {scope()},
                 limits},
             runtime::ToolExecutionError{
                 runtime::ToolExecutionErrorCode::unavailable,
                 "secret-bearing executor detail", true}}});
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(tool, executor, limits));
    const auto snapshot = snapshot_of(registry);
    auto initial =
        request("inference-1", "assistant-1", snapshot.declarations());
    testing::ScriptedBackend backend{{
        {initial, tool_call_script(invocation)},
    }};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                              backend,
                              &wake,
                              {},
                              {},
                              snapshot,
                              allow_policy()};
    REQUIRE(kernel.start(run_start(initial)));
    drain_to_inference_boundary(kernel, wake);
    for (int attempt = 0; attempt < 100; ++attempt) {
      REQUIRE(kernel.drain());
      if (std::ranges::any_of(
              kernel.event_log().events(), [](const auto& event) {
                return std::holds_alternative<domain::ToolErrored>(
                    event.payload);
              })) {
        break;
      }
      wake.wait_for_change(wake.count());
    }
    const auto messages =
        runtime::tool_result_messages(kernel.event_log().events());
    REQUIRE(messages);
    REQUIRE(messages->size() == 1);
    const auto text =
        std::get<domain::TextBlock>(messages->front().content.front()).text;
    REQUIRE(text == "tool executor unavailable");
    REQUIRE(text.find("secret-bearing") == std::string::npos);
    REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
    drain_to_run_end(kernel, wake);
  }

  SECTION("deadline") {
    const runtime::ToolExecutionLimits limits{128, 2, 20ms};
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(tool, std::make_shared<BlockingExecutor>(),
                                   limits));
    const auto snapshot = snapshot_of(registry);
    auto initial =
        request("inference-1", "assistant-1", snapshot.declarations());
    testing::ScriptedBackend backend{{
        {initial, tool_call_script(invocation)},
    }};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                              backend,
                              &wake,
                              {},
                              {},
                              snapshot,
                              allow_policy()};
    REQUIRE(kernel.start(run_start(initial)));
    drain_to_inference_boundary(kernel, wake);
    for (int attempt = 0; attempt < 100; ++attempt) {
      REQUIRE(kernel.drain());
      if (std::ranges::any_of(
              kernel.event_log().events(), [](const auto& event) {
                const auto* failed =
                    std::get_if<domain::ToolErrored>(&event.payload);
                return failed != nullptr &&
                       failed->error.message == "tool execution timed out";
              })) {
        break;
      }
      wake.wait_for_change(wake.count());
    }
    const auto messages =
        runtime::tool_result_messages(kernel.event_log().events());
    REQUIRE(messages);
    REQUIRE(
        std::get<domain::TextBlock>(messages->front().content.front()).text ==
        "tool execution timed out");
    REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
  }
}

TEST_CASE("durable replay rebuilds tool state without validation or execution",
          "[tools][replay][failure]") {
  const auto session = make_id<domain::SessionId>("session");
  const auto run = make_id<domain::RunId>("run");
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto assistant = make_id<domain::MessageId>("assistant");
  const auto result_message = make_id<domain::MessageId>("tool-message");
  const auto tool = declaration();
  const domain::CapabilityScope scope{domain::Effect::read, "filesystem.root",
                                      "/repo"};
  const std::vector<domain::RunEvent> events{
      persisted_event(
          1, domain::RunStarted{make_id<domain::SurfaceId>("test"),
                                make_id<domain::WorkspaceId>("chat"),
                                make_id<domain::PermissionProfileId>("observe"),
                                std::nullopt}),
      persisted_event(2, domain::UserContentAdded{domain::Message{
                             make_id<domain::MessageId>("user"),
                             domain::Role::user,
                             {domain::TextBlock{"hello"}},
                             std::nullopt}}),
      persisted_event(3, domain::RunCompletionRequested{}),
      persisted_event(
          4, domain::InferenceStarted{inference,
                                      make_id<domain::ModelId>("model")}),
      persisted_event(5, domain::AssistantContentStarted{assistant, inference}),
      persisted_event(6,
                      domain::ToolProposed{invocation,
                                           "lookup",
                                           {"application/json", "{}"},
                                           {domain::Effect::read},
                                           std::nullopt},
                      invocation),
      persisted_event(7,
                      domain::AssistantContentFinished{assistant, inference}),
      persisted_event(
          8, domain::InferenceFinished{inference,
                                       domain::FinishReason::tool_call}),
      persisted_event(
          9,
          domain::ToolPolicyDecided{
              invocation, domain::PolicyDecision::allow, {scope}, std::nullopt},
          invocation),
      persisted_event(10, domain::ToolStarted{invocation}, invocation),
      persisted_event(11,
                      domain::ToolResultRecorded{invocation,
                                                 {domain::TextBlock{"done"}},
                                                 result_message},
                      invocation),
  };
  testing::ScriptedSessionStore store{{
      {testing::OpenSessionCall{session},
       storage::SessionInfo{session, domain::EventTimestamp{1ms},
                            domain::EventTimestamp{11ms}, 11}},
      {testing::ReplayEventsCall{session}, events},
  }};
  testing::ScriptedBackend backend{{}};
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(tool, executor));

  auto kernel = runtime::RunKernel::open_durable(
      {session, runtime::DurableSessionMode::resume,
       domain::EventTimestamp{1ms}},
      store, backend, nullptr, {}, {}, snapshot_of(registry));
  REQUIRE(kernel);
  REQUIRE(executor->validations == 0);
  REQUIRE(executor->starts == 0);
  REQUIRE((*kernel)->projection(run) != nullptr);
  REQUIRE((*kernel)->projection(run)->status() == domain::RunStatus::running);
  REQUIRE_FALSE((*kernel)->active_run_id());
  REQUIRE(store.remaining_exchanges() == 0);
}

TEST_CASE("durable replay rejects duplicate invocation identity",
          "[tools][replay][failure]") {
  const auto session = make_id<domain::SessionId>("session");
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto run_started = domain::RunStarted{
      make_id<domain::SurfaceId>("test"), make_id<domain::WorkspaceId>("chat"),
      make_id<domain::PermissionProfileId>("observe"), std::nullopt};
  const auto proposed = domain::ToolProposed{invocation,
                                             "lookup",
                                             {"application/json", "{}"},
                                             {domain::Effect::read},
                                             std::nullopt};
  const std::vector<domain::RunEvent> events{
      persisted_event(1, run_started),
      persisted_event(2, proposed, invocation),
      persisted_event(3, proposed, invocation),
  };
  testing::ScriptedSessionStore store{{
      {testing::OpenSessionCall{session},
       storage::SessionInfo{session, domain::EventTimestamp{1ms},
                            domain::EventTimestamp{3ms}, 3}},
      {testing::ReplayEventsCall{session}, events},
  }};
  testing::ScriptedBackend backend{{}};

  auto kernel = runtime::RunKernel::open_durable(
      {session, runtime::DurableSessionMode::resume,
       domain::EventTimestamp{1ms}},
      store, backend);
  REQUIRE_FALSE(kernel);
  REQUIRE(kernel.error().code == runtime::RunKernelErrorCode::replay_rejected);
  REQUIRE(store.remaining_exchanges() == 0);
}

TEST_CASE("run cancellation before execution never starts the executor",
          "[tools][cancellation][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(), executor));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            approval_policy()};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);

  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "user cancelled"));
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 0);
  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ToolErrored>(event.payload);
          }) == 1);
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::RunCancelled>(event.payload);
          }) == 1);
}

TEST_CASE("run cancellation terminates tool work exactly once",
          "[tools][cancellation][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  const runtime::ToolExecutionLimits limits{128, 2, 1s};
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(),
                                 std::make_shared<BlockingExecutor>(), limits));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            allow_policy()};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "user cancelled"));
  drain_to_run_end(kernel, wake);

  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ToolErrored>(event.payload);
          }) == 1);
  REQUIRE(std::ranges::none_of(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolResultRecorded>(event.payload);
  }));
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::RunCancelled>(event.payload);
          }) == 1);
}

TEST_CASE("multiple tool calls cannot execute ahead of provider order",
          "[tools][ordering][failure]") {
  const auto first = make_id<domain::InvocationId>("first-call");
  const auto second = make_id<domain::InvocationId>("second-call");
  const runtime::ToolExecutionLimits limits{128, 2, 1s};
  auto executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{
          {runtime::ToolInvocation{
               second,
               std::nullopt,
               "second",
               runtime::ValidatedToolArguments{{"application/json", "{}"}},
               {scope()},
               limits},
           testing::ToolStreamScript{{
               tool_step(runtime::ToolResult{{domain::TextBlock{"done"}}}),
               testing::ToolEndOfStream{},
           }}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration("first"), executor, limits));
  REQUIRE(registry.register_tool(declaration("second"), executor, limits));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::ToolCallDelta{first, "first", "{}"}),
           step(backend::ToolCallDelta{second, "second", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{
      make_id<domain::SessionId>("session"), backend, &wake, {}, {}, snapshot,
      std::make_shared<OrderedPolicy>()};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);

  for (int attempt = 0; attempt < 100; ++attempt) {
    REQUIRE(kernel.drain());
    if (std::ranges::any_of(kernel.event_log().events(), [](const auto& event) {
          return std::holds_alternative<domain::ToolResultRecorded>(
              event.payload);
        })) {
      break;
    }
    wake.wait_for_change(wake.count());
  }
  REQUIRE(executor->recorded_invocations().size() == 1);
  REQUIRE(executor->recorded_invocations().front().invocation_id == second);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("argument validation can narrow a declaration's effects",
          "[tools][policy][effects]") {
  const auto invocation = make_id<domain::InvocationId>("narrow-call");
  const auto read_scope = scope();
  const domain::CapabilityScope write_scope{domain::Effect::write,
                                            "filesystem.root", "/repo"};
  auto tool = declaration("narrow");
  tool.effects.push_back(domain::Effect::write);
  tool.capability_scopes.push_back(write_scope);
  auto executor = std::make_shared<ImmediateExecutor>(
      std::vector<domain::CapabilityScope>{read_scope},
      std::vector<domain::Effect>{domain::Effect::read},
      runtime::ToolResult{{domain::TextBlock{"done"}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(tool, executor,
                                 runtime::ToolExecutionLimits{128, 2, 1s}));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::ToolCallDelta{invocation, "narrow", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  auto policy = std::make_shared<RecordingPolicy>();
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            policy};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);
  for (int attempt = 0; attempt < 100 && executor->invocations().empty();
       ++attempt) {
    REQUIRE(kernel.drain());
    if (executor->invocations().empty()) wake.wait_for_change(wake.count());
  }
  REQUIRE(policy->requests.size() == 1);
  REQUIRE(policy->requests.front().effects ==
          std::vector<domain::Effect>{domain::Effect::read});
  REQUIRE(policy->requests.front().scopes ==
          std::vector<domain::CapabilityScope>{read_scope});
  const auto proposed =
      std::ranges::find_if(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::ToolProposed>(event.payload);
      });
  REQUIRE(proposed != kernel.event_log().events().end());
  REQUIRE(std::get<domain::ToolProposed>(proposed->payload).declared_effects ==
          std::vector<domain::Effect>{domain::Effect::read});
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("argument validation cannot widen a declaration's effects",
          "[tools][policy][effects][failure]") {
  const auto invocation = make_id<domain::InvocationId>("widen-call");
  const domain::CapabilityScope write_scope{domain::Effect::write,
                                            "filesystem.root", "/repo"};
  auto executor = std::make_shared<ImmediateExecutor>(
      std::vector<domain::CapabilityScope>{write_scope},
      std::vector<domain::Effect>{domain::Effect::write},
      runtime::ToolResult{{domain::TextBlock{"must not run"}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(), executor,
                                 runtime::ToolExecutionLimits{128, 2, 1s}));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  auto policy = std::make_shared<RecordingPolicy>();
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            policy};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);
  REQUIRE(policy->requests.empty());
  REQUIRE(executor->invocations().empty());
  REQUIRE(
      std::ranges::count_if(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::ToolErrored>(event.payload);
      }) == 1);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("tool-created artifacts are recorded before their terminal result",
          "[tools][artifact][events]") {
  const auto invocation = make_id<domain::InvocationId>("artifact-call");
  const auto artifact = make_id<domain::ArtifactId>("artifact-output");
  const domain::ArtifactMetadata metadata{
      artifact,    "application/octet-stream",
      4,           "sha256:test",
      invocation,  std::nullopt,
      std::nullopt};
  auto executor = std::make_shared<ImmediateExecutor>(
      std::vector<domain::CapabilityScope>{}, std::vector<domain::Effect>{},
      runtime::ToolResult{
          {domain::StructuredDataBlock{"application/json", "{}"},
           domain::ArtifactReferenceBlock{artifact, std::string{"stdout"}}},
          {metadata}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(), executor,
                                 runtime::ToolExecutionLimits{1024, 2, 1s}));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            allow_policy()};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);
  for (int attempt = 0; attempt < 100; ++attempt) {
    REQUIRE(kernel.drain());
    if (std::ranges::any_of(kernel.event_log().events(), [](const auto& event) {
          return std::holds_alternative<domain::ToolResultRecorded>(
              event.payload);
        })) {
      break;
    }
    wake.wait_for_change(wake.count());
  }

  std::vector<std::size_t> sequences;
  for (const auto& event : kernel.event_log().events()) {
    if (std::holds_alternative<domain::ArtifactCreated>(event.payload) ||
        std::holds_alternative<domain::ArtifactReferenced>(event.payload) ||
        std::holds_alternative<domain::ToolResultRecorded>(event.payload)) {
      sequences.push_back(event.metadata.sequence);
    }
  }
  REQUIRE(sequences.size() == 3);
  REQUIRE(sequences[1] == sequences[0] + 1);
  REQUIRE(sequences[2] == sequences[1] + 1);
  const auto messages =
      runtime::tool_result_messages(kernel.event_log().events());
  REQUIRE(messages);
  REQUIRE(messages->size() == 1);
  REQUIRE(
      std::ranges::any_of(messages->front().content, [&](const auto& block) {
        const auto* reference =
            std::get_if<domain::ArtifactReferenceBlock>(&block);
        return reference != nullptr && reference->artifact_id == artifact;
      }));
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("invalid tool artifact metadata fails the run without a reference",
          "[tools][artifact][protocol][failure]") {
  const auto invocation = make_id<domain::InvocationId>("artifact-call");
  const auto artifact = make_id<domain::ArtifactId>("artifact-output");
  auto executor = std::make_shared<ImmediateExecutor>(
      std::vector<domain::CapabilityScope>{}, std::vector<domain::Effect>{},
      runtime::ToolResult{
          {domain::ArtifactReferenceBlock{artifact, std::string{"stdout"}}},
          {domain::ArtifactMetadata{artifact, "application/octet-stream", 4,
                                    "sha256:test",
                                    make_id<domain::InvocationId>("wrong-call"),
                                    std::nullopt, std::nullopt}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(), executor,
                                 runtime::ToolExecutionLimits{1024, 2, 1s}));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            snapshot,
                            allow_policy()};
  REQUIRE(kernel.start(run_start(initial)));
  drain_to_inference_boundary(kernel, wake);
  drain_to_run_end(kernel, wake);
  REQUIRE(
      std::ranges::none_of(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::ArtifactCreated>(event.payload) ||
               std::holds_alternative<domain::ArtifactReferenced>(
                   event.payload);
      }));
  REQUIRE(
      std::ranges::any_of(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::RunFailed>(event.payload);
      }));
}
