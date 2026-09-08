#pragma once
#include <aiforge/runtime/conversation_summary_generation.hpp>
#include <aiforge/runtime/conversation_summary_projection.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace summary_kernel_test {
using namespace aiforge;
template <typename Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}
inline auto attributes(domain::RunPurpose purpose = domain::RunPurpose::summary)
    -> domain::RunStarted {
  return {id<domain::SurfaceId>("chat"),
          id<domain::WorkspaceId>("chat"),
          id<domain::PermissionProfileId>("observe"),
          {},
          {},
          purpose};
}
class Store final : public storage::SessionStore {
 public:
  std::vector<domain::RunEvent> history;
  std::vector<std::vector<domain::RunEvent>> batches;
  bool fail_append{};
  std::size_t append_attempts{};
  std::mutex mutex;
  Store() {
    seed(domain::RunStarted{attributes(domain::RunPurpose::conversation)});
    seed(domain::UserContentAdded{
        {id<domain::MessageId>("source-user"),
         domain::Role::user,
         {domain::TextBlock{"Preserve the open task and its constraints."}},
         {}}});
    seed(domain::RunCompleted{});
  }
  auto seed(domain::RunEventPayload payload) -> void {
    const auto seq = history.size() + 1;
    history.push_back(
        {{id<domain::EventId>("source-event-" + std::to_string(seq)),
          id<domain::RunId>("source-run"),
          seq,
          1,
          domain::EventTimestamp{std::chrono::milliseconds{1}},
          {},
          {},
          {}},
         std::move(payload)});
  }
  auto create_session(storage::SessionCreate, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    return {};
  }
  auto open_session(const domain::SessionId& session, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    std::lock_guard lock{mutex};
    return storage::SessionInfo{
        session,
        {},
        {},
        history.empty() ? 0 : history.back().metadata.sequence,
        1};
  }
  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }
  auto append_events(const domain::SessionId&,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    std::lock_guard lock{mutex};
    ++append_attempts;
    if (fail_append)
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::io_failure, "append refused", false});
    batches.emplace_back(events.begin(), events.end());
    history.insert(history.end(), events.begin(), events.end());
    return {};
  }
  auto replay_events(const domain::SessionId&, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    std::lock_guard lock{mutex};
    return history;
  }
  auto intent_is_durable() -> bool {
    std::lock_guard lock{mutex};
    return std::ranges::any_of(history, [](const auto& event) {
      return std::holds_alternative<
          domain::ConversationSummaryGenerationIntentRecorded>(event.payload);
    });
  }
};
class Stream final : public backend::BackendStream {
 public:
  Stream(domain::MessageId message, bool tools)
      : m_message(std::move(message)), m_tools(tools) {}
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_step++ == 0)
      return backend::BackendEvent{backend::ResponseStarted{"fake"}};
    if (m_step == 2) {
      if (m_tools)
        return backend::BackendEvent{
            backend::ToolCallDelta{id<domain::InvocationId>("unexpected-call"),
                                   "unavailable-tool", "{}"}};
      return backend::BackendEvent{backend::ContentDelta{
          m_message,
          domain::TextBlock{
              "Preserve the open task; unresolved constraints need review."}}};
    }
    if (m_step == 3)
      return backend::BackendEvent{
          backend::ResponseFinished{m_tools ? domain::FinishReason::tool_call
                                            : domain::FinishReason::stop}};
    return std::nullopt;
  }

 private:
  domain::MessageId m_message;
  bool m_tools{};
  unsigned m_step{};
};
class Backend final : public backend::Backend {
 public:
  Store& store;
  bool tools{};
  explicit Backend(Store& value) : store(value) {}
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    const auto durable = store.intent_is_durable();
    std::lock_guard lock{m_mutex};
    m_durable = durable;
    m_requests.push_back(request);
    return std::make_unique<Stream>(request.assistant_message_id, tools);
  }
  auto requests() -> std::vector<backend::BackendRequest> {
    std::lock_guard lock{m_mutex};
    return m_requests;
  }
  auto intent_was_durable() -> bool {
    std::lock_guard lock{m_mutex};
    return m_durable;
  }

 private:
  std::mutex m_mutex;
  std::vector<backend::BackendRequest> m_requests;
  bool m_durable{};
};
struct Fixture {
  Store store;
  Backend backend{store};
  std::unique_ptr<runtime::RunKernel> kernel;
  Fixture() { reopen(); }
  auto reopen() -> void {
    kernel.reset();
    auto result =
        runtime::RunKernel::open_durable({id<domain::SessionId>("session"),
                                          runtime::DurableSessionMode::resume,
                                          {}},
                                         store, backend);
    REQUIRE(result);
    kernel = std::move(*result);
  }
  auto request() -> runtime::RunStart {
    auto sources = runtime::prepare_conversation_summary_sources(
        {kernel->event_log(), {id<domain::RunId>("source-run")}});
    REQUIRE(sources);
    domain::ConversationSummaryIntent spec{
        1,
        1,
        id<domain::ConversationSummaryId>("summary"),
        std::move(sources->sources),
        id<domain::RunId>("summary-run"),
        id<domain::InferenceId>("summary-inference"),
        id<domain::ModelId>("model"),
        id<domain::MessageId>("summary-output"),
        "test",
        {100000, 1000, 0},
        0,
        4096,
        {}};
    auto prepared = runtime::prepare_conversation_summary_generation(
        kernel->event_log(), spec);
    REQUIRE(prepared);
    runtime::RunStart start{spec.producing_run_id,
                            attributes(),
                            prepared->user_message,
                            {spec.producing_inference_id,
                             spec.output_message_id,
                             spec.model_id,
                             std::move(prepared->context),
                             {},
                             {}}};
    start.request.options.max_output_tokens =
        spec.capacity.reserved_output_tokens;
    start.summary_intent = std::move(prepared->intent);
    return start;
  }
  auto drain() -> void {
    for (unsigned count = 0; count < 1000 && kernel->active_inference_id();
         ++count) {
      REQUIRE(kernel->drain());
      if (kernel->active_inference_id())
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE_FALSE(kernel->active_inference_id());
  }
  auto complete() -> void {
    REQUIRE(kernel->start(request()));
    drain();
    REQUIRE_FALSE(kernel->active_run_id());
  }
  auto reject(runtime::RunStart request) -> void {
    const auto before = kernel->event_log().events();
    const auto result = kernel->start(std::move(request));
    REQUIRE_FALSE(result);
    CHECK(kernel->event_log().events() == before);
    CHECK(store.history == before);
    CHECK(backend.requests().empty());
  }
};
} // namespace summary_kernel_test
