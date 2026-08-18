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
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
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
                                 {0.25, 128, 42, {}}};
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

}  // namespace

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
  REQUIRE(kernel.event_log().events().size() == 12);
  REQUIRE(kernel.event_log().events().front().metadata.timestamp ==
          domain::EventTimestamp{1ms});
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
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake, &wake};

  REQUIRE(kernel.start(run_start(backend_request)));
  static_cast<void>(drain_to_end(kernel, wake));
  const auto& events = kernel.event_log().events();
  const auto proposed = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ToolProposed>(event.payload);
  });
  REQUIRE(proposed != events.end());
  REQUIRE(std::get<domain::ToolProposed>(proposed->payload).arguments.data ==
          R"({"q":"x"})");
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
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto drained = kernel.drain();
    REQUIRE(drained);
    const auto* projection = kernel.projection(run);
    if (projection != nullptr && projection->messages().size() == 2 &&
        !projection->messages().back().content.empty()) {
      break;
    }
    std::this_thread::yield();
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
  for (int attempt = 0; attempt < 100 && kernel.projection(run)->status() ==
                                             domain::RunStatus::running;
       ++attempt) {
    REQUIRE(kernel.drain());
    std::this_thread::yield();
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
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    const auto drained = kernel.drain();
    rejected = rejected || !drained;
    std::this_thread::yield();
  }
  REQUIRE(rejected);
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(kernel.projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::completed);
}
