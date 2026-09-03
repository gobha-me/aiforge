#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_session_store.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto reported_cost(const std::string_view value = "0.0645375")
    -> domain::ReportedCost {
  auto amount = domain::MonetaryAmount::create(
                    "venice.diem", domain::DecimalAmount::from(value).value())
                    .value();
  return domain::ReportedCost::create({std::move(amount)}).value();
}

auto context() -> domain::ConstructedContext {
  return domain::ConstructedContext{
      {domain::ContextEntry{
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
          2}},
      {{make_id<domain::ContextEntryId>("runtime-context"),
        domain::ContextDecision::admitted, std::nullopt}},
      {4096, 512, 0},
      2};
}

auto request(std::vector<backend::ToolDeclaration> tools = {})
    -> backend::BackendRequest {
  return backend::BackendRequest{make_id<domain::InferenceId>("inference"),
                                 make_id<domain::MessageId>("assistant"),
                                 make_id<domain::ModelId>("model"),
                                 context(),
                                 std::move(tools),
                                 {0.25, 128, 42, {}, {}}};
}

auto run_start(backend::BackendRequest backend_request = request())
    -> runtime::RunStart {
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
              domain::ProvenanceDisposition::selected, std::nullopt}}}},
          {{"aiforge", "0.10.0"}},
          {}};
}

template <typename Payload>
auto durable_event(const domain::RunId& run, const std::uint64_t sequence,
                   std::string event_id, Payload payload) -> domain::RunEvent {
  return {domain::EventMetadata{
              make_id<domain::EventId>(std::move(event_id)), run, sequence, 1,
              domain::EventTimestamp{std::chrono::milliseconds{100 + sequence}},
              std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

auto step(backend::BackendEvent event) -> testing::ScriptedStep {
  return testing::ScriptedStep{std::move(event)};
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

  auto wait_for_change(std::size_t previous) -> bool {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, 1s, [&] { return m_count > previous; });
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

auto drain_to_end(runtime::RunKernel& kernel, WakeCounter& wake)
    -> std::vector<domain::RunEvent> {
  std::vector<domain::RunEvent> result;
  std::size_t observed_wakes{};
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    auto drained = kernel.drain();
    REQUIRE(drained);
    result.insert(result.end(), std::make_move_iterator(drained->begin()),
                  std::make_move_iterator(drained->end()));
    if (kernel.active_run_id() && drained->empty()) {
      static_cast<void>(wake.wait_for_change(observed_wakes));
    }
    observed_wakes = wake.count();
  }
  REQUIRE_FALSE(kernel.active_run_id());
  return result;
}

class CancelStream final : public backend::BackendStream {
 public:
  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_finished) return std::optional<backend::BackendEvent>{};
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any changed;
    changed.wait(lock, stop_token, [] { return false; });
    m_finished = true;
    return std::optional<backend::BackendEvent>{
        backend::ResponseCancelled{"transport cancelled"}};
  }

 private:
  bool m_finished{};
};

class CancelBackend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest, std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(
          backend::BackendError{backend::BackendErrorKind::cancelled,
                                "must-not-leak", false, std::nullopt});
    }
    return std::make_unique<CancelStream>();
  }
};

class BetweenDeltaStream final : public backend::BackendStream {
 public:
  explicit BetweenDeltaStream(domain::MessageId message_id)
      : m_message_id(std::move(message_id)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_step == 0) {
      ++m_step;
      return backend::BackendEvent{backend::ResponseStarted{"response"}};
    }
    if (m_step == 1) {
      ++m_step;
      return backend::BackendEvent{
          backend::ContentDelta{m_message_id, domain::TextBlock{"partial"}}};
    }
    if (m_step == 2) {
      std::mutex mutex;
      std::unique_lock lock(mutex);
      std::condition_variable_any changed;
      changed.wait(lock, stop_token, [] { return false; });
      ++m_step;
      return backend::BackendEvent{
          backend::ResponseCancelled{"transport cancelled"}};
    }
    return std::optional<backend::BackendEvent>{};
  }

 private:
  domain::MessageId m_message_id;
  int m_step{};
};

class BetweenDeltaBackend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    return std::make_unique<BetweenDeltaStream>(request.assistant_message_id);
  }
};

class PassiveToolExecutor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{arguments};
  }

  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::unavailable, "passive test executor",
        false});
  }
};

} // namespace

