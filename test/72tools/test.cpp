#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/testing/application_launch_context.hpp>
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
          {0.25, 128, 42, {}, {}}};
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

auto provenance() -> domain::RunProvenance {
  return {"test-version",
          "test-backend",
          std::nullopt,
          make_id<domain::ModelId>("model"),
          std::nullopt,
          {},
          {},
          {}};
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

auto spend_digest(const char fill = 'a') -> domain::ContentDigest {
  return {"sha256", std::string(64, fill), 32};
}

auto spend_expiry() -> domain::EventTimestamp {
  return domain::EventTimestamp::max();
}

auto usd(const std::string& value) -> domain::MonetaryAmount {
  return domain::MonetaryAmount::create(
             "USD", domain::DecimalAmount::from(value).value())
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
    const auto drained = kernel.drain();
    std::string failure_message;
    if (!drained) failure_message = drained.error().message;
    INFO(failure_message);
    REQUIRE(drained);
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

class PaidBlockingExecutor final : public runtime::ToolExecutor {
 public:
  explicit PaidBlockingExecutor(domain::ToolSpendQuote quote)
      : m_quote(std::move(quote)) {}

  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{
        arguments, {}, {domain::Effect::spend}, m_quote};
  }

  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return std::make_unique<BlockingStream>();
  }

 private:
  domain::ToolSpendQuote m_quote;
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

class MemoryStore final : public storage::SessionStore {
 public:
  domain::SessionId session_id{make_id<domain::SessionId>("session")};
  domain::EventTimestamp created{std::chrono::milliseconds{1}};
  std::vector<domain::RunEvent> events;
  bool fail_next_paid_terminal_append{};
  bool commit_failed_paid_terminal_append{};
  std::size_t paid_terminal_failures{};
  bool fail_next_tool_started_append{};

  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    session_id = std::move(session.session_id);
    created = session.created_at;
    return {};
  }

  auto open_session(const domain::SessionId& requested, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    if (requested != session_id) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    const auto last =
        events.empty() ? created : events.back().metadata.timestamp;
    return storage::SessionInfo{
        session_id, created, last,
        events.empty() ? 0 : events.back().metadata.sequence};
  }

  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }

  auto append_events(const domain::SessionId& requested,
                     std::span<const domain::RunEvent> additions,
                     std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (requested != session_id) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    const bool paid_terminal = std::ranges::any_of(additions, [](const auto&
                                                                     event) {
      return std::holds_alternative<domain::ToolSpendReleased>(event.payload) ||
             std::holds_alternative<domain::ToolSpendFinalized>(
                 event.payload) ||
             std::holds_alternative<domain::ToolSpendReconciliationRequired>(
                 event.payload);
    });
    if (fail_next_paid_terminal_append && paid_terminal &&
        paid_terminal_failures == 0) {
      ++paid_terminal_failures;
      if (commit_failed_paid_terminal_append) {
        events.insert(events.end(), additions.begin(), additions.end());
      }
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::io_failure,
          "simulated paid terminal append failure", true});
    }
    if (fail_next_tool_started_append &&
        std::ranges::any_of(additions, [](const auto& event) {
          return std::holds_alternative<domain::ToolStarted>(event.payload);
        })) {
      fail_next_tool_started_append = false;
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::io_failure,
          "simulated tool start append failure", true});
    }
    events.insert(events.end(), additions.begin(), additions.end());
    return {};
  }

  auto replay_events(const domain::SessionId&, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    return events;
  }
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

class PaidExecutor final : public runtime::ToolExecutor {
 public:
  PaidExecutor(domain::ToolSpendQuote quote, runtime::ToolResult result,
               std::optional<domain::StructuredDataBlock> normalized_arguments =
                   std::nullopt)
      : m_quote(std::move(quote)), m_result(std::move(result)),
        m_normalized_arguments(std::move(normalized_arguments)) {}

  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    ++validations;
    return runtime::ValidatedToolArguments{
        m_normalized_arguments.value_or(arguments),
        {},
        {domain::Effect::spend},
        m_quote};
  }

  auto start(runtime::ToolInvocation invocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    ++starts;
    invocations.push_back(std::move(invocation));
    return std::make_unique<ImmediateStream>(m_result);
  }

  mutable std::size_t validations{};
  std::size_t starts{};
  std::vector<runtime::ToolInvocation> invocations;

 private:
  domain::ToolSpendQuote m_quote;
  runtime::ToolResult m_result;
  std::optional<domain::StructuredDataBlock> m_normalized_arguments;
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

class CountingPolicy final : public runtime::ToolPolicy {
 public:
  explicit CountingPolicy(std::shared_ptr<runtime::ToolPolicy> delegate)
      : m_delegate(std::move(delegate)) {}

  auto evaluate(const runtime::ToolPolicyRequest& request)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++evaluations;
    requests.push_back(request);
    return m_delegate->evaluate(request);
  }

  auto approve(const runtime::ToolPolicyRequest& request,
               runtime::ToolPolicyApproval approval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    return m_delegate->approve(request, std::move(approval));
  }

  [[nodiscard]] auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* override {
    return m_delegate->provenance();
  }

  [[nodiscard]] auto selected_restriction() const noexcept
      -> std::optional<runtime::RestrictionLevel> override {
    return m_delegate->selected_restriction();
  }

  std::size_t evaluations{};
  std::vector<runtime::ToolPolicyRequest> requests;

 private:
  std::shared_ptr<runtime::ToolPolicy> m_delegate;
};

class CorruptingAutomaticPolicy final : public runtime::ToolPolicy {
 public:
  explicit CorruptingAutomaticPolicy(
      std::shared_ptr<runtime::ToolPolicy> delegate)
      : m_delegate(std::move(delegate)) {}

  auto evaluate(const runtime::ToolPolicyRequest& request)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    auto resolution = m_delegate->evaluate(request);
    if (resolution && resolution->automatic_approval) {
      resolution->automatic_approval->rule_identity = "SecretLikeRuleToken456";
    }
    return resolution;
  }

  auto approve(const runtime::ToolPolicyRequest& request,
               runtime::ToolPolicyApproval approval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    return m_delegate->approve(request, std::move(approval));
  }

  [[nodiscard]] auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* override {
    return m_delegate->provenance();
  }

  [[nodiscard]] auto selected_restriction() const noexcept
      -> std::optional<runtime::RestrictionLevel> override {
    return m_delegate->selected_restriction();
  }

 private:
  std::shared_ptr<runtime::ToolPolicy> m_delegate;
};

