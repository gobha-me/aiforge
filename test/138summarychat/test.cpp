#include "../131summarykernel/fixture.hpp"
#include "../135summarycontext/fixture.hpp"
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/surfaces/chat_session.hpp>

namespace {
using namespace summary_context_test;
using namespace std::chrono_literals;
class Stream final : public backend::BackendStream {
 public:
  explicit Stream(domain::MessageId message) : m_message(std::move(message)) {}
  auto next(std::stop_token stop)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (stop.stop_requested())
      return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
    switch (m_step++) {
      case 0:
        return backend::BackendEvent{backend::ResponseStarted{"response"}};
      case 1:
        return backend::BackendEvent{
            backend::ContentDelta{m_message, domain::TextBlock{"answer"}}};
      case 2:
        return backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::stop}};
      default: return std::optional<backend::BackendEvent>{};
    }
  }

 private:
  domain::MessageId m_message;
  unsigned m_step{};
};

class QuestionStream final : public backend::BackendStream {
 public:
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    switch (m_step++) {
      case 0:
        return backend::BackendEvent{backend::ResponseStarted{"question"}};
      case 1:
        return backend::BackendEvent{backend::ToolCallDelta{
            id<domain::InvocationId>("ask-call"), "ask_user",
            R"({"questions":[{"id":"format","prompt":"Choose output","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"short","label":"Short","recommended":true},{"id":"long","label":"Long"}]}]})"}};
      case 2:
        return backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::tool_call}};
      default: return std::optional<backend::BackendEvent>{};
    }
  }

 private:
  unsigned m_step{};
};

class Backend final : public backend::Backend,
                      public backend::ModelContextProvider {
 public:
  auto lookup(const domain::ModelId& model, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{
        model, window, 16, {}, backend::ModelCapabilityMap{{"tools", true}}};
  }
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    std::unique_ptr<backend::BackendStream> stream;
    if (ask_next) {
      ask_next = false;
      stream = std::make_unique<QuestionStream>();
    } else {
      stream = std::make_unique<Stream>(request.assistant_message_id);
    }
    std::lock_guard lock{m_mutex};
    m_requests.push_back(std::move(request));
    return stream;
  }
  auto requests() -> std::vector<backend::BackendRequest> {
    std::lock_guard lock{m_mutex};
    return m_requests;
  }
  std::uint64_t window{8192};
  bool ask_next{};

 private:
  std::mutex m_mutex;
  std::vector<backend::BackendRequest> m_requests;
};

struct ChatFixture {
  Fixture source;
  summary_kernel_test::Store store;
  Backend backend;
  runtime::ToolRegistrySnapshot tools;
  std::shared_ptr<std::uint64_t> identity{
      std::make_shared<std::uint64_t>(1000)};
  std::unique_ptr<surfaces::ChatSession> chat;
  std::optional<Summary> summary;
  std::optional<domain::ConversationSummaryActivation> activation;

