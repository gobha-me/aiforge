#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/surfaces/one_shot.hpp>
#include <aiforge/testing/scripted_persona_source.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto reported_cost(const std::string& usd_value = "0") -> domain::ReportedCost {
  auto usd = domain::MonetaryAmount::create(
                 "USD", domain::DecimalAmount::from(usd_value).value())
                 .value();
  auto diem =
      domain::MonetaryAmount::create(
          "venice.diem", domain::DecimalAmount::from("0.0645375").value())
          .value();
  return domain::ReportedCost::create({std::move(usd), std::move(diem)})
      .value();
}

auto pricing_observation() -> domain::PricingObservation {
  domain::TextPricing pricing;
  pricing.base.input =
      domain::PriceRate{domain::DecimalAmount::from("1").value(),
                        domain::DecimalAmount::from("1").value()};
  pricing.base.output =
      domain::PriceRate{domain::DecimalAmount::from("2").value(),
                        domain::DecimalAmount::from("2").value()};
  pricing.base.cache_input =
      domain::PriceRate{domain::DecimalAmount::from("0.5").value(),
                        domain::DecimalAmount::from("0.5").value()};
  return domain::make_pricing_observation(
             make_id<domain::ModelId>("model"), "test.models", std::nullopt,
             domain::EventTimestamp{std::chrono::milliseconds{123}},
             domain::PricingCatalogOrigin::live, std::move(pricing))
      .value();
}

auto run_provenance() -> domain::RunProvenance {
  return {"0.10.0",
          "venice",
          std::nullopt,
          make_id<domain::ModelId>("model"),
          domain::CredentialSourceReference{
              domain::CredentialSourceKind::environment, "VENICE_API_KEY"},
          {{"model",
            std::string{"venice-model"},
            true,
            domain::ProvenanceSource::environment,
            false,
            {{domain::ProvenanceSource::environment,
              domain::ProvenanceDisposition::selected, std::nullopt}}},
           {"credential",
            std::nullopt,
            true,
            domain::ProvenanceSource::environment,
            true,
            {{domain::ProvenanceSource::environment,
              domain::ProvenanceDisposition::selected, std::nullopt}}}},
          {{"aiforge", "0.10.0"}},
          {}};
}

struct End {};
using Item = std::variant<backend::BackendEvent, backend::BackendError, End>;

class VectorStream final : public backend::BackendStream {
 public:
  explicit VectorStream(std::vector<Item> items) : m_items(std::move(items)) {}

  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_index >= m_items.size())
      return std::optional<backend::BackendEvent>{};
    auto& item = m_items[m_index++];
    if (auto* event = std::get_if<backend::BackendEvent>(&item)) {
      return std::optional<backend::BackendEvent>{std::move(*event)};
    }
    if (auto* error = std::get_if<backend::BackendError>(&item)) {
      return std::unexpected(std::move(*error));
    }
    return std::optional<backend::BackendEvent>{};
  }

 private:
  std::vector<Item> m_items;
  std::size_t m_index{};
};

class CapturingBackend final : public backend::Backend {
 public:
  explicit CapturingBackend(std::vector<Item> items)
      : m_items(std::move(items)) {}

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    captured = std::move(request);
    ++starts;
    return std::make_unique<VectorStream>(std::move(m_items));
  }

  std::optional<backend::BackendRequest> captured;
  std::size_t starts{};

 private:
  std::vector<Item> m_items;
};

auto success_items(const domain::MessageId& message_id,
                   const std::string& usd_value = "0") -> std::vector<Item>;

class ToolLoopBackend final : public backend::Backend {
 public:
  explicit ToolLoopBackend(std::string arguments = "{}")
      : m_arguments(std::move(arguments)) {}

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    const auto assistant = request.assistant_message_id;
    captured.push_back(std::move(request));
    if (captured.size() == 1) {
      return std::make_unique<VectorStream>(std::vector<Item>{
          backend::BackendEvent{backend::ResponseStarted{"tool-response"}},
          backend::BackendEvent{backend::ToolCallDelta{
              make_id<domain::InvocationId>("lookup-call"), "lookup",
              m_arguments}},
          backend::BackendEvent{
              backend::ResponseFinished{domain::FinishReason::tool_call}},
          End{},
      });
    }
    return std::make_unique<VectorStream>(success_items(assistant));
  }

  std::vector<backend::BackendRequest> captured;

 private:
  std::string m_arguments;
};