TEST_CASE("run kernel rejects invalid limits before starting a worker",
          "[runtime][failure]") {
  testing::ScriptedBackend fake{{}};
  runtime::RunKernel kernel{
      make_id<domain::SessionId>("session"), fake, nullptr, {}, {0, 1}};
  const auto started = kernel.start(run_start());
  REQUIRE_FALSE(started);
  REQUIRE(started.error().code == runtime::RunKernelErrorCode::invalid_limits);
  REQUIRE(kernel.event_log().events().empty());
}

TEST_CASE("run kernel rejects persona provenance that does not match context",
          "[runtime][persona][failure]") {
  testing::ScriptedBackend fake{{}};
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake};
  auto start = run_start();
  const domain::PersonaReference reference{
      make_id<domain::PersonaId>("persona:reviewer"),
      "reviewer",
      "personas/reviewer.md",
      {"sha256", std::string(64, 'a'), 7}};
  start.attributes.persona_id = reference.persona_id;
  start.persona_selection = domain::PersonaSelection{
      domain::PersonaSelectionAction::selected,
      domain::PersonaSelectionSource::command_line, reference, std::nullopt};

  const auto missing_context = kernel.start(start);
  REQUIRE_FALSE(missing_context);
  REQUIRE(missing_context.error().code ==
          runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());

  start.request.context.entries.push_back(domain::ContextEntry{
      make_id<domain::ContextEntryId>("persona-entry"),
      domain::ContextEntryKind::instruction,
      domain::InstructionLayer::persona,
      domain::Message{make_id<domain::MessageId>("persona-message"),
                      domain::Role::system,
                      {domain::TextBlock{"review"}},
                      std::nullopt},
      {make_id<domain::ContextSourceId>("persona-source"),
       reference.source_location,
       reference.content_digest.algorithm + ":" +
           reference.content_digest.value},
      0,
      2,
      2});
  start.request.context.entries.push_back(start.request.context.entries.back());
  start.request.context.decisions.push_back(
      {make_id<domain::ContextEntryId>("persona-entry"),
       domain::ContextDecision::admitted, std::nullopt});
  const auto duplicate_context = kernel.start(std::move(start));
  REQUIRE_FALSE(duplicate_context);
  REQUIRE(duplicate_context.error().code ==
          runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());
}

TEST_CASE("premature backend EOF becomes a redacted failed run",
          "[runtime][failure]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{step(backend::ResponseStarted{"response"}),
                             testing::EndOfStream{}}}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));

  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::failed);
  REQUIRE(kernel.event_log().events().size() == 7);
}

TEST_CASE("a delta before response start fails closed", "[runtime][failure]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ContentDelta{backend_request.assistant_message_id,
                                     domain::TextBlock{"early"}}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::failed);
  REQUIRE(projection->messages().size() == 1);
}

TEST_CASE("reasoning continuation state respects resource and text bounds",
          "[runtime][reasoning][failure]") {
  std::vector<backend::ReasoningDelta> cases;
  cases.push_back({std::string{}, {}});
  cases.push_back({std::nullopt, {}});
  cases.push_back({std::string{"escape\x1b"}, {}});
  cases.push_back({std::string{"\xc3", 1}, {}});
  cases.push_back({std::string(1024U * 1024U + 1, 'x'), {}});
  cases.push_back(
      {std::nullopt, domain::Metadata(257, {"media/type", "value"})});
  cases.push_back({std::nullopt, {{"", "value"}}});
  cases.push_back({std::nullopt, {{std::string(257, 'x'), "value"}}});
  cases.push_back(
      {std::nullopt, {{"media/type", std::string(1024U * 1024U + 1, 'x')}}});
  cases.push_back({std::nullopt,
                   {{"first", std::string(512U * 1024U, 'x')},
                    {"second", std::string(512U * 1024U, 'x')}}});
  cases.push_back({std::nullopt, {{"media/type", "line\nbreak"}}});

  for (auto& reasoning : cases) {
    auto backend_request = request();
    testing::ScriptedBackend fake{{testing::ScriptedExchange{
        backend_request, testing::StreamScript{{
                             step(backend::ResponseStarted{"response"}),
                             step(std::move(reasoning)),
                             testing::EndOfStream{},
                         }}}}};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                              &wake};

    REQUIRE(kernel.start(run_start(backend_request)));
    static_cast<void>(drain_to_end(kernel, wake));
    const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
    REQUIRE(projection != nullptr);
    REQUIRE(projection->status() == domain::RunStatus::failed);
  }

  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ReasoningDelta{std::string(1024U * 1024U, 'x'), {}}),
          step(backend::ReasoningDelta{std::string{"x"}, {}}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};
  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::failed);
  REQUIRE(
      std::ranges::count_if(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::ReasoningMetadataAdded>(
            event.payload);
      }) == 1);
}