enum class AutomaticReplayScenario {
  changed_policy,
  exhausted_matcher,
  expired_matcher,
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

auto tool_call_script(const domain::InvocationId& invocation,
                      std::string tool_name = "lookup",
                      std::string arguments = "{}") -> testing::StreamScript {
  return testing::StreamScript{{
      step(backend::ResponseStarted{"response"}),
      step(backend::ToolCallDelta{invocation, std::move(tool_name),
                                  std::move(arguments)}),
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

auto paid_declaration() -> backend::ToolDeclaration {
  return {"generate_image",
          "Generate a bounded image",
          {"application/schema+json", R"({"type":"object"})"},
          {domain::Effect::spend},
          {{domain::Effect::spend, "spend.microunits", "1000000"}}};
}

auto paid_scope() -> domain::CapabilityScope {
  return {domain::Effect::spend, "spend.microunits", "1000000"};
}

auto paid_policy() -> std::shared_ptr<runtime::ToolPolicy> {
  return std::make_shared<runtime::CapabilityPolicy>(runtime::PermissionProfile{
      make_id<domain::PermissionProfileId>("observe"),
      {domain::Effect::spend},
      {paid_scope()},
      {},
      {}});
}

auto paid_approval_policy() -> std::shared_ptr<runtime::ToolPolicy> {
  return std::make_shared<runtime::CapabilityPolicy>(runtime::PermissionProfile{
      make_id<domain::PermissionProfileId>("observe"),
      {},
      {},
      {domain::Effect::spend},
      {paid_scope()}});
}

auto set_spend_ceiling(runtime::RunKernel& kernel, const std::string& value)
    -> void {
  REQUIRE(kernel.record_session_spend_ceiling(
      {make_id<domain::RunId>("spend-policy"),
       {make_id<domain::SurfaceId>("session-policy"),
        make_id<domain::WorkspaceId>("chat"),
        make_id<domain::PermissionProfileId>("observe"), std::nullopt},
       domain::SessionSpendCeiling::from(value).value(),
       domain::SessionSpendCeilingSource::command_line}));
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

  invalid = declaration();
  invalid.capability_scopes.clear();
  result = registry.register_tool(invalid, executor);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolRegistryErrorCode::invalid_declaration);

  result = registry.register_tool(declaration("invalid-contract"), executor, {},
                                  runtime::ToolExecutorContract{"", "1"});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolRegistryErrorCode::invalid_declaration);

  result = registry.register_tool(declaration("invalid-category"), executor, {},
                                  std::nullopt,
                                  static_cast<runtime::ToolCategory>(100));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolRegistryErrorCode::invalid_declaration);

  REQUIRE(runtime::tool_category_name(runtime::ToolCategory::repository) ==
          "repository");
  REQUIRE(runtime::tool_category_from_name("process") ==
          runtime::ToolCategory::process);
  REQUIRE_FALSE(runtime::tool_category_from_name("unknown"));
  REQUIRE(runtime::all_tool_categories().size() == 6);
  REQUIRE(runtime::all_tool_categories().back() ==
          runtime::ToolCategory::other);

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

TEST_CASE("tool registry subsets are bounded atomic registry-ordered snapshots",
          "[tools][registry][failure]") {
  runtime::ToolRegistry registry;
  auto first_executor = std::make_shared<RejectingExecutor>();
  auto second_executor = std::make_shared<CountingExecutor>();
  const runtime::ToolExecutionLimits first_limits{1024, 2, 1s};
  const runtime::ToolExecutionLimits second_limits{2048, 4, 2s};
  const runtime::ToolExecutorContract first_contract{"test.first", "1"};
  const runtime::ToolExecutorContract second_contract{"test.second", "2"};
  REQUIRE(registry.register_tool(declaration("first"), first_executor,
                                 first_limits, first_contract,
                                 runtime::ToolCategory::repository));
  REQUIRE(registry.register_tool(declaration("second"), second_executor,
                                 second_limits, second_contract,
                                 runtime::ToolCategory::process));
  const auto full = snapshot_of(registry);

  const std::vector<std::string> reverse{"second", "first"};
  const auto selected = full.subset(reverse);
  REQUIRE(selected);
  REQUIRE(selected->declarations() ==
          std::vector<backend::ToolDeclaration>{declaration("first"),
                                                declaration("second")});
  const auto* first = selected->find("first");
  const auto* second = selected->find("second");
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  REQUIRE(first->executor == first_executor);
  REQUIRE(first->limits == first_limits);
  REQUIRE(first->executor_contract == first_contract);
  REQUIRE(first->category == runtime::ToolCategory::repository);
  REQUIRE(second->executor == second_executor);
  REQUIRE(second->limits == second_limits);
  REQUIRE(second->executor_contract == second_contract);
  REQUIRE(second->category == runtime::ToolCategory::process);

  const std::vector<std::string> duplicate{"first", "first"};
  const auto duplicated = full.subset(duplicate);
  REQUIRE_FALSE(duplicated);
  REQUIRE(duplicated.error().code ==
          runtime::ToolRegistryErrorCode::duplicate_name);
  const std::vector<std::string> unknown{"missing"};
  const auto missing = full.subset(unknown);
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          runtime::ToolRegistryErrorCode::invalid_declaration);
  const std::vector<std::string> oversized(257, "first");
  const auto too_many = full.subset(oversized);
  REQUIRE_FALSE(too_many);
  REQUIRE(too_many.error().code ==
          runtime::ToolRegistryErrorCode::invalid_declaration);
  const std::vector<std::string> none;
  const auto empty = full.subset(none);
  REQUIRE(empty);
  REQUIRE(empty->empty());
  REQUIRE(full.size() == 2);
}

TEST_CASE("run tool subsets reject forged unordered and repeated declarations",
          "[tools][runtime][failure]") {
  runtime::ToolRegistry registry;
  auto executor = std::make_shared<CountingExecutor>();
  REQUIRE(registry.register_tool(declaration("first"), executor));
  REQUIRE(registry.register_tool(declaration("second"), executor));
  const auto full = snapshot_of(registry);

  auto accepted = request("inference-1", "assistant-1", {});
  testing::ScriptedBackend backend{{
      {accepted,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
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
                            full,
                            allow_policy()};

  auto reordered = full.declarations();
  std::ranges::reverse(reordered);
  auto result =
      kernel.start(run_start(request("inference-1", "assistant-1", reordered)));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());

  const std::vector<backend::ToolDeclaration> duplicated{
      full.declarations().front(), full.declarations().front()};
  result = kernel.start(
      run_start(request("inference-1", "assistant-1", duplicated)));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());

  const std::vector<backend::ToolDeclaration> unknown{declaration("missing")};
  result =
      kernel.start(run_start(request("inference-1", "assistant-1", unknown)));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());

  auto forged = full.declarations();
  forged.front().description = "Forged declaration";
  result =
      kernel.start(run_start(request("inference-1", "assistant-1", forged)));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());

  REQUIRE(kernel.start(run_start(accepted)));
  drain_to_run_end(kernel, wake);
  REQUIRE(backend.remaining_exchanges() == 0);
}

TEST_CASE("provider cannot call a registered tool outside the run subset",
          "[tools][runtime][failure]") {
  auto hidden_executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(),
                                 std::make_shared<CountingExecutor>()));
  REQUIRE(registry.register_tool(declaration("hidden"), hidden_executor));
  const auto full = snapshot_of(registry);
  const std::vector<std::string> selected_names{"lookup"};
  const auto selected = full.subset(selected_names);
  REQUIRE(selected);
  auto initial =
      request("inference-1", "assistant-1", selected->declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::ToolCallDelta{
               make_id<domain::InvocationId>("hidden-call"), "hidden", "{}"}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            full,
                            allow_policy()};

  REQUIRE(kernel.start(run_start(initial)));
  drain_to_run_end(kernel, wake);
  REQUIRE(hidden_executor->validations == 0);
  REQUIRE(hidden_executor->starts == 0);
  REQUIRE(
      std::ranges::any_of(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::RunFailed>(event.payload);
      }));
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
  REQUIRE(registry.register_tool(declaration("hidden"),
                                 std::make_shared<CountingExecutor>()));
  const auto snapshot = snapshot_of(registry);
  const std::vector<std::string> selected_names{"lookup"};
  const auto selected = snapshot.subset(selected_names);
  REQUIRE(selected);

  auto initial =
      request("inference-1", "assistant-1", selected->declarations());
  const auto tool_message =
      domain::Message{make_id<domain::MessageId>("tool-message-7"),
                      domain::Role::tool,
                      {domain::TextBlock{"done"}},
                      invocation};
  auto continuation = request("inference-2", "assistant-2",
                              selected->declarations(), {tool_message});
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
  const auto continuation_messages =
      runtime::tool_continuation_messages(kernel.event_log().events());
  REQUIRE(continuation_messages);
  REQUIRE(continuation_messages->size() == 2);
  REQUIRE(
      continuation_messages->front() ==
      domain::Message{initial.assistant_message_id,
                      domain::Role::assistant,
                      {},
                      std::nullopt,
                      {{invocation, "lookup", {"application/json", "{}"}}}});
  REQUIRE(continuation_messages->back() == tool_message);
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

