#pragma once
#include "../131summarykernel/fixture.hpp"
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/surfaces/chat_session.hpp>

namespace chat_summary_test {
using namespace aiforge;
using summary_kernel_test::id;
class Models final : public backend::ModelContextProvider {
 public:
  std::uint64_t window{32768};
  auto lookup(const domain::ModelId& model, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{
        model, window, 256, {}, backend::ModelCapabilityMap{{"tools", true}}};
  }
};
class Stream final : public backend::BackendStream {
 public:
  Stream(domain::MessageId message, domain::FinishReason reason)
      : m_message(std::move(message)), m_reason(reason) {}
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    switch (m_step++) {
      case 0: return backend::BackendEvent{backend::ResponseStarted{"fake"}};
      case 1:
        return backend::BackendEvent{backend::ContentDelta{
            m_message, domain::TextBlock{
                           "Keep the open task and unresolved constraints."}}};
      case 2: return backend::BackendEvent{backend::ResponseFinished{m_reason}};
      default: return std::nullopt;
    }
  }

 private:
  domain::MessageId m_message;
  domain::FinishReason m_reason;
  unsigned m_step{};
};
class Backend final : public backend::Backend {
 public:
  domain::FinishReason reason{domain::FinishReason::stop};
  explicit Backend(summary_kernel_test::Store& store) : m_store(store) {}
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    const auto durable = m_store.intent_is_durable();
    std::lock_guard lock{m_mutex};
    m_durable = durable;
    m_requests.push_back(request);
    return std::make_unique<Stream>(request.assistant_message_id, reason);
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
  summary_kernel_test::Store& m_store;
  std::mutex m_mutex;
  std::vector<backend::BackendRequest> m_requests;
  bool m_durable{};
};
struct Fixture {
  summary_kernel_test::Store store;
  Backend backend{store};
  Models models;
  std::shared_ptr<std::uint64_t> identity{
      std::make_shared<std::uint64_t>(1000)};
  std::unique_ptr<surfaces::ChatSession> chat;
  std::stop_source stop;
  std::optional<domain::SessionSpendCeiling> ceiling{};
  surfaces::ChatSessionDependencies dependencies;
  Fixture() {
    runtime::ToolRegistry registry;
    REQUIRE(runtime::register_ask_user_tool(registry, true));
    auto tools = registry.snapshot();
    REQUIRE(tools);
    dependencies.tools = *tools;
    dependencies.identity_suffix_source = [counter = identity] {
      return ++*counter;
    };
    reopen();
  }
  auto reopen() -> void {
    chat.reset();
    surfaces::ChatSessionOpen request{id<domain::ModelId>("model"),
                                      surfaces::ChatSessionOpen::Mode::resume,
                                      id<domain::SessionId>("session")};
    request.session_spend_ceiling = ceiling;
    request.generation_options.temperature = 0.25;
    request.generation_options.seed = 42;
    auto opened = surfaces::ChatSession::open(
        request, backend, models, &store, nullptr, stop.get_token(),
        {1024U * 1024U, 256}, dependencies);
    INFO((opened ? "opened" : opened.error().message));
    REQUIRE(opened);
    chat = std::move(*opened);
  }
  auto request() const -> surfaces::ChatSummaryGenerate {
    return {chat->event_log().last_sequence(),
            {id<domain::RunId>("source-run")},
            4096};
  }
  auto drain() -> void {
    for (unsigned count{}; count < 1000 && chat->active(); ++count) {
      auto drained = chat->drain();
      INFO((drained ? "drained" : drained.error().message));
      REQUIRE(drained);
      if (chat->active())
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE_FALSE(chat->active());
  }
  auto generate() -> surfaces::ChatSummaryGeneration {
    auto generated = chat->generate_conversation_summary(request());
    INFO((generated ? "generated" : generated.error().message));
    REQUIRE(generated);
    drain();
    return *generated;
  }
  auto candidate() -> domain::ConversationSummaryCandidate {
    const auto generated = generate();
    auto published = chat->publish_conversation_summary(generated.summary_id);
    INFO((published ? "published" : published.error().message));
    REQUIRE(published);
    return *published;
  }
};
inline auto version(const domain::ConversationSummaryCandidate& candidate)
    -> domain::ConversationSummaryVersion {
  return {candidate.summary_id, candidate.revision,
          *candidate.candidate_digest};
}
} // namespace chat_summary_test