TEST_CASE("backend failure text is structurally excluded from run events",
          "[runtime][failure][redaction]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      backend::BackendError{backend::BackendErrorKind::authentication,
                            "secret-token-and-header", false, 401}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  std::size_t failures{};
  for (const auto& event : kernel.event_log().events()) {
    if (const auto* failed = std::get_if<domain::RunFailed>(&event.payload)) {
      ++failures;
      REQUIRE(failed->error.message == "backend authentication failed");
      REQUIRE(failed->error.message.find("secret-token") == std::string::npos);
    }
  }
  REQUIRE(failures == 1);
}

TEST_CASE("bounded redacted backend protocol diagnostics reach run events",
          "[runtime][failure][redaction]") {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"Venice stream repeated provider cost",
       "Venice stream repeated provider cost"},
      {"", "backend protocol failure"},
      {std::string(257, 'x'), "backend protocol failure"},
      {std::string{"line\nbreak"}, "backend protocol failure"},
  };
  for (const auto& [diagnostic, expected] : cases) {
    CAPTURE(diagnostic.size());
    auto backend_request = request();
    testing::ScriptedBackend fake{{testing::ScriptedExchange{
        backend_request,
        backend::BackendError{backend::BackendErrorKind::protocol, diagnostic,
                              false, std::nullopt}}}};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                              &wake};

    REQUIRE(kernel.start(run_start(backend_request)));
    static_cast<void>(drain_to_end(kernel, wake));
    const auto failed = std::ranges::find_if(
        kernel.event_log().events(), [](const domain::RunEvent& event) {
          return std::holds_alternative<domain::RunFailed>(event.payload);
        });
    REQUIRE(failed != kernel.event_log().events().end());
    REQUIRE(std::get<domain::RunFailed>(failed->payload).error.message ==
            expected);
  }
}

TEST_CASE("bounded redacted request diagnostics reach run events",
          "[runtime][failure][redaction]") {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"Venice continuation state type is unsupported",
       "Venice continuation state type is unsupported"},
      {"", "backend rejected the request"},
      {std::string(257, 'x'), "backend rejected the request"},
      {std::string{"line\nbreak"}, "backend rejected the request"},
  };
  for (const auto& [diagnostic, expected] : cases) {
    CAPTURE(diagnostic.size());
    auto backend_request = request();
    testing::ScriptedBackend fake{{testing::ScriptedExchange{
        backend_request,
        backend::BackendError{backend::BackendErrorKind::request_rejected,
                              diagnostic, false, std::nullopt}}}};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                              &wake};

    REQUIRE(kernel.start(run_start(backend_request)));
    static_cast<void>(drain_to_end(kernel, wake));
    const auto failed = std::ranges::find_if(
        kernel.event_log().events(), [](const domain::RunEvent& event) {
          return std::holds_alternative<domain::RunFailed>(event.payload);
        });
    REQUIRE(failed != kernel.event_log().events().end());
    REQUIRE(std::get<domain::RunFailed>(failed->payload).error.message ==
            expected);
  }
}

TEST_CASE("missing backend credentials use a fixed replayable failure",
          "[runtime][failure][credentials]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      backend::BackendError{backend::BackendErrorKind::credential_unavailable,
                            "provider-secret-must-not-cross", false,
                            std::nullopt}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto failed = std::ranges::find_if(
      kernel.event_log().events(), [](const domain::RunEvent& event) {
        return std::holds_alternative<domain::RunFailed>(event.payload);
      });
  REQUIRE(failed != kernel.event_log().events().end());
  const auto& error = std::get<domain::RunFailed>(failed->payload).error;
  REQUIRE(error.message == "backend credential is not configured");
  REQUIRE(error.message.find("provider-secret") == std::string::npos);
}

