#include "../144chatsummary/fixture.hpp"
#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/repository_context_controller.hpp>
#include <aiforge/testing/scripted_session_store.hpp>
#include <map>

namespace {
using namespace chat_summary_test;
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

auto rolling(Fixture& f) -> void {
  const auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(
      policy->policy.revision, domain::ConversationMode::rolling, {}));
}
} // namespace
TEST_CASE("Chat summary review rejects stale draft model and durable state "
          "without activation",
          "[chatsummarypreview][stale]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  const auto preview = f.chat->preview_conversation_summary(
      version(candidate), {}, "Unsubmitted next message");
  INFO((preview ? "preview" : preview.error().message));
  REQUIRE(preview);
  std::string draft{"Unsubmitted next message"};
  SECTION("draft") {
    draft = "Changed next message";
  }
  SECTION("model") {
    REQUIRE(f.chat->select_model(id<domain::ModelId>("other")));
  }
  SECTION("policy") {
    rolling(f);
  }
  SECTION("candidate edit") {
    REQUIRE(f.chat->edit_conversation_summary(
        f.chat->event_log().last_sequence(), version(candidate),
        "New reviewed text"));
  }
  const auto before = f.chat->event_log().events();
  const auto applied = f.chat->apply_conversation_summary(*preview, draft);
  REQUIRE_FALSE(applied);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
  const auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  CHECK(catalog->snapshot.active.empty());
}
TEST_CASE("Chat context inspection retains required budget when mandatory "
          "input cannot fit",
          "[chatsummarypreview][capacity]") {
  Fixture f;
  f.models.window = 300;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("tiny")));
  const auto before = f.chat->event_log().events();
  const auto inspected =
      f.chat->inspect_conversation_context("Unsubmitted draft");
  INFO((inspected ? "inspected" : inspected.error().message));
  REQUIRE(inspected);
  REQUIRE(inspected->preparation_error);
  CHECK_FALSE(inspected->next_context);
  CHECK_FALSE(inspected->next_admission);
  CHECK(inspected->mandatory.capacity.context_window_tokens == 300);
  CHECK(inspected->mandatory.capacity.reserved_input_tokens > 0);
  CHECK_FALSE(inspected->mandatory.instructions.empty());
  REQUIRE(inspected->mandatory.content.size() == 1);
  CHECK(std::get<domain::TextBlock>(
            inspected->mandatory.content.front().message.content.front())
            .text == "Unsubmitted draft");
  REQUIRE(inspected->groups.size() == 1);
  CHECK(inspected->groups.front().run_id == id<domain::RunId>("source-run"));
  CHECK_FALSE(inspected->groups.front().decision);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Chat context inspection and summary preview never create missing "
          "memory journals",
          "[chatsummarypreview][readonly]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  testing::ScriptedSessionStore memory_store{
      {{testing::FindMemoryJournalCall{domain::MemoryOwner::global()},
        std::optional<storage::SessionInfo>{}},
       {testing::FindMemoryJournalCall{domain::MemoryOwner::global()},
        std::optional<storage::SessionInfo>{}}}};
  std::uint64_t identities{};
  std::uint64_t timestamps{};
  runtime::MemoryController memory{memory_store, [&] { return ++identities; },
                                   [&] {
                                     ++timestamps;
                                     return domain::EventTimestamp{};
                                   }};
  f.dependencies.memory_controller = &memory;
  f.dependencies.memory_settings.context_tokens = 1000;
  f.reopen();
  const auto before = f.chat->event_log().events();
  const auto inspected =
      f.chat->inspect_conversation_context("draft stays here");
  REQUIRE(inspected);
  INFO((inspected->preparation_error ? inspected->preparation_error->message
                                     : "prepared"));
  REQUIRE(inspected->next_context);
  const auto preview = f.chat->preview_conversation_summary(
      version(candidate), {}, "draft stays here");
  INFO((preview ? "reviewed" : preview.error().message));
  REQUIRE(preview);
  CHECK(identities == 0);
  CHECK(timestamps == 0);
  for (const auto& call : memory_store.recorded_calls())
    CHECK(std::holds_alternative<testing::FindMemoryJournalCall>(call));
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE("Chat reviewed summary applies only explicitly and inspection shows "
          "coverage",
          "[chatsummarypreview]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  std::string draft;
  SECTION("nonempty draft") {
    draft = "Continue the unfinished task";
  }
  SECTION("empty composer") {
  }
  const auto before = f.chat->event_log().events();
  const auto preview =
      f.chat->preview_conversation_summary(version(candidate), {}, draft);
  INFO((preview ? "reviewed" : preview.error().message));
  REQUIRE(preview);
  CHECK(f.chat->event_log().events() == before);
  const auto applied = f.chat->apply_conversation_summary(*preview, draft);
  INFO((applied ? "applied" : applied.error().message));
  REQUIRE(applied);
  auto inspected = f.chat->inspect_conversation_context(draft);
  INFO((inspected ? "inspected" : inspected.error().message));
  REQUIRE(inspected);
  REQUIRE(inspected->next_admission);
  REQUIRE(inspected->summaries.size() == 1);
  REQUIRE(inspected->groups.size() == 1);
  CHECK(inspected->groups.front().decision ==
        runtime::ConversationSelectionDecision::omitted_summary);
  CHECK(inspected->next_admission->groups.empty());
  CHECK(inspected->next_admission->summaries.size() == 1);
  CHECK(f.backend.requests().size() == 1);
  CHECK(f.chat->submitted_prompts().size() == 1);
  const auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(policy->policy.revision,
                                          domain::ConversationMode::full, {}));
  inspected = f.chat->inspect_conversation_context(draft);
  REQUIRE(inspected);
  REQUIRE(inspected->next_admission);
  CHECK(inspected->summaries.size() == 1);
  CHECK(inspected->next_admission->summaries.empty());
  CHECK(inspected->next_admission->groups.size() == 1);
}

