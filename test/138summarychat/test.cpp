#include "../131summarykernel/fixture.hpp"
#include "../135summarycontext/fixture.hpp"
#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/runtime/repository_context_controller.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <map>

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

class ChatRepositorySource final : public runtime::RepositoryContextSource {
 public:
  domain::RepositoryRootIdentity root{
      id<domain::RepositoryId>("chat-repository"), "/repo"};
  std::string instruction{"Keep changes bounded."};
  std::string evidence{"int value = 1;\n"};
  std::map<std::string, std::string> evidence_by_path;
  std::string revision{"revision-one"};
  bool unavailable{};
  std::size_t observations{};
  static auto hash(std::string_view text) -> domain::ContentDigest {
    detail::Sha256 hash;
    hash.update(std::as_bytes(std::span{text.data(), text.size()}));
    return {"sha256", hash.finish(), text.size()};
  }
  auto identity() const noexcept -> std::string_view override {
    return "chat-root-lease";
  }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto observe(repository::RepositorySnapshotLimits, std::stop_token)
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> override {
    ++observations;
    if (unavailable)
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::not_found, "unavailable"});
    return domain::RepositorySnapshot{
        root,
        domain::VcsState{"git", "sha1", domain::VcsHeadKind::branch, "main",
                         std::string(40, 'a')},
        {},
        hash(revision),
        std::chrono::sys_time<std::chrono::milliseconds>{1ms}};
  }
  auto discover(repository::ProjectInstructionRequest request, std::stop_token)
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override {
    const auto snapshot = domain::snapshot_identity(request.baseline);
    return domain::ProjectInstructionDiscovery{
        snapshot,
        request.target_subtree,
        {{id<domain::ProjectInstructionId>("chat-project-instruction"),
          {snapshot, "AGENTS.md", hash(instruction), std::nullopt},
          "",
          instruction,
          0,
          1}}};
  }
  auto read(repository::ExactSourceReadRequest request, std::stop_token)
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override {
    const auto found = evidence_by_path.find(request.relative_path);
    const auto& content =
        found == evidence_by_path.end() ? evidence : found->second;
    return repository::ExactSourceReadResult{
        {domain::snapshot_identity(request.baseline), request.relative_path,
         hash(content), std::nullopt},
        content};
  }
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

  ChatRepositorySource repository_source;
  runtime::RepositoryContextController repository_controller{
      repository_source, repository_source.root};
  bool repository_enabled{};

  explicit ChatFixture(bool dev = false) : repository_enabled(dev) {
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
    if (repository_enabled) {
      dependencies.repository_id = repository_source.root.repository_id;
      dependencies.repository_context_controller = &repository_controller;
      dependencies.repository_context_selection =
          runtime::RepositoryContextRequest{"", 1, {"src/value.cpp"}};
    }
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
    finish();
  }
  auto finish() -> void {
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

TEST_CASE("direct recovered Dev question decisions freeze summaries before "
          "a later disable",
          "[summarychat][repository][freeze]") {
  ChatFixture f{true};
  f.pending();
  const auto original = f.backend.requests().front().context;
  f.reopen();
  const auto question = f.chat->pending_question_input();
  REQUIRE(question);
  // Deliberately make the decision before the first resumed drain.
  SECTION("answer") {
    REQUIRE(f.chat->answer_questions(
        question->run_id, question->invocation_id,
        {{id<domain::QuestionId>("format"), {"short"}, {}}}));
  }
  SECTION("cancel question") {
    REQUIRE(f.chat->cancel_questions(question->run_id, question->invocation_id,
                                     "Use defaults"));
  }
  const auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  const auto disabled = f.chat->disable_conversation_summary(
      policy->policy.revision, f.activation->candidate,
      f.activation->activation_event_id);
  INFO((disabled ? "disabled" : disabled.error().message));
  REQUIRE(disabled);
  REQUIRE(disabled->size() == 3);
  REQUIRE(runtime::recorded_conversation_summaries(f.chat->event_log()));
  CHECK(runtime::recorded_conversation_summaries(f.chat->event_log())
            ->active.empty());
  f.finish();
  REQUIRE_FALSE(f.chat->blocked_recovery());
  const auto requests = f.backend.requests();
  REQUIRE(requests.size() == 2);
  const auto evidence =
      std::ranges::find_if(original.entries, [](const auto& entry) {
        return std::string{entry.entry_id.value()}.starts_with("summary-");
      });
  REQUIRE(evidence != original.entries.end());
  const auto resumed =
      std::ranges::find(requests.back().context.entries, evidence->entry_id,
                        &domain::ContextEntry::entry_id);
  REQUIRE(resumed != requests.back().context.entries.end());
  CHECK(*resumed == *evidence);
}

TEST_CASE("Chat summary disable refuses stale reviews and failed persistence",
          "[summarychat][disable][failure]") {
  ChatFixture f;
  const auto before = f.store.history;
  const auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  auto revision = policy->policy.revision;
  SECTION("stale revision") {
    ++revision;
  }
  SECTION("storage refuses commit") {
    f.store.fail_append = true;
  }
  const auto disabled = f.chat->disable_conversation_summary(
      revision, f.activation->candidate, f.activation->activation_event_id);
  REQUIRE_FALSE(disabled);
  CHECK_FALSE(disabled.error().effect_may_have_applied);
  CHECK(f.store.history == before);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
}