TEST_CASE("streaming lifecycle preserves content citations reasoning and usage",
          "[runtime]") {
  auto backend_request = request();
  const auto assistant = backend_request.assistant_message_id;
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ReasoningDelta{"brief", {{"kind", "summary"}}}),
          step(backend::ContentDelta{assistant, domain::TextBlock{"hello"}}),
          step(backend::CitationObserved{{"https://example.test", "source"}}),
          step(backend::UsageObserved{{2, 1, 0, 1}}),
          step(backend::CostObserved{reported_cost()}),
          step(backend::ResponseFinished{domain::FinishReason::stop}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  std::int64_t tick{};
  runtime::RunKernel kernel{
      make_id<domain::SessionId>("session"), fake, &wake, [&] {
        return domain::EventTimestamp{std::chrono::milliseconds{++tick}};
      }};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));

  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::completed);
  REQUIRE(projection->messages().size() == 2);
  REQUIRE(projection->messages().back().complete);
  REQUIRE(projection->messages().back().content.size() == 2);
  REQUIRE(projection->usage() == domain::Usage{2, 1, 0, 1});
  REQUIRE(projection->reported_cost() == reported_cost());
  REQUIRE(kernel.event_log().events().size() == 13);
  REQUIRE(kernel.event_log().events().front().metadata.timestamp ==
          domain::EventTimestamp{1ms});
}

TEST_CASE("generated artifacts become one typed assistant reference",
          "[runtime][artifact][image]") {
  const domain::ArtifactMetadata artifact{
      make_id<domain::ArtifactId>("image-inference"),
      "image/png",
      72,
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      std::nullopt,
      2,
      1,
      make_id<domain::InferenceId>("inference")};
  testing::ScriptedBackend backend{{
      {request(),
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::ImageArtifactProduced{artifact}),
           step(backend::ResponseFinished{domain::FinishReason::stop}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), backend,
                            &wake};
  REQUIRE(kernel.start(run_start()));
  const auto events = drain_to_end(kernel, wake);
  CHECK(std::ranges::count_if(events, [](const domain::RunEvent& event) {
          return std::holds_alternative<domain::ArtifactCreated>(event.payload);
        }) == 1);
  CHECK(std::ranges::count_if(events, [](const domain::RunEvent& event) {
          return std::holds_alternative<domain::ArtifactReferenced>(
              event.payload);
        }) == 1);
  const auto delta = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::AssistantContentDeltaAdded>(
        event.payload);
  });
  REQUIRE(delta != events.end());
  const auto& content =
      std::get<domain::AssistantContentDeltaAdded>(delta->payload).delta;
  REQUIRE(std::holds_alternative<domain::ArtifactReferenceBlock>(content));
  CHECK(std::get<domain::ArtifactReferenceBlock>(content).artifact_id ==
        artifact.artifact_id);
}

TEST_CASE("duplicate generated artifact observations fail closed",
          "[runtime][artifact][image][failure]") {
  const domain::ArtifactMetadata artifact{
      make_id<domain::ArtifactId>("image-inference"),
      "image/png",
      72,
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      std::nullopt,
      2,
      1,
      make_id<domain::InferenceId>("inference")};
  testing::ScriptedBackend backend{{
      {request(),
       testing::StreamScript{{
           step(backend::ResponseStarted{"response"}),
           step(backend::ImageArtifactProduced{artifact}),
           step(backend::ImageArtifactProduced{artifact}),
           step(backend::ResponseFinished{domain::FinishReason::stop}),
           testing::EndOfStream{},
       }}},
  }};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), backend,
                            &wake};
  REQUIRE(kernel.start(run_start()));
  std::optional<runtime::RunKernelError> rejection;
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    auto drained = kernel.drain();
    if (!drained) {
      rejection = drained.error();
      break;
    }
    if (drained->empty()) static_cast<void>(wake.wait_for_change(wake.count()));
  }
  REQUIRE(rejection);
  CHECK(rejection->code == runtime::RunKernelErrorCode::protocol_failure);
}

TEST_CASE("imported artifacts require exact user references and no producer",
          "[runtime][artifact][failure]") {
  const domain::ArtifactMetadata imported{
      make_id<domain::ArtifactId>("imported-audio"),
      "audio/wav",
      48,
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};

  SECTION("unreferenced import") {
    testing::ScriptedBackend backend{std::vector<testing::ScriptedExchange>{}};
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), backend};
    auto start = run_start();
    start.imported_artifacts = {imported};
    auto rejected = kernel.start(std::move(start));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == runtime::RunKernelErrorCode::invalid_start);
    CHECK(kernel.event_log().events().empty());
  }

  SECTION("unknown reference") {
    testing::ScriptedBackend backend{std::vector<testing::ScriptedExchange>{}};
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), backend};
    auto start = run_start();
    start.user_message.content = {domain::ArtifactReferenceBlock{
        imported.artifact_id, std::string{"audio to transcribe"}}};
    auto rejected = kernel.start(std::move(start));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == runtime::RunKernelErrorCode::invalid_start);
    CHECK(kernel.event_log().events().empty());
  }

  SECTION("import with producer") {
    testing::ScriptedBackend backend{std::vector<testing::ScriptedExchange>{}};
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), backend};
    auto start = run_start();
    start.user_message.content = {domain::ArtifactReferenceBlock{
        imported.artifact_id, std::string{"audio to transcribe"}}};
    auto produced = imported;
    produced.producing_inference_id =
        make_id<domain::InferenceId>("prior-inference");
    start.imported_artifacts = {std::move(produced)};
    auto rejected = kernel.start(std::move(start));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == runtime::RunKernelErrorCode::invalid_start);
    CHECK(kernel.event_log().events().empty());
  }
}