TEST_CASE("paid tools reserve before start and finalize before result",
          "[tools][runtime][spend]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto quote = domain::ToolSpendQuote{
      usd("0.3"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), spend_expiry()};
  auto executor = std::make_shared<PaidExecutor>(
      quote, runtime::ToolResult{
                 {domain::TextBlock{"generated"}},
                 {},
                 domain::ToolSpendFinalized{domain::ToolSpendFinalization{
                     invocation, usd("0.2"),
                     domain::ToolSpendFinalizationBasis::catalog_estimate,
                     std::nullopt}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(paid_declaration(), executor));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
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
                            paid_policy()};
  set_spend_ceiling(kernel, "1");
  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  REQUIRE(kernel.start(std::move(start)));
  drain_to_inference_boundary(kernel, wake);
  for (int attempt = 0; attempt < 100; ++attempt) {
    REQUIRE(kernel.drain());
    if (std::ranges::any_of(kernel.event_log().events(), [](const auto& value) {
          return std::holds_alternative<domain::ToolResultRecorded>(
              value.payload);
        })) {
      break;
    }
    wake.wait_for_change(wake.count());
  }

  CHECK(executor->starts == 1);
  const auto& events = kernel.event_log().events();
  const auto reserved = std::ranges::find_if(events, [](const auto& value) {
    return std::holds_alternative<domain::ToolSpendReserved>(value.payload);
  });
  const auto started_event =
      std::ranges::find_if(events, [](const auto& value) {
        return std::holds_alternative<domain::ToolStarted>(value.payload);
      });
  const auto finalized = std::ranges::find_if(events, [](const auto& value) {
    return std::holds_alternative<domain::ToolSpendFinalized>(value.payload);
  });
  const auto result = std::ranges::find_if(events, [](const auto& value) {
    return std::holds_alternative<domain::ToolResultRecorded>(value.payload);
  });
  REQUIRE(reserved != events.end());
  REQUIRE(started_event != events.end());
  REQUIRE(finalized != events.end());
  REQUIRE(result != events.end());
  CHECK(reserved < started_event);
  CHECK(started_event < finalized);
  CHECK(finalized < result);
  CHECK(std::get<domain::ToolSpendReserved>(reserved->payload)
            .reservation.maximum.amount()
            .to_string() == "0.3");
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("invalid paid finalization requires provider-cost reconciliation",
          "[tools][runtime][spend][failure]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto quote = domain::ToolSpendQuote{
      usd("0.3"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), spend_expiry()};
  auto executor = std::make_shared<PaidExecutor>(
      quote, runtime::ToolResult{
                 {domain::TextBlock{"generated"}},
                 {},
                 domain::ToolSpendFinalized{domain::ToolSpendFinalization{
                     invocation, usd("0.4"),
                     domain::ToolSpendFinalizationBasis::catalog_estimate,
                     std::nullopt}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(paid_declaration(), executor));
  const auto tools = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            tools,
                            paid_policy()};
  set_spend_ceiling(kernel, "1");
  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  REQUIRE(kernel.start(std::move(start)));
  drain_to_run_end(kernel, wake);

  CHECK(executor->starts == 1);
  CHECK(
      std::ranges::none_of(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::ToolSpendFinalized>(
            event.payload);
      }));
  CHECK(
      std::ranges::count_if(kernel.event_log().events(), [](const auto& event) {
        const auto* reconciliation =
            std::get_if<domain::ToolSpendReconciliationRequired>(
                &event.payload);
        return reconciliation != nullptr &&
               reconciliation->reason == domain::ToolSpendReconciliationReason::
                                             provider_cost_mismatch;
      }) == 1);
  const auto& events = kernel.event_log().events();
  const auto reconciled = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolSpendReconciliationRequired>(
        event.payload);
  });
  const auto errored = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolErrored>(event.payload);
  });
  const auto failed = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::RunFailed>(event.payload);
  });
  REQUIRE(reconciled != events.end());
  REQUIRE(errored != events.end());
  REQUIRE(failed != events.end());
  CHECK(reconciled < errored);
  CHECK(errored < failed);
}

TEST_CASE("paid terminal append failure is reconciled without re-execution",
          "[tools][runtime][storage][spend][failure]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto quote = domain::ToolSpendQuote{
      usd("0.3"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), spend_expiry()};
  auto executor = std::make_shared<PaidExecutor>(
      quote, runtime::ToolResult{
                 {domain::TextBlock{"generated"}},
                 {},
                 domain::ToolSpendFinalized{domain::ToolSpendFinalization{
                     invocation, usd("0.2"),
                     domain::ToolSpendFinalizationBasis::catalog_estimate,
                     std::nullopt}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      paid_declaration(), executor, {},
      runtime::ToolExecutorContract{"test.generate-image", "1"}));
  const auto tools = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  MemoryStore store;
  bool commit_before_failure{};
  SECTION("failed append did not commit") {
  }
  SECTION("failed append committed atomically") {
    commit_before_failure = true;
    store.commit_failed_paid_terminal_append = true;
  }
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, paid_policy());
  REQUIRE(kernel);
  set_spend_ceiling(**kernel, "1");
  store.fail_next_paid_terminal_append = true;
  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  start.provenance = provenance();
  const auto started = (*kernel)->start(std::move(start));
  std::string start_failure;
  if (!started) start_failure = started.error().message;
  INFO(start_failure);
  REQUIRE(started);
  drain_to_inference_boundary(**kernel, wake);

  std::optional<runtime::RunKernelError> persistence_error;
  for (int attempt = 0;
       attempt < 100 && !persistence_error && store.paid_terminal_failures == 0;
       ++attempt) {
    auto drained = (*kernel)->drain();
    if (!drained) {
      persistence_error = std::move(drained.error());
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  REQUIRE(store.paid_terminal_failures == 1);
  CHECK(executor->starts == 1);
  if (commit_before_failure) {
    CHECK_FALSE(persistence_error);
    CHECK(std::ranges::count_if(store.events, [](const auto& event) {
            return std::holds_alternative<domain::ToolSpendFinalized>(
                event.payload);
          }) == 1);
    CHECK(std::ranges::none_of(store.events, [](const auto& event) {
      return std::holds_alternative<domain::ToolSpendReconciliationRequired>(
          event.payload);
    }));
    return;
  }
  REQUIRE(persistence_error);
  CHECK(persistence_error->code ==
        runtime::RunKernelErrorCode::storage_failure);
  CHECK(std::ranges::count_if(store.events, [](const auto& event) {
          return std::holds_alternative<domain::ToolSpendFinalized>(
              event.payload);
        }) == 0);
  CHECK(std::ranges::count_if(store.events, [](const auto& event) {
          const auto* reconciliation =
              std::get_if<domain::ToolSpendReconciliationRequired>(
                  &event.payload);
          return reconciliation != nullptr &&
                 reconciliation->reason ==
                     domain::ToolSpendReconciliationReason::
                         finalization_persistence_unknown;
        }) == 1);
  CHECK(std::ranges::count_if(store.events, [](const auto& event) {
          return std::holds_alternative<domain::ToolErrored>(event.payload);
        }) == 1);
  CHECK(std::ranges::count_if(store.events, [](const auto& event) {
          return std::holds_alternative<domain::RunFailed>(event.payload);
        }) == 1);
  kernel->reset();

  testing::ScriptedBackend replay_backend{{}};
  auto reopened = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, nullptr, {}, {}, tools, paid_policy());
  REQUIRE(reopened);
  CHECK(executor->starts == 1);
  CHECK(
      std::ranges::count_if(store.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolSpendReconciliationRequired>(
            event.payload);
      }) == 1);
}

TEST_CASE("release and reconciliation append retries preserve durable truth",
          "[tools][runtime][storage][spend][failure]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  std::optional<runtime::ToolSpendDisposition> disposition;
  std::optional<domain::ToolSpendReconciliationReason> expected_reason;
  SECTION("release") {
    disposition = domain::ToolSpendReleased{invocation};
  }
  SECTION("reconciliation") {
    expected_reason =
        domain::ToolSpendReconciliationReason::provider_cost_unavailable;
  }
  const auto quote = domain::ToolSpendQuote{
      usd("0.3"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), spend_expiry()};
  auto executor = std::make_shared<PaidExecutor>(
      quote,
      runtime::ToolResult{{domain::TextBlock{"generated"}}, {}, disposition});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      paid_declaration(), executor, {},
      runtime::ToolExecutorContract{"test.generate-image", "1"}));
  const auto tools = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  MemoryStore store;
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, paid_policy());
  REQUIRE(kernel);
  set_spend_ceiling(**kernel, "1");
  store.fail_next_paid_terminal_append = true;
  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  start.provenance = provenance();
  REQUIRE((*kernel)->start(std::move(start)));
  drain_to_inference_boundary(**kernel, wake);

  for (int attempt = 0; attempt < 100 && store.paid_terminal_failures == 0;
       ++attempt) {
    REQUIRE((*kernel)->drain());
    std::this_thread::sleep_for(10ms);
  }
  REQUIRE(store.paid_terminal_failures == 1);
  CHECK(executor->starts == 1);
  if (expected_reason) {
    CHECK(std::ranges::count_if(store.events, [&](const auto& event) {
            const auto* reconciliation =
                std::get_if<domain::ToolSpendReconciliationRequired>(
                    &event.payload);
            return reconciliation != nullptr &&
                   reconciliation->reason == *expected_reason;
          }) == 1);
  } else {
    CHECK(std::ranges::count_if(store.events, [](const auto& event) {
            return std::holds_alternative<domain::ToolSpendReleased>(
                event.payload);
          }) == 1);
  }
  CHECK(std::ranges::none_of(store.events, [](const auto& event) {
    const auto* reconciliation =
        std::get_if<domain::ToolSpendReconciliationRequired>(&event.payload);
    return reconciliation != nullptr &&
           reconciliation->reason == domain::ToolSpendReconciliationReason::
                                         finalization_persistence_unknown;
  }));
}