class RejectingToolExecutor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock&) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::invalid_arguments,
        "arguments rejected", false});
  }

  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    ++starts;
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::internal_failure,
        "rejected tool unexpectedly started", false});
  }

  std::size_t starts{};
};

class FakeModels final : public backend::ModelContextProvider {
 public:
  auto lookup(const domain::ModelId& model_id, std::stop_token stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    ++lookups;
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::cancelled, "hidden", false, std::nullopt});
    }
    if (failure) return std::unexpected(*failure);
    auto result = info;
    result.model_id = model_id;
    return result;
  }

  backend::ModelContextInfo info{make_id<domain::ModelId>("model"), 8192, 1024};
  std::optional<backend::BackendError> failure;
  std::size_t lookups{};
};

auto persona_document(std::string text = "Review carefully.")
    -> domain::PersonaDocument {
  return {{make_id<domain::PersonaId>("persona:reviewer"),
           "reviewer",
           "personas/reviewer.md",
           {"sha256", std::string(64, 'a'), text.size()}},
          std::move(text)};
}

class MemoryStore final : public storage::SessionStore {
 public:
  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    ++calls;
    if (sessions.contains(session.session_id)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::already_exists, "session exists",
          false});
    }
    sessions.emplace(session.session_id,
                     storage::SessionInfo{session.session_id,
                                          session.created_at,
                                          session.created_at, 0});
    return {};
  }

  auto open_session(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    ++calls;
    const auto found = sessions.find(session_id);
    if (found == sessions.end()) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "session missing", false});
    }
    return found->second;
  }

  auto list_sessions(std::size_t limit, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    ++calls;
    std::vector<storage::SessionInfo> result;
    for (const auto& [id, info] : sessions) {
      static_cast<void>(id);
      result.push_back(info);
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
      if (left.last_activity_at != right.last_activity_at) {
        return left.last_activity_at > right.last_activity_at;
      }
      return left.session_id < right.session_id;
    });
    if (result.size() > limit) {
      result.erase(result.begin() + static_cast<std::ptrdiff_t>(limit),
                   result.end());
    }
    return result;
  }

  auto append_events(const domain::SessionId& session_id,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    ++calls;
    ++append_calls;
    if (append_failure &&
        (!fail_on_append || append_calls == *fail_on_append)) {
      return std::unexpected(*append_failure);
    }
    const auto found = sessions.find(session_id);
    if (found == sessions.end()) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "session missing", false});
    }
    auto& stored = histories[session_id];
    auto sequence = found->second.last_sequence;
    for (const auto& event : events) {
      if (event.metadata.sequence <= sequence) {
        return std::unexpected(
            storage::SessionStoreError{storage::SessionStoreErrorCode::conflict,
                                       "sequence conflict", false});
      }
      sequence = event.metadata.sequence;
    }
    stored.insert(stored.end(), events.begin(), events.end());
    append_batch_sizes.push_back(events.size());
    if (!events.empty()) {
      found->second.last_sequence = events.back().metadata.sequence;
      found->second.last_activity_at = events.back().metadata.timestamp;
    }
    return {};
  }

  auto replay_events(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    ++calls;
    if (!sessions.contains(session_id)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "session missing", false});
    }
    return histories[session_id];
  }

  std::map<domain::SessionId, storage::SessionInfo> sessions;
  std::map<domain::SessionId, std::vector<domain::RunEvent>> histories;
  std::vector<std::size_t> append_batch_sizes;
  std::optional<storage::SessionStoreError> append_failure;
  std::optional<std::size_t> fail_on_append;
  std::size_t append_calls{};
  std::size_t calls{};
};

class ConversationBackend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    const auto assistant = request.assistant_message_id;
    captured.push_back(std::move(request));
    return std::make_unique<VectorStream>(success_items(assistant, usd_value));
  }

  std::vector<backend::BackendRequest> captured;
  std::string usd_value{"0"};
};

auto success_items(const domain::MessageId& message_id,
                   const std::string& usd_value) -> std::vector<Item> {
  return {
      backend::BackendEvent{backend::ResponseStarted{"response"}},
      backend::BackendEvent{backend::ContentDelta{
          message_id, domain::TextBlock{"hello\x1b[31mred"}}},
      backend::BackendEvent{backend::CitationObserved{
          {"https://example.test/\x1b[2J", "source\nforged\x7f"}}},
      backend::BackendEvent{backend::UsageObserved{{3, 2, 1, 0}}},
      backend::BackendEvent{backend::CostObserved{reported_cost(usd_value)}},
      backend::BackendEvent{
          backend::ResponseFinished{domain::FinishReason::stop}},
      End{},
  };
}