TEST_CASE("cost observations require a started response and occur once",
          "[runtime][cost][failure]") {
  SECTION("before response start") {
    auto backend_request = request();
    testing::ScriptedBackend fake{{testing::ScriptedExchange{
        backend_request, testing::StreamScript{{
                             step(backend::CostObserved{reported_cost()}),
                             testing::EndOfStream{},
                         }}}}};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                              &wake};
    REQUIRE(kernel.start(run_start(backend_request)));
    static_cast<void>(drain_to_end(kernel, wake));
    REQUIRE(kernel.projection(make_id<domain::RunId>("run"))->status() ==
            domain::RunStatus::failed);
  }

  SECTION("duplicate for one inference") {
    auto backend_request = request();
    testing::ScriptedBackend fake{{testing::ScriptedExchange{
        backend_request, testing::StreamScript{{
                             step(backend::ResponseStarted{"response"}),
                             step(backend::CostObserved{reported_cost("0.01")}),
                             step(backend::CostObserved{reported_cost("0.02")}),
                             testing::EndOfStream{},
                         }}}}};
    WakeCounter wake;
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                              &wake};
    REQUIRE(kernel.start(run_start(backend_request)));
    static_cast<void>(drain_to_end(kernel, wake));
    const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
    REQUIRE(projection->status() == domain::RunStatus::failed);
    REQUIRE(projection->reported_cost() == reported_cost("0.01"));
  }
}

TEST_CASE("usage overflow fails without committing the overflowing event",
          "[runtime][failure]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::UsageObserved{
              {std::numeric_limits<std::uint64_t>::max(), 0, 0, 0}}),
          step(backend::UsageObserved{{1, 0, 0, 0}}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::failed);
  REQUIRE(projection->usage().input_tokens ==
          std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("tool fragments assemble once and undeclared tools fail",
          "[runtime][failure]") {
  const backend::ToolDeclaration declaration{
      "lookup",
      "Lookup",
      {"application/schema+json", R"({"type":"object"})"},
      {domain::Effect::network},
      {}};
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(declaration,
                                 std::make_shared<PassiveToolExecutor>()));
  auto backend_request = request({declaration});
  const auto invocation = make_id<domain::InvocationId>("call");
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ToolCallDelta{invocation, "lookup", "{\"q\":"}),
          step(backend::ToolCallDelta{invocation, "", "\"x\"}"}),
          step(backend::ResponseFinished{domain::FinishReason::tool_call}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            fake,
                            &wake,
                            {},
                            {},
                            std::move(*snapshot)};

  REQUIRE(kernel.start(run_start(backend_request)));
  std::size_t observed_wakes{};
  for (int attempt = 0; attempt < 100 && kernel.active_inference_id();
       ++attempt) {
    REQUIRE(kernel.drain());
    if (kernel.active_inference_id()) {
      static_cast<void>(wake.wait_for_change(observed_wakes));
    }
    observed_wakes = wake.count();
  }
  REQUIRE_FALSE(kernel.active_inference_id());
  const auto& events = kernel.event_log().events();
  const auto proposed = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolProposed>(event.payload);
  });
  REQUIRE(proposed != events.end());
  REQUIRE(std::get<domain::ToolProposed>(proposed->payload).arguments.data ==
          R"({"q":"x"})");
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "test cleanup"));
}

TEST_CASE("undeclared tool calls fail without recording a proposal",
          "[runtime][failure]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ToolCallDelta{make_id<domain::InvocationId>("call"),
                                      "undeclared", "{}"}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::failed);
  REQUIRE(
      std::ranges::none_of(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::ToolProposed>(event.payload);
      }));
}