  ChatFixture() {
    runtime::ToolRegistry registry;
    REQUIRE(runtime::register_ask_user_tool(registry, true));
    auto registered = registry.snapshot();
    REQUIRE(registered);
    tools = *registered;
    source.source("old", "Original secret story fact.");
    summary = source.summary("story", {id<domain::RunId>("old")},
                             "Reviewed story fact and unfinished task.");
    activation = source.activate(*summary);
    source.policy(domain::ConversationMode::rolling);
    store.history = source.log.events();
    reopen();
  }
  auto reopen() -> void {
    surfaces::ChatSessionDependencies dependencies;
    dependencies.tools = tools;
    dependencies.identity_suffix_source = [counter = identity] {
      return ++*counter;
    };
    auto result = surfaces::ChatSession::open(
        {id<domain::ModelId>("model"), surfaces::ChatSessionOpen::Mode::resume,
         source.log.session_id(),
         domain::RunProvenance{
             "test", "test", {}, id<domain::ModelId>("model"), {}, {}, {}, {}}},
        backend, backend, &store, nullptr, {}, {1024 * 1024, 16},
        std::move(dependencies));
    INFO((result ? "session opened" : result.error().message));
    REQUIRE(result);
    chat = std::move(*result);
  }
  auto pending() -> void {
    backend.ask_next = true;
    const auto submitted = chat->submit("Continue the story after I answer.");
    INFO((submitted ? "submitted" : submitted.error().message));
    REQUIRE(submitted);
    for (unsigned attempt = 0;
         attempt < 1000 && !chat->pending_question_input(); ++attempt) {
      REQUIRE(chat->drain());
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(chat->pending_question_input());
    REQUIRE(backend.requests().size() == 1);
    chat.reset();
  }
  auto append_disabled(bool reactivate) -> void {
    for (std::size_t i = source.log.events().size(); i < store.history.size();
         ++i)
      REQUIRE(source.log.append(store.history[i]));
    source.disable(*activation);
    if (reactivate) source.activate(*summary);
    store.history = source.log.events();
  }
  auto answer() -> void {
    const auto pending = chat->pending_question_input();
    REQUIRE(pending);
    const auto answered = chat->answer_questions(
        pending->run_id, pending->invocation_id,
        {{id<domain::QuestionId>("format"), {"short"}, {}}});
    INFO((answered ? "answered" : answered.error().message));
    REQUIRE(answered);
    for (unsigned attempt = 0; attempt < 1000 && chat->active(); ++attempt) {
      const auto drained = chat->drain();
      INFO((drained ? "drained" : drained.error().message));
      REQUIRE(drained);
      if (drained->empty()) std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(chat->active());
  }
};
} // namespace

TEST_CASE("Chat rejects a disabled or reactivated saved summary before resumed "
          "dispatch",
          "[summarychat][recovery]") {
  ChatFixture f;
  f.pending();
  bool reactivate{};
  SECTION("disabled") {
  }
  SECTION("same candidate activated again") {
    reactivate = true;
  }
  f.append_disabled(reactivate);
  const auto before = f.store.history;
  f.reopen();
  CHECK(f.backend.requests().size() == 1);
  const auto drained = f.chat->drain();
  REQUIRE(drained);
  CHECK(drained->empty());
  REQUIRE(f.chat->blocked_recovery());
  CHECK(f.chat->blocked_recovery()->reason.code ==
        surfaces::ChatSessionErrorCode::context_failed);
  CHECK(f.store.history == before);
  CHECK(f.backend.requests().size() == 1);
  REQUIRE(f.chat->cancel_active("Cancel unavailable summary context"));
  CHECK_FALSE(f.chat->active());
  CHECK(f.backend.requests().size() == 1);
}

TEST_CASE("Chat restores exact summary evidence and keeps the active rolling "
          "base when next policy becomes full",
          "[summarychat][freeze]") {
  ChatFixture f;
  f.pending();
  const auto original = f.backend.requests().front().context;
  const auto before = f.store.history;
  f.reopen();
  CHECK(f.store.history == before);
  REQUIRE(f.chat->drain());
  REQUIRE_FALSE(f.chat->blocked_recovery());
  const auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(policy->policy.revision,
                                          domain::ConversationMode::full));
  f.answer();
  const auto requests = f.backend.requests();
  REQUIRE(requests.size() == 2);
  std::size_t summaries{};
  for (const auto& entry : original.entries) {
    if (entry.message.role != domain::Role::evidence) continue;
    ++summaries;
    const auto found =
        std::ranges::find(requests.back().context.entries, entry.entry_id,
                          &domain::ContextEntry::entry_id);
    REQUIRE(found != requests.back().context.entries.end());
    CHECK(*found == entry);
  }
  CHECK(summaries == 1);
  CHECK(std::ranges::none_of(
      requests.back().context.entries, [](const auto& entry) {
        return entry.message.message_id == id<domain::MessageId>("old-user");
      }));
  REQUIRE(f.chat->submit("Next full turn"));
  for (unsigned attempt = 0; attempt < 1000 && f.chat->active(); ++attempt) {
    REQUIRE(f.chat->drain());
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE_FALSE(f.chat->active());
  REQUIRE(f.backend.requests().size() == 3);
  const auto next = f.backend.requests().back().context;
  CHECK(std::ranges::any_of(next.entries, [](const auto& entry) {
    return entry.message.message_id == id<domain::MessageId>("old-user");
  }));
  CHECK(std::ranges::none_of(next.entries, [](const auto& entry) {
    return entry.message.role == domain::Role::evidence;
  }));
}