class CancelStream final : public backend::BackendStream {
 public:
  explicit CancelStream(domain::MessageId message_id)
      : m_message_id(std::move(message_id)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_finished) return std::optional<backend::BackendEvent>{};
    if (m_step++ == 0) {
      return backend::BackendEvent{backend::ResponseStarted{"response"}};
    }
    if (m_step == 2) {
      return backend::BackendEvent{
          backend::ContentDelta{m_message_id, domain::TextBlock{"partial"}}};
    }
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any ready;
    ready.wait(lock, stop_token, [] { return false; });
    m_finished = true;
    return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
  }

 private:
  domain::MessageId m_message_id;
  int m_step{};
  bool m_finished{};
};

class CancelBackend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    return std::make_unique<CancelStream>(request.assistant_message_id);
  }
};

} // namespace

TEST_CASE("one-shot streams only sanitized text and builds neutral evidence",
          "[one-shot]") {
  FakeModels models;
  models.info.pricing_observation = pricing_observation();
  // The surface owns assistant identity, so replace the scripted placeholder
  // when the backend sees the request.
  class RewritingBackend final : public backend::Backend {
   public:
    auto start(backend::BackendRequest request, std::stop_token)
        -> std::expected<std::unique_ptr<backend::BackendStream>,
                         backend::BackendError> override {
      captured = request;
      return std::make_unique<VectorStream>(
          success_items(request.assistant_message_id));
    }
    std::optional<backend::BackendRequest> captured;
  } rewriting;

  const backend::ToolDeclaration tool{
      "lookup",
      "Look up a value",
      {"application/schema+json", R"({"type":"object"})"},
      {domain::Effect::read},
      {{domain::Effect::read, "filesystem.root", "/repo"}}};
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      tool, std::make_shared<testing::ScriptedToolExecutor>(
                std::vector<testing::ScriptedToolExchange>{})));
  auto tools = registry.snapshot();
  REQUIRE(tools);
  surfaces::OneShotDependencies dependencies;
  dependencies.tools = std::move(*tools);
  surfaces::OneShotSurface surface{
      rewriting, models, {}, nullptr, std::move(dependencies)};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = surface.run({"explain", std::string{"file contents"},
                                   make_id<domain::ModelId>("model")},
                                  output, error);

  REQUIRE(result);
  REQUIRE(result->usage == domain::Usage{3, 2, 1, 0});
  REQUIRE(result->reported_cost == reported_cost());
  REQUIRE(result->catalog_estimates.size() == 2);
  REQUIRE(result->catalog_estimates.front().subtotal);
  REQUIRE(result->catalog_estimates.front().subtotal->amount().to_string() ==
          "0.0000065");
  REQUIRE(output.str() == "hellored");
  REQUIRE(error.str().find("citation: https://example.test/") !=
          std::string::npos);
  REQUIRE(error.str().find("\x1b") == std::string::npos);
  REQUIRE(error.str().find("\nforged") == std::string::npos);
  REQUIRE(error.str().find("usage: input=3 output=2 cached=1 reasoning=0") !=
          std::string::npos);
  REQUIRE(error.str().find(
              "cost: USD=0 venice.diem=0.0645375 (provider-reported)") !=
          std::string::npos);
  REQUIRE(
      error.str().find(
          "estimate: USD=0.0000065 venice.diem=0.0000065 (catalog-derived)") !=
      std::string::npos);
  REQUIRE(rewriting.captured);
  REQUIRE(rewriting.captured->tools ==
          std::vector<backend::ToolDeclaration>{tool});
  REQUIRE(rewriting.captured->context.entries.size() == 3);
  REQUIRE(rewriting.captured->context.entries.back().kind ==
          domain::ContextEntryKind::evidence);
  REQUIRE(rewriting.captured->context.entries.back().message.role ==
          domain::Role::evidence);
  REQUIRE(
      rewriting.captured->context.entries.back().provenance.source_location ==
      "stdin");
}

