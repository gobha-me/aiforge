#include "../144chatsummary/fixture.hpp"
#include "fixture.hpp"
#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/repository_context_controller.hpp>
#include <map>

namespace {
using namespace chat_evidence_test;
using summary_kernel_test::id;
class Stream final : public backend::BackendStream {
 public:
  explicit Stream(std::vector<backend::BackendEvent> events)
      : m_events(std::move(events)) {}
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_position == m_events.size()) return std::nullopt;
    return m_events[m_position++];
  }

 private:
  std::vector<backend::BackendEvent> m_events;
  std::size_t m_position{};
};
class Backend final : public backend::Backend {
 public:
  std::atomic<bool> ask{};
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    std::lock_guard lock{mutex};
    history.push_back(request);
    if (!ask.exchange(false))
      return std::make_unique<chat_summary_test::Stream>(
          request.assistant_message_id, domain::FinishReason::stop);
    return std::make_unique<Stream>(std::vector<backend::BackendEvent>{
        backend::ResponseStarted{"question-response"},
        backend::ToolCallDelta{
            id<domain::InvocationId>("question"), "ask_user",
            R"({"questions":[{"id":"choice","prompt":"Choose","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"yes","label":"Yes"}]}]})"},
        backend::UsageObserved{{7, 3, 0}},
        backend::ResponseFinished{domain::FinishReason::tool_call}});
  }
  auto requests() -> std::vector<backend::BackendRequest> {
    std::lock_guard lock{mutex};
    return history;
  }

 private:
  std::mutex mutex;
  std::vector<backend::BackendRequest> history;
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
        std::chrono::sys_time<std::chrono::milliseconds>{
            std::chrono::milliseconds{observations}}};
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

struct Fixture {
  std::shared_ptr<Factory> factory{std::make_shared<Factory>()};
  std::unique_ptr<Browser> browser{Browser::create(factory).value()};
  summary_kernel_test::Store store;
  Backend backend;
  chat_summary_test::Models models;
  surfaces::ChatSessionDependencies dependencies;
  std::uint64_t identity{1000};
  std::unique_ptr<surfaces::ChatSession> chat;
  Fixture() {
    REQUIRE(browser->activate_session(id<domain::SessionId>("session")));
    dependencies.local_sources = browser.get();
    dependencies.identity_suffix_source = [this] { return ++identity; };
    runtime::ToolRegistry registry;
    REQUIRE(runtime::register_ask_user_tool(registry, true));
    auto tools = registry.snapshot();
    REQUIRE(tools);
    dependencies.tools = std::move(*tools);
    reopen();
  }
  auto reopen() -> void {
    chat.reset();
    // Durable conversational tools require provenance on every start and
    // reopen; the kernel fills the exact registered tool identity itself.
    const domain::RunProvenance provenance{
        "test", "fake", {}, id<domain::ModelId>("model"), {}, {}, {}, {}};
    REQUIRE(domain::validate_run_provenance(provenance));
    auto result = surfaces::ChatSession::open(
        {id<domain::ModelId>("model"), surfaces::ChatSessionOpen::Mode::resume,
         id<domain::SessionId>("session"), provenance},
        backend, models, &store, nullptr, {}, {1024U * 1024U, 256},
        dependencies);
    INFO((result ? "opened" : result.error().message));
    REQUIRE(result);
    chat = std::move(*result);
  }
  auto finish_browser() -> void {
    REQUIRE(until([&] {
      REQUIRE(browser->poll());
      return !browser->state().granting && !browser->state().reading;
    }));
  }
  auto grant() -> void {
    REQUIRE(browser->add_folder("/private/folder"));
    finish_browser();
  }
  auto select() -> void {
    REQUIRE(browser->add_evidence(root(), "notes.txt"));
    finish_browser();
  }
  auto prepared(std::optional<std::string_view> current_draft = {})
      -> std::expected<surfaces::ChatEvidenceOutcome,
                       surfaces::ChatSessionError> {
    std::optional<surfaces::ChatEvidenceOutcome> result;
    std::optional<surfaces::ChatSessionError> failure;
    REQUIRE(until([&] {
      auto next = chat->poll_evidence_work(current_draft);
      if (!next)
        failure = next.error();
      else if (*next)
        result = std::move(**next);
      return result.has_value() || failure.has_value();
    }));
    if (failure) return std::unexpected(*failure);
    return std::move(*result);
  }
  auto submit() -> domain::RunId {
    REQUIRE(chat->request_evidence_submit("Use my selected notes"));
    REQUIRE(chat->pending_evidence_work());
    auto result = prepared();
    INFO((result ? "submitted" : result.error().message));
    REQUIRE(result);
    REQUIRE(std::holds_alternative<surfaces::ChatSubmission>(result->result));
    return std::get<surfaces::ChatSubmission>(result->result).run_id;
  }
  template <class Payload> auto count() const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        chat->event_log().events(), [](const auto& event) {
          return std::holds_alternative<Payload>(event.payload);
        }));
  }
  auto drain_once() -> void {
    auto result = chat->drain();
    INFO((result ? "drained" : result.error().message));
    REQUIRE(result);
  }
};
} // namespace