TEST_CASE("paid tools fail closed before transport without bounded accounting",
          "[tools][runtime][spend][failure]") {
  const auto run_case = [](const std::optional<std::string>& ceiling,
                           domain::ToolSpendQuote quote,
                           const bool report_usage) {
    const auto invocation = make_id<domain::InvocationId>("paid-call");
    auto executor = std::make_shared<PaidExecutor>(
        std::move(quote), runtime::ToolResult{{domain::TextBlock{"unused"}}});
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(paid_declaration(), executor));
    const auto snapshot = snapshot_of(registry);
    auto initial =
        request("inference-1", "assistant-1", snapshot.declarations());
    std::vector<testing::ScriptedStep> steps{
        step(backend::ResponseStarted{"response"})};
    if (report_usage)
      steps.push_back(step(backend::UsageObserved{{0, 0, 0, 0}}));
    steps.push_back(
        step(backend::ToolCallDelta{invocation, "generate_image", "{}"}));
    steps.push_back(
        step(backend::ResponseFinished{domain::FinishReason::tool_call}));
    steps.push_back(testing::EndOfStream{});
    testing::ScriptedBackend backend{{
        {initial, testing::StreamScript{std::move(steps)}},
    }};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                              backend,
                              &wake,
                              {},
                              {},
                              snapshot,
                              paid_policy()};
    if (ceiling) set_spend_ceiling(kernel, *ceiling);
    auto start = run_start(initial);
    start.pricing_observation = pricing_observation();
    REQUIRE(kernel.start(std::move(start)));
    drain_to_inference_boundary(kernel, wake);
    REQUIRE(kernel.drain());
    CHECK(executor->starts == 0);
    CHECK(std::ranges::none_of(
        kernel.event_log().events(), [](const auto& value) {
          return std::holds_alternative<domain::ToolStarted>(value.payload) ||
                 std::holds_alternative<domain::ToolSpendReserved>(
                     value.payload);
        }));
    CHECK(
        std::ranges::any_of(kernel.event_log().events(), [](const auto& value) {
          return std::holds_alternative<domain::ToolErrored>(value.payload);
        }));
  };

  run_case(std::nullopt,
           {usd("0.1"), domain::ToolSpendEstimateBasis::catalog_estimate,
            spend_digest(), spend_expiry()},
           true);
  run_case("0.1",
           {usd("0.2"), domain::ToolSpendEstimateBasis::policy_upper_bound,
            spend_digest(), spend_expiry()},
           true);
  run_case("1",
           {usd("0.2"), domain::ToolSpendEstimateBasis::catalog_estimate,
            spend_digest(), spend_expiry()},
           false);
}

TEST_CASE("paid tool approval rejects its quote at the exact expiry boundary",
          "[tools][runtime][spend][approval][failure]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  auto now = domain::EventTimestamp{1ms};
  auto executor = std::make_shared<PaidExecutor>(
      domain::ToolSpendQuote{usd("0.4"),
                             domain::ToolSpendEstimateBasis::catalog_estimate,
                             spend_digest(), domain::EventTimestamp{10ms}},
      runtime::ToolResult{{domain::TextBlock{"unused"}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(paid_declaration(), executor));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            [&now] { return now; },
                            {},
                            snapshot,
                            paid_approval_policy()};
  set_spend_ceiling(kernel, "1");
  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  REQUIRE(kernel.start(std::move(start)));
  drain_to_inference_boundary(kernel, wake);

  now = domain::EventTimestamp{10ms};
  REQUIRE(kernel.decide_approval(make_id<domain::RunId>("run"), invocation,
                                 {domain::ApprovalDecision::approved,
                                  {paid_scope()},
                                  domain::ApprovalGrantLifetime::invocation}));

  CHECK(executor->starts == 0);
  CHECK(
      std::ranges::none_of(kernel.event_log().events(), [](const auto& value) {
        return std::holds_alternative<domain::ToolSpendReserved>(
                   value.payload) ||
               std::holds_alternative<domain::ToolStarted>(value.payload);
      }));
  CHECK(std::ranges::any_of(kernel.event_log().events(), [](const auto& value) {
    const auto* failed = std::get_if<domain::ToolErrored>(&value.payload);
    return failed != nullptr &&
           failed->error.message ==
               "paid tool price quote expired before launch";
  }));
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("successful paid result without cost evidence remains reserved",
          "[tools][runtime][spend][reconcile]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  auto executor = std::make_shared<PaidExecutor>(
      domain::ToolSpendQuote{usd("0.4"),
                             domain::ToolSpendEstimateBasis::policy_upper_bound,
                             spend_digest(), spend_expiry()},
      runtime::ToolResult{{domain::TextBlock{"done"}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(paid_declaration(), executor));
  const auto snapshot = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", snapshot.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
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
                            paid_policy()};
  set_spend_ceiling(kernel, "1");
  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  REQUIRE(kernel.start(std::move(start)));
  drain_to_inference_boundary(kernel, wake);
  for (int attempt = 0; attempt < 100; ++attempt) {
    REQUIRE(kernel.drain());
    if (std::ranges::any_of(kernel.event_log().events(), [](const auto& value) {
          return std::holds_alternative<
              domain::ToolSpendReconciliationRequired>(value.payload);
        })) {
      break;
    }
    wake.wait_for_change(wake.count());
  }
  CHECK(
      std::ranges::count_if(kernel.event_log().events(), [](const auto& value) {
        return std::holds_alternative<domain::ToolSpendReconciliationRequired>(
            value.payload);
      }) == 1);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("continuation projection withholds incomplete assistant tool turns",
          "[tools][runtime][failure]") {
  const auto invocation = make_id<domain::InvocationId>("blocking-call");
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(),
                                 std::make_shared<BlockingExecutor>(),
                                 runtime::ToolExecutionLimits{1024, 2, 30s}));
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

  const auto continuation =
      runtime::tool_continuation_messages(kernel.event_log().events());
  REQUIRE(continuation);
  REQUIRE(continuation->empty());
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "test cleanup"));
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

  REQUIRE(kernel.pending_tool_approval() ==
          (runtime::PendingToolApproval{make_id<domain::RunId>("run"),
                                        invocation,
                                        "lookup",
                                        {domain::Effect::read},
                                        {scope}}));

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
  REQUIRE_FALSE(kernel.pending_tool_approval());
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