TEST_CASE("one-shot continues after a rejected tool result",
          "[one-shot][tools][failure]") {
  FakeModels models;
  ToolLoopBackend backend{"{"};
  auto executor = std::make_shared<RejectingToolExecutor>();
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      {"lookup",
       "Look up a value",
       {"application/schema+json", R"({"type":"object"})"},
       {},
       {}},
      executor));
  auto tools = registry.snapshot();
  REQUIRE(tools);
  surfaces::OneShotDependencies dependencies;
  dependencies.tools = std::move(*tools);
  surfaces::OneShotSurface surface{
      backend, models, {}, nullptr, std::move(dependencies)};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = surface.run(
      {"use the lookup", std::nullopt, make_id<domain::ModelId>("model")},
      output, error);

  REQUIRE(result);
  REQUIRE(executor->starts == 0);
  REQUIRE(backend.captured.size() == 2);
  const auto& continuation = backend.captured.back();
  REQUIRE(continuation.context.entries.back().kind ==
          domain::ContextEntryKind::tool_result);
  REQUIRE(continuation.context.entries.back().message.role ==
          domain::Role::tool);
  REQUIRE(continuation.context.entries.back().message.invocation_id ==
          make_id<domain::InvocationId>("lookup-call"));
}

TEST_CASE("one-shot continues after a successful tool result",
          "[one-shot][tools]") {
  FakeModels models;
  ToolLoopBackend backend;
  const auto invocation = make_id<domain::InvocationId>("lookup-call");
  auto executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{
          {{invocation,
            std::nullopt,
            "lookup",
            runtime::ValidatedToolArguments{{"application/json", "{}"}},
            {},
            {}},
           testing::ToolStreamScript{{
               runtime::ToolExecutionEvent{
                   runtime::ToolResult{{domain::TextBlock{"done"}}}},
               testing::ToolEndOfStream{},
           }}},
      });
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      {"lookup",
       "Look up a value",
       {"application/schema+json", R"({"type":"object"})"},
       {},
       {}},
      executor));
  auto tools = registry.snapshot();
  REQUIRE(tools);
  surfaces::OneShotDependencies dependencies;
  dependencies.tools = std::move(*tools);
  surfaces::OneShotSurface surface{
      backend, models, {}, nullptr, std::move(dependencies)};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = surface.run(
      {"use the lookup", std::nullopt, make_id<domain::ModelId>("model")},
      output, error);

  REQUIRE(result);
  REQUIRE(executor->remaining_exchanges() == 0);
  REQUIRE(backend.captured.size() == 2);
  const auto& continuation = backend.captured.back();
  REQUIRE(continuation.context.entries.back().kind ==
          domain::ContextEntryKind::tool_result);
  REQUIRE(
      continuation.context.entries.back().message ==
      domain::Message{continuation.context.entries.back().message.message_id,
                      domain::Role::tool,
                      {domain::TextBlock{"done"}},
                      invocation});
}

TEST_CASE("invalid and oversized one-shot input never reaches a backend",
          "[one-shot][failure]") {
  FakeModels models;
  CapturingBackend backend{{}};
  surfaces::OneShotSurface surface{backend, models, {8, 2}};
  std::ostringstream output;
  std::ostringstream error;
  const auto model = make_id<domain::ModelId>("model");

  auto result = surface.run({"", std::nullopt, model}, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::invalid_input);

  result = surface.run({std::string{"bad\0text", 8}, std::nullopt, model},
                       output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::invalid_input);

  result = surface.run({std::string{"\xc0\xaf", 2}, std::nullopt, model},
                       output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::invalid_input);

  result = surface.run({"12345678", std::string{"x"}, model}, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::input_too_large);
  REQUIRE(backend.starts == 0);
  REQUIRE(models.lookups == 0);
}

TEST_CASE("model and context failures are typed before inference",
          "[one-shot][failure]") {
  FakeModels models;
  models.failure = backend::BackendError{backend::BackendErrorKind::network,
                                         "secret provider body", true, 500};
  CapturingBackend backend{{}};
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  std::ostringstream error;
  auto result =
      surface.run({"hello", std::nullopt, make_id<domain::ModelId>("model")},
                  output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          surfaces::OneShotErrorCode::model_lookup_failed);
  REQUIRE(result.error().message.find("secret") == std::string::npos);

  models.failure.reset();
  models.info.context_window_tokens = 100;
  models.info.maximum_output_tokens = 50;
  surfaces::OneShotSurface small{backend, models, {1024, 50}};
  result = small.run(
      {std::string(60, 'x'), std::nullopt, make_id<domain::ModelId>("model")},
      output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::context_failed);
  REQUIRE(backend.starts == 0);
}