TEST_CASE("changed local file refuses asynchronous submission without provider "
          "or event mutation",
          "[chatevidence][failure]") {
  Fixture f;
  f.grant();
  f.select();
  f.factory->observation->changed = true;
  const auto before = f.chat->event_log().events();
  REQUIRE(f.chat->request_evidence_submit("Keep this draft"));
  CHECK(f.backend.requests().empty());
  auto result = f.prepared();
  REQUIRE_FALSE(result);
  CHECK_FALSE(f.chat->pending_evidence_work());
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
  CHECK(f.browser->state().selection.size() == 1);
  CHECK(f.factory->observation->owner_io == 0);
}

TEST_CASE("cancelled and changed-session local completions cannot submit a "
          "captured draft",
          "[chatevidence][stale]") {
  Fixture f;
  f.grant();
  f.select();
  auto gate = std::make_shared<Gate>();
  f.factory->observation->read_gate = gate;
  Release release{gate};
  const auto before = f.chat->event_log().events();
  REQUIRE(f.chat->request_evidence_submit("Keep this draft"));
  REQUIRE(gate->await());
  SECTION("explicit cancellation") {
    f.chat->cancel_evidence_work();
  }
  SECTION("session switch") {
    REQUIRE(f.browser->activate_session(id<domain::SessionId>("other")));
    auto result = f.chat->poll_evidence_work();
    REQUIRE_FALSE(result);
  }
  gate->release();
  CHECK_FALSE(f.chat->pending_evidence_work());
  REQUIRE(f.chat->poll_evidence_work());
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
}

TEST_CASE("empty local tray preserves absent admission without a local "
          "preparation job",
          "[chatevidence][compatibility]") {
  Fixture f;
  const auto run = f.submit();
  REQUIRE(until([&] { return !f.backend.requests().empty(); }));
  CHECK(f.factory->observation->reads == 0);
  CHECK(f.browser->occupied_workers() == 0);
  auto recorded =
      runtime::recorded_local_context_admission(f.chat->event_log(), run);
  REQUIRE(recorded);
  CHECK_FALSE(*recorded);
  REQUIRE(f.chat->cancel_active("fixture complete"));
}

TEST_CASE("local inspection and actual submission share exact admitted "
          "optional evidence",
          "[chatevidence][success]") {
  Fixture f;
  f.grant();
  f.select();
  REQUIRE(f.chat->request_context_inspection("Use my selected notes"));
  auto inspected = f.prepared();
  REQUIRE(inspected);
  const auto& inspection =
      std::get<surfaces::ChatConversationContextInspection>(inspected->result);
  REQUIRE(inspection.local_admission);
  REQUIRE(inspection.next_context);
  REQUIRE(domain::local_context_admission_matches_context(
      *inspection.local_admission, *inspection.next_context));
  CHECK(f.backend.requests().empty());
  auto run = f.submit();
  REQUIRE(until([&] { return !f.backend.requests().empty(); }));
  auto recorded =
      runtime::recorded_local_context_admission(f.chat->event_log(), run);
  REQUIRE(recorded);
  REQUIRE(*recorded);
  CHECK((*recorded)->evidence == inspection.local_admission->evidence);
  REQUIRE(domain::local_context_admission_matches_context(
      **recorded, f.backend.requests().front().context));
  CHECK(f.factory->observation->owner_io == 0);
  REQUIRE(f.chat->cancel_active("fixture complete"));
}