TEST_CASE("durable pending approval is reconstructed without execution",
          "[tools][policy][storage][replay][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-medium-prompt-v1");
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      declaration(), executor, {},
      runtime::ToolExecutorContract{"test.lookup", "1"}));
  const auto tools = snapshot_of(registry);
  const auto make_policy = [&] {
    return runtime::make_tool_launch_policy(
        tools, {profile,
                testing::available_application_launch_context(
                    runtime::RestrictionLevel::medium),
                {}});
  };
  auto policy = make_policy();
  REQUIRE(policy);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation, "lookup", R"({"ratio":1.5})")},
  }};
  MemoryStore store;
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, *policy);
  REQUIRE(kernel);
  auto caller_owned = run_start(initial);
  caller_owned.attributes.permission_profile_id = profile;
  caller_owned.provenance = provenance();
  caller_owned.provenance->tool_policy = *(*policy)->provenance();
  auto rejected = (*kernel)->start(std::move(caller_owned));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == runtime::RunKernelErrorCode::invalid_start);

  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  start.provenance = provenance();
  REQUIRE((*kernel)->start(std::move(start)));
  drain_to_inference_boundary(**kernel, wake);
  REQUIRE((*kernel)->projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::awaiting_approval);
  REQUIRE((*kernel)->pending_tool_approval());
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 0);
  kernel->reset();

  auto replay_policy = make_policy();
  REQUIRE(replay_policy);
  testing::ScriptedBackend replay_backend{{}};
  WakeCounter replay_wake;
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, &replay_wake, {}, {}, tools, *replay_policy);
  REQUIRE(replayed);
  REQUIRE((*replayed)->projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::awaiting_approval);
  REQUIRE((*replayed)->pending_tool_approval());
  REQUIRE((*replayed)->pending_tool_approval()->invocation_id == invocation);
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 0);
  REQUIRE((*replayed)->decide_approval(make_id<domain::RunId>("run"),
                                       invocation,
                                       {domain::ApprovalDecision::denied, {}}));
  REQUIRE_FALSE((*replayed)->pending_tool_approval());
  REQUIRE(executor->starts == 0);
  REQUIRE((*replayed)->cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("durable consumed approval is renewed before restart execution",
          "[tools][policy][storage][replay][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-none-prompt-v2");
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      declaration(), executor, {},
      runtime::ToolExecutorContract{"test.lookup", "1"}));
  const auto tools = snapshot_of(registry);
  const auto make_policy = [&] {
    return runtime::make_tool_launch_policy(
        tools, {profile,
                testing::available_application_launch_context(
                    runtime::RestrictionLevel::none),
                {}});
  };
  auto policy = make_policy();
  REQUIRE(policy);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation, "lookup", R"({"ratio":1.5})")},
  }};
  MemoryStore store;
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, *policy);
  REQUIRE(kernel);
  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  start.provenance = provenance();
  REQUIRE((*kernel)->start(std::move(start)));
  drain_to_inference_boundary(**kernel, wake);
  REQUIRE((*kernel)->pending_tool_approval());

  store.fail_next_tool_started_append = true;
  auto interrupted = (*kernel)->decide_approval(
      make_id<domain::RunId>("run"), invocation,
      {domain::ApprovalDecision::approved,
       {{domain::Effect::read, "filesystem.root", "/repo"}},
       domain::ApprovalGrantLifetime::invocation});
  REQUIRE_FALSE(interrupted);
  REQUIRE(interrupted.error().code ==
          runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(executor->starts == 0);
  kernel->reset();

  auto replay_policy = make_policy();
  REQUIRE(replay_policy);
  testing::ScriptedBackend replay_backend{{}};
  WakeCounter replay_wake;
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, &replay_wake, {}, {}, tools, *replay_policy);
  REQUIRE(replayed);
  REQUIRE((*replayed)->pending_tool_approval());
  REQUIRE((*replayed)->projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::awaiting_approval);
  REQUIRE(executor->starts == 0);

  const auto approvals_requested =
      std::ranges::count_if(store.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolApprovalRequested>(
            event.payload);
      });
  const auto approvals_decided =
      std::ranges::count_if(store.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolApprovalDecided>(
            event.payload);
      });
  REQUIRE(approvals_requested == 2);
  REQUIRE(approvals_decided == 1);

  REQUIRE((*replayed)->decide_approval(
      make_id<domain::RunId>("run"), invocation,
      {domain::ApprovalDecision::approved,
       {{domain::Effect::read, "filesystem.root", "/repo"}},
       domain::ApprovalGrantLifetime::invocation}));
  replay_wake.wait_for_change(0);
  REQUIRE(executor->starts == 1);
  REQUIRE(std::ranges::count_if(store.events, [](const auto& event) {
            return std::holds_alternative<domain::ToolApprovalDecided>(
                event.payload);
          }) == 2);
}

TEST_CASE("floating-point tool arguments remain usable outside automatic mode",
          "[tools][policy][automatic][canonical][failure]") {
  const auto approval =
      GENERATE(runtime::ApprovalMode::prompt, runtime::ApprovalMode::automatic,
               runtime::ApprovalMode::allow_all);
  CAPTURE(approval);
  const auto invocation = make_id<domain::InvocationId>("floating-call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-none-floating-v2");
  auto executor = std::make_shared<ImmediateExecutor>(
      std::vector{scope()}, std::vector{domain::Effect::read},
      runtime::ToolResult{{domain::TextBlock{"done"}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(), executor));
  const auto tools = snapshot_of(registry);

  std::shared_ptr<runtime::AutomaticApprovalMatcher> matcher;
  std::optional<std::string> matcher_identity;
  if (approval == runtime::ApprovalMode::automatic) {
    matcher = runtime::compile_automatic_approval_matcher(
                  {runtime::ExactToolArgumentsApprovalRule{
                      "lookup",
                      runtime::canonicalize_validated_tool_arguments(
                          {"application/json", "{}"})
                          .value(),
                      {{runtime::RestrictionLevel::none}, 1, std::nullopt, 0}}})
                  .value();
    matcher_identity = std::string{matcher->identity()};
  }
  auto launch_policy = runtime::make_tool_launch_policy(
      tools, {profile,
              testing::available_application_launch_context(
                  runtime::RestrictionLevel::none, approval,
                  std::move(matcher_identity)),
              std::move(matcher)});
  REQUIRE(launch_policy);
  auto policy = std::make_shared<CountingPolicy>(std::move(*launch_policy));

  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation, "lookup", R"({"ratio":1.5})")},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            tools,
                            policy};
  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  REQUIRE(kernel.start(std::move(start)));
  drain_to_inference_boundary(kernel, wake);
  REQUIRE(policy->requests.size() == 1);
  REQUIRE_FALSE(policy->requests.front().canonical_arguments);

  if (approval == runtime::ApprovalMode::automatic) {
    REQUIRE(executor->invocations().empty());
    const auto decided = std::ranges::find_if(
        kernel.event_log().events(), [](const auto& event) {
          const auto* value =
              std::get_if<domain::ToolPolicyDecided>(&event.payload);
          return value != nullptr &&
                 value->decision == domain::PolicyDecision::deny;
        });
    REQUIRE(decided != kernel.event_log().events().end());
  } else {
    if (approval == runtime::ApprovalMode::prompt) {
      REQUIRE(kernel.pending_tool_approval());
      REQUIRE(
          kernel.decide_approval(make_id<domain::RunId>("run"), invocation,
                                 {domain::ApprovalDecision::approved,
                                  {scope()},
                                  domain::ApprovalGrantLifetime::invocation}));
    }
    for (int attempt = 0; attempt < 100 && executor->invocations().empty();
         ++attempt) {
      REQUIRE(kernel.drain());
      if (executor->invocations().empty()) wake.wait_for_change(wake.count());
    }
    REQUIRE(executor->invocations().size() == 1);
    REQUIRE(executor->invocations().front().arguments.value.data ==
            R"({"ratio":1.5})");
  }
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("durable implicit approval is revalidated before restart execution",
          "[tools][policy][storage][replay][failure]") {
  const auto approval = GENERATE(runtime::ApprovalMode::automatic,
                                 runtime::ApprovalMode::allow_all);
  CAPTURE(approval);
  const std::string arguments =
      approval == runtime::ApprovalMode::automatic ? "{}" : R"({"ratio":1.5})";
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-none-implicit-v2");
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      declaration(), executor, {},
      runtime::ToolExecutorContract{"test.lookup", "1"}));
  const auto tools = snapshot_of(registry);
  const auto make_policy = [&] {
    std::shared_ptr<runtime::AutomaticApprovalMatcher> matcher;
    std::optional<std::string> matcher_identity;
    if (approval == runtime::ApprovalMode::automatic) {
      matcher =
          runtime::compile_automatic_approval_matcher(
              {runtime::ExactToolArgumentsApprovalRule{
                  "lookup",
                  runtime::canonicalize_validated_tool_arguments(
                      {"application/json", "{}"})
                      .value(),
                  {{runtime::RestrictionLevel::none}, 1, std::nullopt, 0}}})
              .value();
      matcher_identity = std::string{matcher->identity()};
    }
    auto policy = runtime::make_tool_launch_policy(
        tools, {profile,
                testing::available_application_launch_context(
                    runtime::RestrictionLevel::none, approval,
                    std::move(matcher_identity)),
                std::move(matcher)});
    REQUIRE(policy);
    return std::make_shared<CountingPolicy>(std::move(*policy));
  };

  auto policy = make_policy();
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation, "lookup", arguments)},
  }};
  MemoryStore store;
  store.fail_next_tool_started_append = true;
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, policy);
  REQUIRE(kernel);
  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  start.provenance = provenance();
  REQUIRE((*kernel)->start(std::move(start)));

  std::optional<runtime::RunKernelError> interruption;
  std::size_t observed{};
  for (int attempt = 0; attempt < 100 && !interruption; ++attempt) {
    auto drained = (*kernel)->drain();
    if (!drained) {
      interruption = std::move(drained.error());
      break;
    }
    if ((*kernel)->active_inference_id()) wake.wait_for_change(observed);
    observed = wake.count();
  }
  REQUIRE(interruption);
  REQUIRE(interruption->code == runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(policy->evaluations == 1);
  REQUIRE(policy->requests.size() == 1);
  REQUIRE(policy->requests.front().canonical_arguments.has_value() ==
          (approval == runtime::ApprovalMode::automatic));
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 0);
  REQUIRE(std::ranges::count_if(store.events, [](const auto& event) {
            return std::holds_alternative<domain::ToolPolicyDecided>(
                event.payload);
          }) == 1);
  const auto decided =
      std::ranges::find_if(store.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolPolicyDecided>(event.payload);
      });
  REQUIRE(decided != store.events.end());
  const auto& policy_decision =
      std::get<domain::ToolPolicyDecided>(decided->payload);
  REQUIRE(decided->metadata.schema_version == 2);
  if (approval == runtime::ApprovalMode::automatic) {
    REQUIRE(policy_decision.source ==
            domain::PolicyDecisionSource::automatic_matcher);
    REQUIRE(policy_decision.automatic_approval.has_value());
    REQUIRE_FALSE(
        policy_decision.automatic_approval->policy_identity.contains("lookup"));
    REQUIRE_FALSE(
        policy_decision.automatic_approval->rule_identity.contains("{}"));
  } else {
    REQUIRE_FALSE(policy_decision.automatic_approval);
  }
  REQUIRE(std::ranges::none_of(store.events, [](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  }));
  if (approval == runtime::ApprovalMode::automatic) {
    auto exhausted_request = policy->requests.front();
    exhausted_request.invocation_id =
        make_id<domain::InvocationId>("after-failed-start");
    const auto exhausted = policy->evaluate(exhausted_request);
    REQUIRE(exhausted);
    REQUIRE(exhausted->decision == domain::PolicyDecision::deny);
    REQUIRE(exhausted->scopes.empty());
    REQUIRE_FALSE(exhausted->automatic_approval);
    REQUIRE(policy->evaluations == 2);
    REQUIRE(executor->starts == 0);
  }
  kernel->reset();

  auto replay_policy = make_policy();
  testing::ScriptedBackend replay_backend{{}};
  WakeCounter replay_wake;
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, &replay_wake, {}, {}, tools, replay_policy);
  REQUIRE(replayed);
  REQUIRE(replay_policy->evaluations == 1);
  REQUIRE(replay_policy->requests.size() == 1);
  REQUIRE(replay_policy->requests.front().canonical_arguments.has_value() ==
          (approval == runtime::ApprovalMode::automatic));
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 0);
  REQUIRE_FALSE((*replayed)->pending_tool_approval());
  REQUIRE(std::ranges::none_of(store.events, [](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  }));

  auto resumed = (*replayed)->drain();
  REQUIRE(resumed);
  REQUIRE(std::ranges::any_of(*resumed, [](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  }));
  replay_wake.wait_for_change(0);
  REQUIRE(replay_policy->evaluations == 1);
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 1);
}