TEST_CASE("backend failure preserves partial output and redacts diagnostics",
          "[one-shot][failure]") {
  FakeModels models;
  class PartialFailureBackend final : public backend::Backend {
   public:
    auto start(backend::BackendRequest request, std::stop_token)
        -> std::expected<std::unique_ptr<backend::BackendStream>,
                         backend::BackendError> override {
      std::vector<Item> items{
          backend::BackendEvent{backend::ResponseStarted{"response"}},
          backend::BackendEvent{backend::ContentDelta{
              request.assistant_message_id, domain::TextBlock{"partial"}}},
          backend::BackendError{backend::BackendErrorKind::authentication,
                                "secret-token", false, 401},
          End{}};
      return std::make_unique<VectorStream>(std::move(items));
    }
  } backend;
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  std::ostringstream error;
  const auto result =
      surface.run({"hello", std::nullopt, make_id<domain::ModelId>("model")},
                  output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::run_failed);
  REQUIRE(result.error().message == "backend authentication failed");
  REQUIRE(result.error().message.find("secret-token") == std::string::npos);
  REQUIRE(output.str() == "partial");
}

TEST_CASE("one-shot cancellation keeps streamed partial content",
          "[one-shot][failure][cancel]") {
  FakeModels models;
  CancelBackend backend;
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  std::ostringstream error;
  std::stop_source cancellation;
  std::jthread requester{[&] {
    std::this_thread::sleep_for(20ms);
    cancellation.request_stop();
  }};
  const auto result =
      surface.run({"hello", std::nullopt, make_id<domain::ModelId>("model")},
                  output, error, cancellation.get_token());
  REQUIRE_FALSE(result);
  INFO(result.error().message);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::cancelled);
  REQUIRE(output.str() == "partial");
}

TEST_CASE("broken output cancels the one-shot operation",
          "[one-shot][failure][output]") {
  FakeModels models;
  class TextBackend final : public backend::Backend {
   public:
    auto start(backend::BackendRequest request, std::stop_token)
        -> std::expected<std::unique_ptr<backend::BackendStream>,
                         backend::BackendError> override {
      return std::make_unique<VectorStream>(std::vector<Item>{
          backend::BackendEvent{backend::ResponseStarted{"response"}},
          backend::BackendEvent{backend::ContentDelta{
              request.assistant_message_id, domain::TextBlock{"answer"}}},
          backend::BackendEvent{
              backend::ResponseFinished{domain::FinishReason::stop}},
          End{}});
    }
  } backend;
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  output.setstate(std::ios::badbit);
  std::ostringstream error;
  const auto result =
      surface.run({"hello", std::nullopt, make_id<domain::ModelId>("model")},
                  output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::output_failed);
}

TEST_CASE("durable one-shot sessions resume completed conversation in order",
          "[one-shot][session]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;
  const auto model = make_id<domain::ModelId>("model");

  auto first = surface.run({"first", std::nullopt, model}, output, error);
  REQUIRE(first);
  REQUIRE(first->durable);
  REQUIRE(store.sessions.contains(first->session_id));
  REQUIRE(store.append_batch_sizes.front() == 4);
  REQUIRE(store.append_batch_sizes.back() == 3);
  REQUIRE(
      error.str().find("session: " + std::string{first->session_id.value()}) !=
      std::string::npos);
  const auto first_event_count = store.histories[first->session_id].size();
  REQUIRE(first_event_count > 4);

  output.str({});
  error.str({});
  auto resumed = surface.run({"second", std::nullopt, model,
                              surfaces::OneShotRequest::SessionMode::resume,
                              first->session_id},
                             output, error);
  REQUIRE(resumed);
  REQUIRE(resumed->session_id == first->session_id);
  REQUIRE(backend.captured.size() == 2);
  std::vector<domain::Role> conversation_roles;
  for (const auto& entry : backend.captured.back().context.entries) {
    if (entry.kind == domain::ContextEntryKind::conversation) {
      conversation_roles.push_back(entry.message.role);
    }
  }
  REQUIRE(conversation_roles == std::vector{domain::Role::user,
                                            domain::Role::assistant,
                                            domain::Role::user});
  const auto history_source = std::ranges::find_if(
      backend.captured.back().context.entries, [](const auto& entry) {
        return entry.provenance.source_location &&
               entry.provenance.source_location->starts_with("session:");
      });
  REQUIRE(history_source != backend.captured.back().context.entries.end());

  const auto& persisted = store.histories[first->session_id];
  REQUIRE(persisted.size() > first_event_count);
  for (std::size_t index = 1; index < persisted.size(); ++index) {
    REQUIRE(persisted[index].metadata.sequence >
            persisted[index - 1].metadata.sequence);
  }

  output.str({});
  error.str({});
  auto continued =
      surface.run({"third", std::nullopt, model,
                   surfaces::OneShotRequest::SessionMode::continue_latest},
                  output, error);
  REQUIRE(continued);
  REQUIRE(continued->session_id == first->session_id);
}