TEST_CASE("queued tools account observations while original local source proof "
          "is blocked",
          "[chatevidence][dispatch]") {
  Fixture f;
  f.grant();
  f.select();
  f.backend.ask = true;
  const auto run = f.submit();
  REQUIRE(until([&] { return !f.backend.requests().empty(); }));
  auto gate = std::make_shared<Gate>();
  f.factory->observation->read_gate = gate;
  Release release{gate};
  REQUIRE(until([&] {
    f.drain_once();
    return f.chat->pending_evidence_work().has_value();
  }));
  REQUIRE(gate->await());
  CHECK(f.count<domain::UsageRecorded>() == 1);
  CHECK(f.count<domain::ToolStarted>() == 0);
  CHECK_FALSE(f.chat->pending_question_input());
  for (unsigned i = 0; i < 3; ++i)
    f.drain_once();
  CHECK(f.count<domain::ToolStarted>() == 0);
  CHECK(f.backend.requests().size() == 1);
  REQUIRE(f.chat->cancel_active("cancel paused source proof"));
  gate->release();
  f.drain_once();
  CHECK(f.count<domain::RunCancelled>() == 1);
  CHECK(f.count<domain::ToolStarted>() == 0);
  CHECK(f.factory->observation->owner_io == 0);
  CHECK_FALSE(f.chat->pending_evidence_work());
  static_cast<void>(run);
}

TEST_CASE("repository completion waits for local preparation before mixed "
          "evidence submission",
          "[chatevidence][mixed]") {
  ChatRepositorySource source;
  runtime::RepositoryContextController controller{source, source.root};
  Fixture f;
  f.dependencies.repository_context_controller = &controller;
  f.dependencies.repository_context_selection =
      runtime::RepositoryContextRequest{"", 1, {"small.cpp"}};
  f.dependencies.async_repository_preparation = true;
  f.reopen();
  f.grant();
  f.select();
  auto gate = std::make_shared<Gate>();
  f.factory->observation->read_gate = gate;
  Release release{gate};
  REQUIRE(f.chat->request_evidence_submit("Read both sources"));
  const auto token = f.chat->pending_evidence_work();
  REQUIRE(token);
  const auto work = f.chat->pending_repository_work();
  REQUIRE(work);
  REQUIRE(f.factory->observation->reads == 0);
  auto repository = controller.prepare(
      std::get<runtime::RepositoryContextRequest>(work->input));
  REQUIRE(repository);
  auto completed =
      f.chat->complete_repository_work({work->token, std::move(repository)});
  REQUIRE(completed);
  CHECK(completed->pending);
  CHECK_FALSE(completed->submitted);
  CHECK(completed->events.empty());
  REQUIRE(gate->await());
  CHECK(f.chat->pending_evidence_work() == token);
  CHECK(f.backend.requests().empty());
  auto waiting = f.chat->poll_evidence_work();
  REQUIRE(waiting);
  CHECK_FALSE(*waiting);
  gate->release();
  auto submitted = f.prepared();
  INFO((submitted ? "submitted" : submitted.error().message));
  REQUIRE(submitted);
  const auto run = std::get<surfaces::ChatSubmission>(submitted->result).run_id;
  REQUIRE(until([&] { return !f.backend.requests().empty(); }));
  auto local =
      runtime::recorded_local_context_admission(f.chat->event_log(), run);
  auto repo =
      runtime::recorded_repository_context_admission(f.chat->event_log(), run);
  REQUIRE(local);
  REQUIRE(*local);
  REQUIRE(repo);
  REQUIRE(*repo);
  const auto context = f.backend.requests().front().context;
  CHECK(domain::local_context_admission_matches_context(**local, context));
  CHECK(domain::repository_context_admission_matches_context(**repo, context));
  CHECK(f.factory->observation->owner_io == 0);
  REQUIRE(f.chat->cancel_active("fixture complete"));
}