TEST_CASE("automatic approval replay rejects matcher drift and stale state",
          "[tools][policy][automatic][storage][replay][failure]") {
  const auto scenario = GENERATE(AutomaticReplayScenario::changed_policy,
                                 AutomaticReplayScenario::exhausted_matcher,
                                 AutomaticReplayScenario::expired_matcher);
  CAPTURE(scenario);
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-none-automatic-v2");
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      declaration(), executor, {},
      runtime::ToolExecutorContract{"test.lookup", "1"}));
  const auto tools = snapshot_of(registry);
  const auto canonical = runtime::canonicalize_validated_tool_arguments(
      {"application/json", "{}"});
  REQUIRE(canonical);
  auto now = std::chrono::steady_clock::time_point{};
  const auto clock = [&] { return now; };
  const auto maximum_matches =
      scenario == AutomaticReplayScenario::expired_matcher ? 2U : 1U;
  const auto expiry = scenario == AutomaticReplayScenario::expired_matcher
                          ? std::optional{10ms}
                          : std::nullopt;
  auto initial_matcher = runtime::compile_automatic_approval_matcher(
      {runtime::ExactToolArgumentsApprovalRule{
          "lookup",
          *canonical,
          {{runtime::RestrictionLevel::none}, maximum_matches, expiry, 0}}},
      clock);
  REQUIRE(initial_matcher);
  const auto make_policy =
      [&](std::shared_ptr<runtime::AutomaticApprovalMatcher> matcher) {
        auto launch_policy = runtime::make_tool_launch_policy(
            tools, {profile,
                    testing::available_application_launch_context(
                        runtime::RestrictionLevel::none,
                        runtime::ApprovalMode::automatic,
                        std::string{matcher->identity()}),
                    std::move(matcher)});
        REQUIRE(launch_policy);
        return std::make_shared<CountingPolicy>(std::move(*launch_policy));
      };
  auto initial_policy = make_policy(*initial_matcher);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  MemoryStore store;
  store.fail_next_tool_started_append = true;
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, initial_policy);
  REQUIRE(kernel);
  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  start.provenance = provenance();
  REQUIRE((*kernel)->start(std::move(start)));

  std::optional<runtime::RunKernelError> interruption;
  std::size_t observed{};
  for (int attempt = 0; attempt < 100 && !interruption; ++attempt) {
    auto drained = (*kernel)->drain();
    if (!drained) {
      interruption = std::move(drained.error());
      break;
    }
    if ((*kernel)->active_inference_id()) wake.wait_for_change(observed);
    observed = wake.count();
  }
  REQUIRE(interruption);
  REQUIRE(interruption->code == runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(initial_policy->evaluations == 1);
  REQUIRE(executor->starts == 0);
  REQUIRE(std::ranges::none_of(store.events, [](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  }));
  kernel->reset();

  std::shared_ptr<CountingPolicy> replay_policy;
  std::size_t expected_evaluations{};
  if (scenario == AutomaticReplayScenario::changed_policy) {
    const auto changed = runtime::canonicalize_validated_tool_arguments(
        {"application/json", R"({"changed":true})"});
    REQUIRE(changed);
    auto changed_matcher = runtime::compile_automatic_approval_matcher(
        {runtime::ExactToolArgumentsApprovalRule{
            "lookup",
            *changed,
            {{runtime::RestrictionLevel::none}, 1, std::nullopt, 0}}},
        clock);
    REQUIRE(changed_matcher);
    replay_policy = make_policy(*changed_matcher);
  } else if (scenario == AutomaticReplayScenario::exhausted_matcher) {
    auto exhausted_matcher = runtime::compile_automatic_approval_matcher(
        {runtime::ExactToolArgumentsApprovalRule{
            "lookup",
            *canonical,
            {{runtime::RestrictionLevel::none}, 1, std::nullopt, 0}}},
        clock);
    REQUIRE(exhausted_matcher);
    const auto consumed = (*exhausted_matcher)
                              ->match(runtime::AutomaticApprovalMatchRequest{
                                  store.session_id,
                                  make_id<domain::RunId>("preconsume-run"),
                                  make_id<domain::InvocationId>("preconsume"),
                                  "lookup",
                                  *canonical,
                                  runtime::RestrictionLevel::none,
                                  {domain::Effect::read},
                                  {scope()}});
    REQUIRE(consumed);
    REQUIRE(consumed->has_value());
    replay_policy = make_policy(*exhausted_matcher);
    expected_evaluations = 1;
  } else {
    now += 10ms;
    replay_policy = initial_policy;
    expected_evaluations = 2;
  }

  testing::ScriptedBackend replay_backend{{}};
  WakeCounter replay_wake;
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, &replay_wake, {}, {}, tools, replay_policy);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);
  REQUIRE(replay_policy->evaluations == expected_evaluations);
  REQUIRE(executor->starts == 0);
  REQUIRE(std::ranges::none_of(store.events, [](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  }));
}

TEST_CASE("run kernel rejects secret-like automatic approval evidence",
          "[tools][policy][automatic][redaction][failure]") {
  const auto invocation = make_id<domain::InvocationId>("call");
  const auto profile = make_id<domain::PermissionProfileId>("observe");
  auto executor = std::make_shared<CountingExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration(), executor));
  const auto tools = snapshot_of(registry);

  auto matcher = runtime::compile_automatic_approval_matcher(
      {runtime::ExactToolArgumentsApprovalRule{
          "lookup",
          runtime::canonicalize_validated_tool_arguments(
              {"application/json", "{}"})
              .value(),
          {{runtime::RestrictionLevel::none}, 1, std::nullopt, 0}}});
  REQUIRE(matcher);
  auto launch_policy = runtime::make_tool_launch_policy(
      tools,
      {profile,
       testing::available_application_launch_context(
           runtime::RestrictionLevel::none, runtime::ApprovalMode::automatic,
           std::string{(*matcher)->identity()}),
       *matcher});
  REQUIRE(launch_policy);
  auto policy =
      std::make_shared<CorruptingAutomaticPolicy>(std::move(*launch_policy));

  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation)},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            tools,
                            std::move(policy)};
  REQUIRE(kernel.start(run_start(initial)));
  std::size_t observed{};
  for (int attempt = 0; attempt < 100 && kernel.active_inference_id();
       ++attempt) {
    REQUIRE(kernel.drain());
    if (kernel.active_inference_id()) wake.wait_for_change(observed);
    observed = wake.count();
  }
  REQUIRE_FALSE(kernel.active_inference_id());
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 0);
  REQUIRE(
      std::ranges::any_of(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::RunFailed>(event.payload);
      }));
  REQUIRE(
      std::ranges::none_of(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::ToolPolicyDecided>(
                   event.payload) ||
               std::holds_alternative<domain::ToolStarted>(event.payload);
      }));
}