TEST_CASE("one-shot spend ceiling allows crossing then blocks durable resume",
          "[one-shot][spend][session][failure]") {
  FakeModels models;
  ConversationBackend backend;
  backend.usd_value = "1.25";
  MemoryStore store;
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;
  const auto model = make_id<domain::ModelId>("model");
  const auto cap = domain::SessionSpendCeiling::from("1").value();

  auto first = surface.run({"first",
                            std::nullopt,
                            model,
                            surfaces::OneShotRequest::SessionMode::create,
                            std::nullopt,
                            std::nullopt,
                            {},
                            cap},
                           output, error);
  REQUIRE(first);
  REQUIRE(first->spend);
  REQUIRE(first->spend->reached);
  REQUIRE(first->spend->accounted->amount().to_string() == "1.25");
  REQUIRE(error.str().find("spend: USD=1.25 cap=1 remaining=0 reached") !=
          std::string::npos);
  REQUIRE(backend.captured.size() == 1);

  output.str({});
  error.str({});
  auto blocked = surface.run({"second", std::nullopt, model,
                              surfaces::OneShotRequest::SessionMode::resume,
                              first->session_id},
                             output, error);
  REQUIRE_FALSE(blocked);
  REQUIRE(blocked.error().code ==
          surfaces::OneShotErrorCode::spend_ceiling_reached);
  REQUIRE(backend.captured.size() == 1);

  auto widened = surface.run({"wider",
                              std::nullopt,
                              model,
                              surfaces::OneShotRequest::SessionMode::resume,
                              first->session_id,
                              std::nullopt,
                              {},
                              domain::SessionSpendCeiling::from("2").value()},
                             output, error);
  REQUIRE_FALSE(widened);
  REQUIRE(widened.error().code == surfaces::OneShotErrorCode::invalid_input);
  REQUIRE(backend.captured.size() == 1);
}

TEST_CASE("one-shot personas are injected recorded and identity-checked",
          "[one-shot][persona][session][failure]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  const auto original = persona_document();
  const auto changed = persona_document("Changed instructions.");
  testing::ScriptedPersonaSource personas{
      {},
      {{"reviewer", original}, {"reviewer", original}, {"reviewer", changed}}};
  surfaces::OneShotSurface surface{backend, models, store, {}, &personas};
  std::ostringstream output;
  std::ostringstream error;
  const auto model = make_id<domain::ModelId>("model");

  const auto first =
      surface.run({"first",
                   std::nullopt,
                   model,
                   surfaces::OneShotRequest::SessionMode::create,
                   std::nullopt,
                   std::nullopt,
                   {persona::PersonaDirectiveKind::select, "reviewer",
                    domain::PersonaSelectionSource::command_line}},
                  output, error);
  REQUIRE(first);
  REQUIRE(backend.captured.size() == 1);
  const auto persona_entry = std::ranges::find_if(
      backend.captured.front().context.entries, [](const auto& entry) {
        return entry.instruction_layer == domain::InstructionLayer::persona;
      });
  REQUIRE(persona_entry != backend.captured.front().context.entries.end());
  REQUIRE(persona_entry->provenance.source_location ==
          original.reference.source_location);
  REQUIRE(persona_entry->provenance.digest ==
          "sha256:" + original.reference.content_digest.value);
  const auto recorded = std::ranges::find_if(
      store.histories[first->session_id], [](const auto& event) {
        return std::holds_alternative<domain::PersonaSelectionRecorded>(
            event.payload);
      });
  REQUIRE(recorded != store.histories[first->session_id].end());
  REQUIRE(std::get<domain::PersonaSelectionRecorded>(recorded->payload)
              .selection.persona == original.reference);

  output.str({});
  error.str({});
  const auto resumed = surface.run(
      {"second", std::nullopt, model,
       surfaces::OneShotRequest::SessionMode::resume, first->session_id},
      output, error);
  REQUIRE(resumed);
  REQUIRE(backend.captured.size() == 2);

  output.str({});
  error.str({});
  const auto rejected = surface.run(
      {"third", std::nullopt, model,
       surfaces::OneShotRequest::SessionMode::resume, first->session_id},
      output, error);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == surfaces::OneShotErrorCode::context_failed);
  REQUIRE(backend.captured.size() == 2);
}