TEST_CASE("transport cancellation is one terminal outcome and keeps its reason",
          "[runtime][failure]") {
  CancelBackend fake;
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};
  const auto run = make_id<domain::RunId>("run");
  const auto inference = make_id<domain::InferenceId>("inference");

  const auto before_start = kernel.cancel(run, inference, "escape");
  REQUIRE_FALSE(before_start);
  REQUIRE(before_start.error().code ==
          runtime::RunKernelErrorCode::no_active_run);
  REQUIRE(kernel.start(run_start()));
  REQUIRE(kernel.cancel(run, inference, "escape"));
  static_cast<void>(drain_to_end(kernel, wake));

  const auto* projection = kernel.projection(run);
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::cancelled);
  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::RunCancelled>(event.payload);
          }) == 1);
  const auto cancelled = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::RunCancelled>(event.payload);
  });
  REQUIRE(std::get<domain::RunCancelled>(cancelled->payload).reason ==
          "escape");
}

TEST_CASE("cancellation between deltas preserves partial content",
          "[runtime][failure]") {
  BetweenDeltaBackend fake;
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};
  const auto run = make_id<domain::RunId>("run");
  const auto inference = make_id<domain::InferenceId>("inference");

  REQUIRE(kernel.start(run_start()));
  std::size_t observed_wakes{};
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto drained = kernel.drain();
    REQUIRE(drained);
    const auto* projection = kernel.projection(run);
    if (projection != nullptr && projection->messages().size() == 2 &&
        !projection->messages().back().content.empty()) {
      break;
    }
    static_cast<void>(wake.wait_for_change(observed_wakes));
    observed_wakes = wake.count();
  }
  REQUIRE(kernel.projection(run)->messages().back().content ==
          std::vector<domain::ContentBlock>{domain::TextBlock{"partial"}});

  REQUIRE(kernel.cancel(run, inference, "escape"));
  static_cast<void>(drain_to_end(kernel, wake));
  REQUIRE(kernel.projection(run)->status() == domain::RunStatus::cancelled);
  REQUIRE(kernel.projection(run)->messages().back().content ==
          std::vector<domain::ContentBlock>{domain::TextBlock{"partial"}});
}

TEST_CASE("cancellation after a finish is rejected", "[runtime][failure]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ResponseFinished{domain::FinishReason::stop}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{
      make_id<domain::SessionId>("session"), fake, &wake, {}, {1, 1024}};

  REQUIRE(kernel.start(run_start(backend_request)));
  const auto run = make_id<domain::RunId>("run");
  std::size_t observed_wakes{};
  for (int attempt = 0; attempt < 100 && kernel.projection(run)->status() ==
                                             domain::RunStatus::running;
       ++attempt) {
    REQUIRE(kernel.drain());
    if (kernel.projection(run)->status() == domain::RunStatus::running) {
      static_cast<void>(wake.wait_for_change(observed_wakes));
    }
    observed_wakes = wake.count();
  }
  REQUIRE(kernel.projection(run)->status() == domain::RunStatus::completed);
  const auto cancelled =
      kernel.cancel(run, make_id<domain::InferenceId>("inference"), "too late");
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code ==
          runtime::RunKernelErrorCode::already_terminal);
  static_cast<void>(drain_to_end(kernel, wake));
}

TEST_CASE("kernel teardown cancels a stalled worker without late delivery",
          "[runtime][threads]") {
  CancelBackend fake;
  WakeCounter wake;
  {
    runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                              &wake};
    REQUIRE(kernel.start(run_start()));
  }
  REQUIRE(wake.count() == 0);
}

TEST_CASE("bounded queue applies backpressure without dropping deltas",
          "[runtime][threads]") {
  auto backend_request = request();
  std::vector<testing::ScriptedStep> steps;
  steps.push_back(step(backend::ResponseStarted{"response"}));
  for (int index = 0; index < 32; ++index) {
    steps.push_back(step(backend::ContentDelta{
        backend_request.assistant_message_id, domain::TextBlock{"x"}}));
  }
  steps.push_back(step(backend::ResponseFinished{domain::FinishReason::stop}));
  steps.push_back(testing::EndOfStream{});
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request, testing::StreamScript{std::move(steps)}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{
      make_id<domain::SessionId>("session"), fake, &wake, {}, {1, 1024}};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::completed);
  REQUIRE(projection->messages().back().content.size() == 32);
}

TEST_CASE("post-terminal backend events are rejected without stranding the run",
          "[runtime][failure]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ResponseFinished{domain::FinishReason::stop}),
          step(backend::ContentDelta{backend_request.assistant_message_id,
                                     domain::TextBlock{"late"}}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  bool rejected{};
  std::size_t observed_wakes{};
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    const auto drained = kernel.drain();
    if (!drained) {
      rejected = true;
      REQUIRE(drained.error().code ==
              runtime::RunKernelErrorCode::protocol_failure);
    }
    if (kernel.active_run_id())
      static_cast<void>(wake.wait_for_change(observed_wakes));
    observed_wakes = wake.count();
  }

  REQUIRE(rejected);
  REQUIRE_FALSE(kernel.active_run_id());
  const auto* projection = kernel.projection(make_id<domain::RunId>("run"));
  REQUIRE(projection != nullptr);
  REQUIRE(projection->status() == domain::RunStatus::completed);
}