TEST_CASE("reopened local question refuses missing grants and resumes exact "
          "originals after explicit regrant",
          "[chatevidence][recovery]") {
  Fixture f;
  f.grant();
  f.select();
  f.backend.ask = true;
  const auto run = f.submit();
  REQUIRE(until([&] {
    f.drain_once();
    if (f.chat->pending_evidence_work()) {
      auto completed = f.prepared();
      INFO((completed ? "ready" : completed.error().message));
      REQUIRE(completed);
    }
    return f.chat->pending_question_input().has_value();
  }));
  const auto provenance = std::ranges::find_if(
      f.chat->event_log().events(), [&](const auto& event) {
        return event.metadata.run_id == run &&
               std::holds_alternative<domain::RunProvenanceRecorded>(
                   event.payload);
      });
  REQUIRE(provenance != f.chat->event_log().events().end());
  const auto& tools =
      std::get<domain::RunProvenanceRecorded>(provenance->payload)
          .provenance.tools;
  REQUIRE(tools.size() == 1);
  CHECK(tools.front().tool_name == "ask_user");
  CHECK(tools.front().registration_digest.has_value());
  auto original =
      runtime::recorded_local_context_admission(f.chat->event_log(), run);
  REQUIRE(original);
  REQUIRE(*original);
  const auto first = f.backend.requests().front();
  REQUIRE(f.browser->remove_folder(root()));
  f.reopen();
  const auto before = f.chat->event_log().events();
  REQUIRE(f.chat->answer_questions(
      run, id<domain::InvocationId>("question"),
      {{id<domain::QuestionId>("choice"), {"yes"}, {}}}));
  auto refused = f.prepared();
  REQUIRE_FALSE(refused);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
  f.grant();
  CHECK(f.browser->state().selection.empty());
  f.drain_once();
  CHECK_FALSE(f.chat->pending_evidence_work());
  CHECK(f.backend.requests().size() == 1);
  REQUIRE(f.chat->answer_questions(
      run, id<domain::InvocationId>("question"),
      {{id<domain::QuestionId>("choice"), {"yes"}, {}}}));
  auto answered = f.prepared();
  INFO((answered ? "answered" : answered.error().message));
  REQUIRE(answered);
  REQUIRE(until([&] {
    f.drain_once();
    if (f.chat->pending_evidence_work()) {
      auto next = f.prepared();
      INFO((next ? "ready" : next.error().message));
      REQUIRE(next);
    }
    return f.backend.requests().size() == 2;
  }));
  const auto continued = f.backend.requests().back();
  CHECK(domain::local_context_admission_matches_context(**original,
                                                        continued.context));
  for (const auto& ref : (*original)->evidence) {
    auto find = [&](const auto& context) -> domain::ContextEntry {
      auto found = std::ranges::find(context.entries, ref.entry_id,
                                     &domain::ContextEntry::entry_id);
      REQUIRE(found != context.entries.end());
      return *found;
    };
    CHECK(find(first.context) == find(continued.context));
  }
  CHECK(f.factory->observation->owner_io == 0);
  REQUIRE(f.chat->cancel_active("fixture complete"));
}

TEST_CASE(
    "asynchronous mandatory failure retains the draft and prior file selection",
    "[chatevidence][capacity]") {
  Fixture f;
  f.grant();
  f.select();
  f.models.window = 300;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("tiny")));
  const auto before = f.chat->event_log().events();
  const std::string draft = "Keep this unsubmitted draft";
  REQUIRE(f.chat->request_context_inspection(draft));
  auto inspected = f.prepared();
  REQUIRE(inspected);
  const auto& detail =
      std::get<surfaces::ChatConversationContextInspection>(inspected->result);
  REQUIRE(detail.preparation_error);
  CHECK_FALSE(detail.next_context);
  REQUIRE(detail.mandatory.content.size() == 1);
  CHECK(std::get<domain::TextBlock>(
            detail.mandatory.content.front().message.content.front())
            .text == draft);
  REQUIRE(f.chat->request_evidence_submit(draft));
  auto submitted = f.prepared();
  REQUIRE_FALSE(submitted);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
  CHECK(f.browser->state().selection.size() == 1);
}