TEST_CASE("one-shot runs persist provenance and refuse a sensitive value",
          "[one-shot][session][provenance][failure]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;
  const auto model = make_id<domain::ModelId>("model");

  auto secret = run_provenance();
  secret.configuration.front().sensitive = true;
  const auto refused = surface.run(
      {"first", std::nullopt, model,
       surfaces::OneShotRequest::SessionMode::create, std::nullopt, secret},
      output, error);
  REQUIRE_FALSE(refused);
  REQUIRE(refused.error().code == surfaces::OneShotErrorCode::run_failed);
  for (const auto& [session, history] : store.histories) {
    static_cast<void>(session);
    REQUIRE(history.empty());
  }

  output.str({});
  error.str({});
  const auto result =
      surface.run({"first", std::nullopt, model,
                   surfaces::OneShotRequest::SessionMode::create, std::nullopt,
                   run_provenance()},
                  output, error);
  REQUIRE(result);
  const auto& persisted = store.histories[result->session_id];
  REQUIRE(persisted.size() > 1);
  REQUIRE(std::holds_alternative<domain::RunStarted>(persisted[0].payload));
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&persisted[1].payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance.backend_id == "venice");
  REQUIRE(recorded->provenance.credential_source->identity == "VENICE_API_KEY");
  // Only the locator is durable; a sensitive key keeps presence without value.
  REQUIRE(recorded->provenance.configuration.back().sensitive);
  REQUIRE_FALSE(recorded->provenance.configuration.back().value.has_value());
}

TEST_CASE("ephemeral one-shot bypasses an available durable store",
          "[one-shot][session]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;

  const auto result =
      surface.run({"temporary", std::nullopt, make_id<domain::ModelId>("model"),
                   surfaces::OneShotRequest::SessionMode::ephemeral},
                  output, error);
  REQUIRE(result);
  REQUIRE_FALSE(result->durable);
  REQUIRE(store.calls == 0);
  REQUIRE(error.str().find("(ephemeral)") != std::string::npos);
}

TEST_CASE("session failures occur before provider start and retain prior truth",
          "[one-shot][session][failure]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  store.append_failure =
      storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                 "sensitive filesystem detail", true};
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;

  auto result = surface.run(
      {"never sent", std::nullopt, make_id<domain::ModelId>("model")}, output,
      error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::run_failed);
  REQUIRE(result.error().message.find("sensitive") == std::string::npos);
  REQUIRE(backend.captured.empty());
  REQUIRE(store.sessions.size() == 1);
  REQUIRE(store.histories.empty());

  MemoryStore empty;
  surfaces::OneShotSurface empty_surface{backend, models, empty};
  result = empty_surface.run(
      {"continue", std::nullopt, make_id<domain::ModelId>("model"),
       surfaces::OneShotRequest::SessionMode::continue_latest},
      output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::invalid_input);
  REQUIRE(backend.captured.empty());
}

TEST_CASE("terminal persistence failure publishes none of its event batch",
          "[one-shot][session][failure]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  store.append_failure =
      storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                 "sensitive filesystem detail", true};
  store.fail_on_append = 6;
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = surface.run(
      {"persist atomically", std::nullopt, make_id<domain::ModelId>("model")},
      output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::run_failed);
  REQUIRE(backend.captured.size() == 1);
  REQUIRE(store.sessions.size() == 1);
  REQUIRE(store.histories.size() == 1);
  const auto& events = store.histories.begin()->second;
  REQUIRE(std::ranges::none_of(events, [](const auto& event) {
    return std::holds_alternative<domain::AssistantContentFinished>(
               event.payload) ||
           std::holds_alternative<domain::InferenceFinished>(event.payload) ||
           std::holds_alternative<domain::RunCompleted>(event.payload);
  }));
}