TEST_CASE("duplicate terminal backend events are rejected once",
          "[runtime][failure]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ResponseFinished{domain::FinishReason::stop}),
          step(backend::ResponseFinished{domain::FinishReason::stop}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  bool rejected{};
  std::size_t observed_wakes{};
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    const auto drained = kernel.drain();
    rejected = rejected || !drained;
    if (kernel.active_run_id())
      static_cast<void>(wake.wait_for_change(observed_wakes));
    observed_wakes = wake.count();
  }
  REQUIRE(rejected);
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(kernel.projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::completed);
}

TEST_CASE("run provenance is validated and completed before it is recorded",
          "[runtime][provenance][failure]") {
  const backend::ToolDeclaration declaration{
      "lookup",
      "Lookup",
      {"application/schema+json", R"({"type":"object"})"},
      {domain::Effect::network},
      {{domain::Effect::network, "network.host", "example.test"}}};
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      declaration, std::make_shared<PassiveToolExecutor>(), {},
      runtime::ToolExecutorContract{"test.lookup", "1"}));
  auto hidden = declaration;
  hidden.name = "hidden";
  REQUIRE(registry.register_tool(
      hidden, std::make_shared<PassiveToolExecutor>(), {},
      runtime::ToolExecutorContract{"test.hidden", "1"}));
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const std::vector<std::string> selected_names{"lookup"};
  auto selected = snapshot->subset(selected_names);
  REQUIRE(selected);
  auto backend_request = request(selected->declarations());
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ResponseFinished{domain::FinishReason::stop}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            fake,
                            &wake,
                            {},
                            {},
                            std::move(*snapshot)};

  // The tool section is kernel-owned, so a caller may not assert it.
  auto claimed_tools = run_start(backend_request);
  claimed_tools.provenance = provenance();
  claimed_tools.provenance->tools = {{"lookup", {}, {}}};
  auto started = kernel.start(std::move(claimed_tools));
  REQUIRE_FALSE(started);
  REQUIRE(started.error().code == runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());

  // A sensitive value is refused before anything is recorded, even with no
  // durable store attached.
  auto secret = run_start(backend_request);
  secret.provenance = provenance();
  secret.provenance->configuration.front().sensitive = true;
  started = kernel.start(std::move(secret));
  REQUIRE_FALSE(started);
  REQUIRE(started.error().code == runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().empty());
  REQUIRE_FALSE(kernel.active_run_id());

  // The kernel remains usable, and fills tool identity from the run's exact
  // effective subset rather than every registered tool.
  auto valid = run_start(backend_request);
  valid.provenance = provenance();
  REQUIRE(kernel.start(std::move(valid)));

  const auto& events = kernel.event_log().events();
  REQUIRE(events.size() >= 2);
  REQUIRE(std::holds_alternative<domain::RunStarted>(events[0].payload));
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&events[1].payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance.tools.size() == 1);
  REQUIRE(recorded->provenance.tools.front().tool_name == declaration.name);
  REQUIRE(recorded->provenance.tools.front().declared_effects ==
          declaration.effects);
  REQUIRE(recorded->provenance.tools.front().capability_scopes ==
          declaration.capability_scopes);
  REQUIRE(recorded->provenance.tools.front().registration_digest);
  REQUIRE(recorded->provenance.tools.front().registration_digest->starts_with(
      "sha256:"));
  REQUIRE(events[1].metadata.run_id == events[0].metadata.run_id);
  REQUIRE(events[1].metadata.sequence > events[0].metadata.sequence);

  // Copied before draining: the event log may reallocate underneath a pointer.
  const auto completed = recorded->provenance;
  static_cast<void>(drain_to_end(kernel, wake));
  REQUIRE(kernel.projection(make_id<domain::RunId>("run"))->provenance() ==
          completed);
}

TEST_CASE("a run without provenance records none", "[runtime][provenance]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          step(backend::ResponseStarted{"response"}),
          step(backend::ResponseFinished{domain::FinishReason::stop}),
          testing::EndOfStream{},
      }}}}};
  WakeCounter wake;
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::none_of(events, [](const auto& event) {
    return std::holds_alternative<domain::RunProvenanceRecorded>(event.payload);
  }));
  REQUIRE_FALSE(kernel.projection(make_id<domain::RunId>("run"))->provenance());
}