TEST_CASE("durable paid approval replay invokes no executor methods",
          "[tools][policy][storage][replay][spend][failure]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-low-prompt-v1");
  const auto quote = domain::ToolSpendQuote{
      usd("0.4"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), domain::EventTimestamp::max()};
  auto executor = std::make_shared<PaidExecutor>(
      quote, runtime::ToolResult{{domain::TextBlock{"unused"}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      paid_declaration(), executor, {},
      runtime::ToolExecutorContract{"test.generate-image", "1"}));
  const auto tools = snapshot_of(registry);
  const auto make_policy = [&] {
    return runtime::make_tool_launch_policy(
        tools, {profile,
                testing::available_application_launch_context(
                    runtime::RestrictionLevel::low),
                {}});
  };
  auto policy = make_policy();
  REQUIRE(policy);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  MemoryStore store;
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, *policy);
  REQUIRE(kernel);
  set_spend_ceiling(**kernel, "1");
  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  start.provenance = provenance();
  REQUIRE((*kernel)->start(std::move(start)));
  drain_to_inference_boundary(**kernel, wake);
  REQUIRE((*kernel)->pending_tool_approval());
  REQUIRE(executor->validations == 1);
  REQUIRE(executor->starts == 0);
  kernel->reset();

  auto replay_policy = make_policy();
  REQUIRE(replay_policy);
  testing::ScriptedBackend replay_backend{{}};
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, nullptr, {}, {}, tools, *replay_policy);
  REQUIRE(replayed);
  REQUIRE((*replayed)->pending_tool_approval());
  CHECK((*replayed)->pending_tool_approval()->invocation_id == invocation);
  CHECK(executor->validations == 1);
  CHECK(executor->starts == 0);
  REQUIRE((*replayed)->decide_approval(make_id<domain::RunId>("run"),
                                       invocation,
                                       {domain::ApprovalDecision::denied, {}}));
  CHECK(executor->validations == 1);
  CHECK(executor->starts == 0);
  REQUIRE((*replayed)->cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("durable paid approval reuses normalized arguments and raw history",
          "[tools][policy][storage][replay][spend]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-low-prompt-v1");
  const auto quote = domain::ToolSpendQuote{
      usd("0.4"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), domain::EventTimestamp::max()};
  const auto raw =
      domain::StructuredDataBlock{"application/json", R"({"count":"1"})"};
  const auto normalized =
      domain::StructuredDataBlock{"application/json", R"({"count":1})"};
  auto executor = std::make_shared<PaidExecutor>(
      quote,
      runtime::ToolResult{{domain::TextBlock{"done"}},
                          {},
                          domain::ToolSpendReleased{invocation}},
      normalized);
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      paid_declaration(), executor, {},
      runtime::ToolExecutorContract{"test.generate-image", "1"}));
  const auto tools = snapshot_of(registry);
  const auto make_policy = [&] {
    return runtime::make_tool_launch_policy(
        tools, {profile,
                testing::available_application_launch_context(
                    runtime::RestrictionLevel::low),
                {}});
  };
  auto policy = make_policy();
  REQUIRE(policy);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", raw.data}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  MemoryStore store;
  WakeCounter wake;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, tools, *policy);
  REQUIRE(kernel);
  set_spend_ceiling(**kernel, "1");
  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  start.provenance = provenance();
  start.pricing_observation = pricing_observation();
  REQUIRE((*kernel)->start(std::move(start)));
  drain_to_inference_boundary(**kernel, wake);
  REQUIRE((*kernel)->pending_tool_approval());
  const auto proposed =
      std::ranges::find_if(store.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolProposed>(event.payload);
      });
  REQUIRE(proposed != store.events.end());
  CHECK(std::get<domain::ToolProposed>(proposed->payload).arguments == raw);
  CHECK(std::get<domain::ToolProposed>(proposed->payload).validated_arguments ==
        normalized);
  CHECK(executor->validations == 1);
  kernel->reset();

  auto replay_policy = make_policy();
  REQUIRE(replay_policy);
  testing::ScriptedBackend replay_backend{{}};
  WakeCounter replay_wake;
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, &replay_wake, {}, {}, tools, *replay_policy);
  REQUIRE(replayed);
  CHECK(executor->validations == 1);
  REQUIRE((*replayed)->decide_approval(
      make_id<domain::RunId>("run"), invocation,
      {domain::ApprovalDecision::approved,
       {paid_scope()},
       domain::ApprovalGrantLifetime::invocation}));
  const auto has_result = [&] {
    return std::ranges::any_of(
        (*replayed)->event_log().events(), [](const auto& event) {
          return std::holds_alternative<domain::ToolResultRecorded>(
              event.payload);
        });
  };
  for (int attempt = 0; attempt < 100 && !has_result(); ++attempt) {
    REQUIRE((*replayed)->drain());
    if (!has_result()) std::this_thread::sleep_for(10ms);
  }
  REQUIRE(has_result());
  REQUIRE(executor->starts == 1);
  REQUIRE(executor->invocations.size() == 1);
  CHECK(executor->invocations.front().arguments.value == normalized);
  const auto continuation =
      runtime::tool_continuation_messages((*replayed)->event_log().events());
  REQUIRE(continuation);
  REQUIRE_FALSE(continuation->empty());
  REQUIRE(continuation->front().tool_calls.size() == 1);
  CHECK(continuation->front().tool_calls.front().arguments == raw);
  REQUIRE((*replayed)->cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("durable paid replay rejects malformed normalized arguments",
          "[tools][policy][storage][replay][spend][failure]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto profile =
      make_id<domain::PermissionProfileId>("tools-low-prompt-v1");
  const auto quote = domain::ToolSpendQuote{
      usd("0.4"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), domain::EventTimestamp::max()};
  auto executor = std::make_shared<PaidExecutor>(
      quote, runtime::ToolResult{{domain::TextBlock{"unused"}}});
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      paid_declaration(), executor, {},
      runtime::ToolExecutorContract{"test.generate-image", "1"}));
  const auto tools = snapshot_of(registry);
  const auto make_policy = [&] {
    return runtime::make_tool_launch_policy(
        tools, {profile,
                testing::available_application_launch_context(
                    runtime::RestrictionLevel::low),
                {}});
  };
  auto policy = make_policy();
  REQUIRE(policy);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial, tool_call_script(invocation, "generate_image")},
  }};
  MemoryStore store;
  WakeCounter wake;
  runtime::RunKernelLimits limits;
  limits.tool_argument_bytes = 64;
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, limits, tools, *policy);
  REQUIRE(kernel);
  set_spend_ceiling(**kernel, "1");
  auto start = run_start(initial);
  start.attributes.permission_profile_id = profile;
  start.provenance = provenance();
  start.pricing_observation = pricing_observation();
  REQUIRE((*kernel)->start(std::move(start)));
  drain_to_inference_boundary(**kernel, wake);
  REQUIRE((*kernel)->pending_tool_approval());
  kernel->reset();

  auto proposed_event =
      std::ranges::find_if(store.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolProposed>(event.payload);
      });
  REQUIRE(proposed_event != store.events.end());
  auto& proposed = std::get<domain::ToolProposed>(proposed_event->payload);
  REQUIRE(proposed.validated_arguments);
  SECTION("wrong media type") {
    proposed.validated_arguments->media_type = "text/plain";
  }
  SECTION("empty payload") {
    proposed.validated_arguments->data.clear();
  }
  SECTION("oversized payload") {
    proposed.validated_arguments->data.assign(limits.tool_argument_bytes + 1U,
                                              'x');
  }

  auto replay_policy = make_policy();
  REQUIRE(replay_policy);
  testing::ScriptedBackend replay_backend{{}};
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, nullptr, {}, limits, tools, *replay_policy);
  REQUIRE_FALSE(replayed);
  CHECK(replayed.error().code == runtime::RunKernelErrorCode::replay_rejected);
  CHECK(executor->validations == 1);
  CHECK(executor->starts == 0);
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
  const auto continuation_messages =
      runtime::tool_continuation_messages(kernel.event_log().events());
  REQUIRE(continuation_messages);
  REQUIRE(continuation_messages->size() == 2);
  REQUIRE(continuation_messages->front().role == domain::Role::assistant);
  REQUIRE(continuation_messages->front().tool_calls.size() == 1);
  REQUIRE(continuation_messages->back() == messages->front());
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