TEST_CASE("Chat Dev summary preview admits optional evidence under the actual "
          "remaining budget",
          "[chatsummarypreview][repository]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  auto source = std::make_shared<ChatRepositorySource>();
  source->evidence_by_path["large.cpp"] = std::string(200000, 'x');
  source->evidence_by_path["small.cpp"] = "small optional evidence";
  runtime::RepositoryContextController controller{*source, source->root};
  f.dependencies.repository_id = source->root.repository_id;
  f.dependencies.repository_context_controller = &controller;
  f.dependencies.repository_context_selection =
      runtime::RepositoryContextRequest{"", 1, {"large.cpp", "small.cpp"}};
  f.reopen();
  const auto preview =
      f.chat->preview_conversation_summary(version(candidate), {}, "draft");
  INFO((preview ? "reviewed" : preview.error().message));
  REQUIRE(preview);
  bool small{};
  for (const auto& entry : preview->context().entries)
    for (const auto& block : entry.message.content)
      if (const auto* text = std::get_if<domain::TextBlock>(&block)) {
        CHECK(text->text != source->evidence_by_path["large.cpp"]);
        if (text->text.find("small optional evidence") != std::string::npos)
          small = true;
      }
  CHECK(small);
  SECTION("changing an observed timestamp alone preserves review") {
    const auto applied = f.chat->apply_conversation_summary(*preview, "draft");
    INFO((applied ? "applied" : applied.error().message));
    REQUIRE(applied);
  }
  SECTION("changed admitted evidence invalidates review") {
    source->evidence_by_path["small.cpp"] = "changed optional evidence";
    const auto before = f.chat->event_log().events();
    CHECK_FALSE(f.chat->apply_conversation_summary(*preview, "draft"));
    CHECK(f.chat->event_log().events() == before);
  }
  CHECK(f.backend.requests().size() == 1);
}

TEST_CASE(
    "Summary and control source purposes cannot enter automatic memory capture",
    "[chatsummarypreview][memory]") {
  summary_kernel_test::Store source;
  source.history.clear();
  auto purpose = domain::RunPurpose::summary;
  SECTION("summary") {
  }
  SECTION("control") {
    purpose = domain::RunPurpose::control;
  }
  source.seed(summary_kernel_test::attributes(purpose));
  source.seed(domain::ToolProposed{
      id<domain::InvocationId>("proposal"),
      "propose_memory",
      {"application/json",
       R"({"scope":"global","kind":"user_preference","content":"Synthetic summary instruction","rationale":"Should never be captured","evidence_excerpt":"Synthetic source"})"},
      {}});
  source.seed(domain::ToolResultRecorded{
      id<domain::InvocationId>("proposal"),
      {domain::StructuredDataBlock{
          "application/json", R"({"scope":"global","status":"proposed"})"}},
      {}});
  source.seed(domain::RunCompleted{});
  testing::ScriptedSessionStore store{{}};
  std::uint64_t identities{};
  runtime::MemoryController memory{store, [&] { return ++identities; },
                                   [] { return domain::EventTimestamp{}; }};
  const auto captured =
      memory.capture_committed(id<domain::SessionId>("session"), source.history,
                               {domain::MemoryCaptureMode::automatic,
                                domain::MemoryCaptureMode::automatic,
                                domain::MemoryCaptureMode::automatic, 2048},
                               {}, "test");
  REQUIRE(captured);
  CHECK(*captured == 0);
  CHECK(identities == 0);
  CHECK(store.recorded_calls().empty());
}

TEST_CASE("Chat context inspection distinguishes active frozen admission from "
          "next policy",
          "[chatsummarypreview][active]") {
  Fixture f;
  REQUIRE(f.chat->submit("Current run input"));
  REQUIRE(f.chat->active());
  rolling(f);
  const auto inspection =
      f.chat->inspect_conversation_context("Next unsubmitted draft");
  INFO((inspection ? "inspected" : inspection.error().message));
  REQUIRE(inspection);
  REQUIRE(inspection->active_admission);
  REQUIRE(inspection->next_admission);
  CHECK(inspection->active_admission->mode == domain::ConversationMode::full);
  CHECK(inspection->next_admission->mode == domain::ConversationMode::rolling);
  CHECK(inspection->policy.policy.mode == domain::ConversationMode::rolling);
  f.drain();
  CHECK(f.backend.requests().size() == 1);
}