TEST_CASE("run kernel records spend ceiling changes without backend work",
          "[runtime][spend][failure]") {
  testing::ScriptedBackend fake{{}};
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake};
  const auto change = [](const std::string& run, const std::string& value) {
    return runtime::SessionSpendCeilingChange{
        make_id<domain::RunId>(run),
        {make_id<domain::SurfaceId>("session-policy"),
         make_id<domain::WorkspaceId>("chat"),
         make_id<domain::PermissionProfileId>("observe"), std::nullopt},
        domain::SessionSpendCeiling::from(value).value(),
        domain::SessionSpendCeilingSource::command_line};
  };

  REQUIRE(kernel.record_session_spend_ceiling(change("policy-10", "10")));
  REQUIRE(kernel.event_log().events().size() == 3);
  REQUIRE(std::holds_alternative<domain::RunStarted>(
      kernel.event_log().events()[0].payload));
  REQUIRE(std::get<domain::SessionSpendCeilingSet>(
              kernel.event_log().events()[1].payload)
              .ceiling.amount()
              .to_string() == "10");
  REQUIRE(std::holds_alternative<domain::RunCompleted>(
      kernel.event_log().events()[2].payload));
  REQUIRE(kernel.projection(make_id<domain::RunId>("policy-10"))->status() ==
          domain::RunStatus::completed);
  REQUIRE(fake.recorded_requests().empty());

  REQUIRE(kernel.record_session_spend_ceiling(change("policy-5", "5")));
  const auto widened =
      kernel.record_session_spend_ceiling(change("policy-6", "6"));
  REQUIRE_FALSE(widened);
  REQUIRE(widened.error().code == runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(kernel.event_log().events().size() == 6);
  REQUIRE_FALSE(kernel.projection(make_id<domain::RunId>("policy-6")));
}

TEST_CASE("resume restores recorded provenance and rejects a duplicate record",
          "[runtime][provenance][failure]") {
  const auto session = make_id<domain::SessionId>("session");
  const auto run = make_id<domain::RunId>("run");
  auto legacy_provenance = provenance();
  legacy_provenance.tools = {{"legacy_tool", {}, {}}};
  const auto history = [&](const bool duplicate) {
    std::vector<domain::RunEvent> events{
        durable_event(
            run, 1, "e1",
            domain::RunStarted{make_id<domain::SurfaceId>("test"),
                               make_id<domain::WorkspaceId>("chat"),
                               make_id<domain::PermissionProfileId>("observe"),
                               std::nullopt}),
        durable_event(run, 2, "e2",
                      domain::RunProvenanceRecorded{legacy_provenance}),
        durable_event(run, 3, "e3", domain::RunCompleted{})};
    if (duplicate) {
      events.insert(
          events.begin() + 2,
          durable_event(run, 3, "e2b",
                        domain::RunProvenanceRecorded{legacy_provenance}));
      events.back().metadata.sequence = 4;
    }
    return events;
  };

  testing::ScriptedBackend fake{{}};
  const storage::SessionInfo info{
      session, domain::EventTimestamp{std::chrono::milliseconds{100}},
      domain::EventTimestamp{std::chrono::milliseconds{103}}, 3};
  testing::ScriptedSessionStore store{
      {{testing::OpenSessionCall{session}, info},
       {testing::ReplayEventsCall{session}, history(false)}}};
  auto resumed = runtime::RunKernel::open_durable(
      {session, runtime::DurableSessionMode::resume,
       domain::EventTimestamp{std::chrono::milliseconds{100}}},
      store, fake);
  REQUIRE(resumed);
  REQUIRE((*resumed)->projection(run)->provenance() == legacy_provenance);

  const storage::SessionInfo duplicate_info{
      session, domain::EventTimestamp{std::chrono::milliseconds{100}},
      domain::EventTimestamp{std::chrono::milliseconds{104}}, 4};
  testing::ScriptedSessionStore duplicate_store{
      {{testing::OpenSessionCall{session}, duplicate_info},
       {testing::ReplayEventsCall{session}, history(true)}}};
  const auto rejected = runtime::RunKernel::open_durable(
      {session, runtime::DurableSessionMode::resume,
       domain::EventTimestamp{std::chrono::milliseconds{100}}},
      duplicate_store, fake);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);
}