TEST_CASE("paid tool cancellation and timeout reconcile started transport",
          "[tools][spend][cancellation][failure]") {
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto quote = domain::ToolSpendQuote{
      usd("0.4"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), domain::EventTimestamp::max()};
  auto timeout = 1000ms;
  bool cancel{};
  SECTION("deadline") {
    timeout = 20ms;
  }
  SECTION("cancellation") {
    cancel = true;
  }

  const runtime::ToolExecutionLimits limits{128, 2, timeout};
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(paid_declaration(),
                                 std::make_shared<PaidBlockingExecutor>(quote),
                                 limits));
  const auto tools = snapshot_of(registry);
  auto initial = request("inference-1", "assistant-1", tools.declarations());
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::UsageObserved{{0, 0, 0, 0}}),
           step(backend::ToolCallDelta{invocation, "generate_image", "{}"}),
           step(backend::ResponseFinished{domain::FinishReason::tool_call}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            backend,
                            &wake,
                            {},
                            {},
                            tools,
                            paid_policy()};
  set_spend_ceiling(kernel, "1");
  auto start = run_start(initial);
  start.pricing_observation = pricing_observation();
  REQUIRE(kernel.start(std::move(start)));
  drain_to_inference_boundary(kernel, wake);

  const auto has_event = [&](const auto predicate) {
    return std::ranges::any_of(kernel.event_log().events(), predicate);
  };
  for (int attempt = 0;
       attempt < 100 && !has_event([](const auto& event) {
         return std::holds_alternative<domain::ToolStarted>(event.payload);
       });
       ++attempt) {
    REQUIRE(kernel.drain());
    std::this_thread::sleep_for(10ms);
  }
  REQUIRE(has_event([](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  }));

  if (cancel) {
    REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "user cancelled"));
  } else {
    for (int attempt = 0;
         attempt < 100 && !has_event([](const auto& event) {
           return std::holds_alternative<domain::ToolErrored>(event.payload);
         });
         ++attempt) {
      REQUIRE(kernel.drain());
      std::this_thread::sleep_for(10ms);
    }
  }

  const auto& events = kernel.event_log().events();
  const auto reserved = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolSpendReserved>(event.payload);
  });
  const auto started = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolStarted>(event.payload);
  });
  const auto reconciled = std::ranges::find_if(events, [](const auto& event) {
    const auto* value =
        std::get_if<domain::ToolSpendReconciliationRequired>(&event.payload);
    return value != nullptr &&
           value->reason ==
               domain::ToolSpendReconciliationReason::transport_outcome_unknown;
  });
  const auto errored = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolErrored>(event.payload);
  });
  REQUIRE(reserved != events.end());
  REQUIRE(started != events.end());
  REQUIRE(reconciled != events.end());
  REQUIRE(errored != events.end());
  CHECK(reserved < started);
  CHECK(started < reconciled);
  CHECK(reconciled < errored);
  CHECK(std::ranges::none_of(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolSpendReleased>(event.payload) ||
           std::holds_alternative<domain::ToolSpendFinalized>(event.payload);
  }));
  if (!cancel) {
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

TEST_CASE(
    "durable replay reconciles an interrupted paid tool without execution",
    "[tools][replay][spend][failure]") {
  const auto session = make_id<domain::SessionId>("paid-session");
  const auto run = make_id<domain::RunId>("run");
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto assistant = make_id<domain::MessageId>("assistant");
  const auto result_message = make_id<domain::MessageId>("tool-message");
  const auto quote = domain::ToolSpendQuote{
      usd("0.4"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), domain::EventTimestamp{1000ms}};
  MemoryStore store;
  store.session_id = session;
  store.events = {
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
                                           "generate_image",
                                           {"application/json", "{}"},
                                           {domain::Effect::spend},
                                           std::nullopt,
                                           true,
                                           {},
                                           {},
                                           result_message,
                                           quote,
                                           domain::StructuredDataBlock{
                                               "application/json", "{}"}},
                      invocation),
      persisted_event(7,
                      domain::AssistantContentFinished{assistant, inference}),
      persisted_event(
          8, domain::InferenceFinished{inference,
                                       domain::FinishReason::tool_call}),
      persisted_event(
          9,
          domain::ToolPolicyDecided{
              invocation, domain::PolicyDecision::allow, {}, std::nullopt},
          invocation),
      persisted_event(10,
                      domain::ToolSpendReserved{domain::ToolSpendReservation{
                          invocation, quote.maximum, quote.basis,
                          quote.evidence_digest, quote.valid_until}},
                      invocation),
      persisted_event(11, domain::ToolStarted{invocation}, invocation),
  };
  store.events[5].metadata.schema_version = 2;
  testing::ScriptedBackend backend{{}};

  auto kernel = runtime::RunKernel::open_durable(
      {session, runtime::DurableSessionMode::resume,
       domain::EventTimestamp{1ms}},
      store, backend, nullptr, [] { return domain::EventTimestamp{100ms}; });

  REQUIRE(kernel);
  REQUIRE(store.events.size() == 14);
  CHECK(std::holds_alternative<domain::ToolSpendReconciliationRequired>(
      store.events[11].payload));
  CHECK(std::holds_alternative<domain::ToolErrored>(store.events[12].payload));
  CHECK(std::holds_alternative<domain::RunFailed>(store.events[13].payload));
  REQUIRE((*kernel)->projection(run) != nullptr);
  CHECK((*kernel)->projection(run)->status() == domain::RunStatus::failed);
  CHECK_FALSE((*kernel)->active_run_id());
}

TEST_CASE("durable replay releases a paid reservation before tool start",
          "[tools][replay][spend][failure]") {
  const auto session = make_id<domain::SessionId>("paid-session");
  const auto run = make_id<domain::RunId>("run");
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto assistant = make_id<domain::MessageId>("assistant");
  const auto result_message = make_id<domain::MessageId>("tool-message");
  const auto quote = domain::ToolSpendQuote{
      usd("0.4"), domain::ToolSpendEstimateBasis::catalog_estimate,
      spend_digest(), domain::EventTimestamp{1000ms}};
  MemoryStore store;
  store.session_id = session;
  store.events = {
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
                                           "generate_image",
                                           {"application/json", "{}"},
                                           {domain::Effect::spend},
                                           std::nullopt,
                                           true,
                                           {},
                                           {},
                                           result_message,
                                           quote,
                                           domain::StructuredDataBlock{
                                               "application/json", "{}"}},
                      invocation),
      persisted_event(7,
                      domain::AssistantContentFinished{assistant, inference}),
      persisted_event(
          8, domain::InferenceFinished{inference,
                                       domain::FinishReason::tool_call}),
      persisted_event(
          9,
          domain::ToolPolicyDecided{
              invocation, domain::PolicyDecision::allow, {}, std::nullopt},
          invocation),
      persisted_event(10,
                      domain::ToolSpendReserved{domain::ToolSpendReservation{
                          invocation, quote.maximum, quote.basis,
                          quote.evidence_digest, quote.valid_until}},
                      invocation),
  };
  store.events[5].metadata.schema_version = 2;
  testing::ScriptedBackend backend{{}};

  auto kernel = runtime::RunKernel::open_durable(
      {session, runtime::DurableSessionMode::resume,
       domain::EventTimestamp{1ms}},
      store, backend, nullptr, [] { return domain::EventTimestamp{100ms}; });

  REQUIRE(kernel);
  REQUIRE(store.events.size() == 13);
  CHECK(std::holds_alternative<domain::ToolSpendReleased>(
      store.events[10].payload));
  CHECK(std::holds_alternative<domain::ToolErrored>(store.events[11].payload));
  CHECK(std::holds_alternative<domain::RunFailed>(store.events[12].payload));
  REQUIRE((*kernel)->projection(run) != nullptr);
  CHECK((*kernel)->projection(run)->status() == domain::RunStatus::failed);
  CHECK_FALSE((*kernel)->active_run_id());
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
  REQUIRE(
      policy->requests.front().canonical_arguments ==
      runtime::canonicalize_validated_tool_arguments({"application/json", "{}"})
          .value());
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