TEST_CASE("spend ceiling persistence fails atomically before provider start",
          "[one-shot][spend][session][failure]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  store.append_failure = storage::SessionStoreError{
      storage::SessionStoreErrorCode::io_failure, "sensitive detail", true};
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;

  const auto result =
      surface.run({"bounded",
                   std::nullopt,
                   make_id<domain::ModelId>("model"),
                   surfaces::OneShotRequest::SessionMode::create,
                   std::nullopt,
                   std::nullopt,
                   {},
                   domain::SessionSpendCeiling::from("1").value()},
                  output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::run_failed);
  REQUIRE(result.error().message.find("sensitive") == std::string::npos);
  REQUIRE(backend.captured.empty());
  REQUIRE(store.sessions.size() == 1);
  REQUIRE(store.histories.empty());
}

TEST_CASE("semantically corrupt replay is rejected without provider work",
          "[one-shot][session][replay][failure]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  const auto session_id = make_id<domain::SessionId>("corrupt-session");
  const auto timestamp = domain::EventTimestamp{std::chrono::milliseconds{100}};
  store.sessions.emplace(
      session_id, storage::SessionInfo{session_id, timestamp, timestamp, 1});
  store.histories[session_id] = {domain::RunEvent{
      {make_id<domain::EventId>("bad-event"), make_id<domain::RunId>("bad-run"),
       1, 1, timestamp, std::nullopt, std::nullopt, std::nullopt},
      domain::RunCompleted{}}};
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = surface.run(
      {"do not send", std::nullopt, make_id<domain::ModelId>("model"),
       surfaces::OneShotRequest::SessionMode::resume, session_id},
      output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::run_failed);
  REQUIRE(backend.captured.empty());
}

TEST_CASE("cancelled partial assistant content stays out of resumed context",
          "[one-shot][session][replay]") {
  FakeModels models;
  ConversationBackend backend;
  MemoryStore store;
  const auto session_id = make_id<domain::SessionId>("cancelled-session");
  const auto run_id = make_id<domain::RunId>("cancelled-run");
  const auto inference_id = make_id<domain::InferenceId>("cancelled-inference");
  const auto user_id = make_id<domain::MessageId>("cancelled-user");
  const auto assistant_id = make_id<domain::MessageId>("cancelled-assistant");
  const auto timestamp = domain::EventTimestamp{std::chrono::milliseconds{100}};
  const auto event = [&](const std::uint64_t sequence,
                         domain::RunEventPayload payload) {
    return domain::RunEvent{
        {make_id<domain::EventId>("cancelled-event-" +
                                  std::to_string(sequence)),
         run_id, sequence, 1,
         domain::EventTimestamp{std::chrono::milliseconds{sequence}},
         std::nullopt, std::nullopt, std::nullopt},
        std::move(payload)};
  };
  store.sessions.emplace(
      session_id, storage::SessionInfo{session_id, timestamp, timestamp, 8});
  store.histories[session_id] = {
      event(1,
            domain::RunStarted{make_id<domain::SurfaceId>("one-shot"),
                               make_id<domain::WorkspaceId>("chat"),
                               make_id<domain::PermissionProfileId>("observe"),
                               std::nullopt}),
      event(2, domain::UserContentAdded{{user_id,
                                         domain::Role::user,
                                         {domain::TextBlock{"old prompt"}},
                                         std::nullopt}}),
      event(3, domain::RunCompletionRequested{}),
      event(4, domain::InferenceStarted{inference_id,
                                        make_id<domain::ModelId>("old-model")}),
      event(5, domain::AssistantContentStarted{assistant_id, inference_id}),
      event(6,
            domain::AssistantContentDeltaAdded{assistant_id, inference_id,
                                               domain::TextBlock{"partial"}}),
      event(7, domain::InferenceCancelled{inference_id, "cancelled"}),
      event(8, domain::RunCancelled{"cancelled"})};
  surfaces::OneShotSurface surface{backend, models, store};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = surface.run(
      {"new prompt", std::nullopt, make_id<domain::ModelId>("model"),
       surfaces::OneShotRequest::SessionMode::resume, session_id},
      output, error);
  REQUIRE(result);
  std::vector<domain::Role> roles;
  for (const auto& entry : backend.captured.back().context.entries) {
    if (entry.kind == domain::ContextEntryKind::conversation) {
      roles.push_back(entry.message.role);
    }
  }
  REQUIRE(roles == std::vector{domain::Role::user, domain::Role::user});
  REQUIRE(std::ranges::none_of(
      backend.captured.back().context.entries, [](const auto& entry) {
        return std::ranges::any_of(
            entry.message.content, [](const auto& block) {
              const auto* text = std::get_if<domain::TextBlock>(&block);
              return text != nullptr && text->text == "partial";
            });
      }));
}