TEST_CASE(
    "summary application revalidates the exact reviewed local optional proof",
    "[chatevidence][summary]") {
  Fixture f;
  f.grant();
  f.select();
  auto generated = f.chat->generate_conversation_summary(
      {f.chat->event_log().last_sequence(),
       {id<domain::RunId>("source-run")},
       4096});
  REQUIRE(generated);
  REQUIRE(until([&] {
    f.drain_once();
    return !f.chat->active();
  }));
  auto candidate = f.chat->publish_conversation_summary(generated->summary_id);
  REQUIRE(candidate);
  auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(policy->policy.revision,
                                          domain::ConversationMode::rolling));
  const std::string draft = "Keep this draft";
  REQUIRE(f.chat->request_summary_preview(
      chat_summary_test::version(*candidate), {}, draft));
  auto reviewed = f.prepared();
  INFO((reviewed ? "reviewed" : reviewed.error().message));
  REQUIRE(reviewed);
  const auto preview = std::get<surfaces::ChatSummaryPreview>(reviewed->result);
  REQUIRE(preview.local_admission());
  CHECK(domain::local_context_admission_matches_context(
      *preview.local_admission(), preview.context()));
  const auto before = f.chat->event_log().events();
  f.factory->observation->changed = true;
  REQUIRE(f.chat->request_summary_apply(preview, draft));
  auto refused = f.prepared(draft);
  REQUIRE_FALSE(refused);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
  f.factory->observation->changed = false;
  REQUIRE(f.chat->request_summary_apply(preview, draft));
  auto applied = f.prepared(draft);
  INFO((applied ? "applied" : applied.error().message));
  REQUIRE(applied);
  REQUIRE(std::holds_alternative<domain::ConversationSummaryActivation>(
      applied->result));
  CHECK(f.backend.requests().size() == 1);
  CHECK(f.factory->observation->owner_io == 0);
}

TEST_CASE("summary apply needs a fresh exact composer binding before the "
          "prepared action",
          "[chatevidence][draft]") {
  Fixture f;
  f.grant();
  f.select();
  auto generated = f.chat->generate_conversation_summary(
      {f.chat->event_log().last_sequence(),
       {id<domain::RunId>("source-run")},
       4096});
  REQUIRE(generated);
  REQUIRE(until([&] {
    f.drain_once();
    return !f.chat->active();
  }));
  auto candidate = f.chat->publish_conversation_summary(generated->summary_id);
  REQUIRE(candidate);
  auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(policy->policy.revision,
                                          domain::ConversationMode::rolling));
  REQUIRE(f.chat->request_summary_preview(
      chat_summary_test::version(*candidate), {}, "reviewed draft"));
  auto reviewed = f.prepared();
  REQUIRE(reviewed);
  const auto preview = std::get<surfaces::ChatSummaryPreview>(reviewed->result);
  auto gate = std::make_shared<Gate>();
  f.factory->observation->read_gate = gate;
  Release release{gate};
  const auto before = f.chat->event_log().events();
  REQUIRE(f.chat->request_summary_apply(preview, "reviewed draft"));
  REQUIRE(gate->await());
  SECTION("missing current binding") {
    REQUIRE_FALSE(f.chat->poll_evidence_work());
  }
  SECTION("composer changed during file read") {
    REQUIRE_FALSE(f.chat->poll_evidence_work("new draft"));
  }
  CHECK_FALSE(f.chat->pending_evidence_work());
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.count<domain::ConversationSummaryActivated>() == 0);
  CHECK(f.backend.requests().size() == 1);
  gate->release();
  REQUIRE(f.chat->poll_evidence_work("reviewed draft"));
  CHECK(f.chat->event_log().events() == before);
}

TEST_CASE("ordinary preview with an empty tray does not create local evidence "
          "authority",
          "[chatevidence][browsing]") {
  Fixture f;
  f.grant();
  auto gate = std::make_shared<Gate>();
  f.factory->observation->read_gate = gate;
  Release release{gate};
  REQUIRE(f.browser->preview(root(), "notes.txt"));
  REQUIRE(gate->await());
  const auto run = f.submit();
  auto admission =
      runtime::recorded_local_context_admission(f.chat->event_log(), run);
  REQUIRE(admission);
  CHECK_FALSE(*admission);
  CHECK(f.factory->observation->reads == 0);
  gate->release();
  REQUIRE(f.chat->cancel_active("fixture complete"));
}
