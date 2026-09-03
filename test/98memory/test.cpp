#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/memory_controller.hpp>
#include <aiforge/runtime/memory_tool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

namespace {

using namespace aiforge;

template <typename Id> auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-memory-XXXXXX")
            .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

auto event(const std::uint64_t sequence, std::string event_id,
           domain::RunEventPayload payload,
           const domain::RunId& run_id = id<domain::RunId>("run"))
    -> domain::RunEvent {
  return {{id<domain::EventId>(std::move(event_id)), run_id, sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{1000 + sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

auto proposal_arguments(
    const std::string_view scope = "project",
    const std::string_view kind = "project_convention",
    const std::string_view content = "Use snake_case for functions",
    const std::string_view excerpt = "Use snake_case") -> std::string {
  return "{\"scope\":\"" + std::string{scope} + "\",\"kind\":\"" +
         std::string{kind} + "\",\"content\":\"" + std::string{content} +
         "\",\"rationale\":\"Preserve the recorded convention\","
         "\"evidence_excerpt\":\"" +
         std::string{excerpt} + "\"}";
}

auto source_events(const domain::InvocationId& invocation,
                   std::string arguments = proposal_arguments(),
                   std::string user_text = "Use snake_case in this project")
    -> std::vector<domain::RunEvent> {
  const auto run = id<domain::RunId>("source-run");
  return {
      event(1, "source-start",
            domain::RunStarted{id<domain::SurfaceId>("surface"),
                               id<domain::WorkspaceId>("workspace"),
                               id<domain::PermissionProfileId>("observe"),
                               std::nullopt},
            run),
      event(2, "source-user",
            domain::UserContentAdded{{id<domain::MessageId>("user-message"),
                                      domain::Role::user,
                                      {domain::TextBlock{std::move(user_text)}},
                                      std::nullopt}},
            run),
      event(3, "source-inference",
            domain::InferenceStarted{id<domain::InferenceId>("inference"),
                                     id<domain::ModelId>("model")},
            run),
      event(4, "source-proposal",
            domain::ToolProposed{invocation,
                                 "propose_memory",
                                 {"application/json", std::move(arguments)},
                                 {},
                                 std::nullopt,
                                 true,
                                 {},
                                 {},
                                 std::nullopt},
            run),
      event(5, "source-result",
            domain::ToolResultRecorded{
                invocation,
                {domain::StructuredDataBlock{
                    "application/json",
                    R"({"scope":"project","status":"proposed"})"}},
                std::nullopt},
            run)};
}

auto execute_sql(const std::filesystem::path& path, const std::string& sql)
    -> void {
  sqlite3* database{};
  REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  char* message{};
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &message);
  if (message != nullptr) sqlite3_free(message);
  REQUIRE(result == SQLITE_OK);
  REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

struct Fixture {
  TemporaryDirectory temporary;
  std::unique_ptr<adapters::SqliteSessionStore> store;
  domain::SessionId source_session{id<domain::SessionId>("source-session")};
  domain::RepositoryId repository{id<domain::RepositoryId>("repository")};
  std::uint64_t suffix{};
  std::int64_t time{2000};
  std::unique_ptr<runtime::MemoryController> controller;

  Fixture() {
    auto opened = adapters::SqliteSessionStore::open(
        temporary.path() / "aiforge" / "sessions.sqlite3");
    REQUIRE(opened);
    store = std::move(*opened);
    REQUIRE(store->create_session(
        {source_session,
         domain::EventTimestamp{std::chrono::milliseconds{1000}}}));
    controller = std::make_unique<runtime::MemoryController>(
        *store, [this] { return ++suffix; },
        [this] {
          return domain::EventTimestamp{std::chrono::milliseconds{++time}};
        });
  }
};

} // namespace

TEST_CASE("memory proposal parsing fails closed", "[memory][tool][failure]") {
  const runtime::MemoryToolConfiguration configuration{true, true, {}};
  auto parsed = runtime::parse_memory_proposal_draft(
      {"application/json", proposal_arguments()}, configuration);
  REQUIRE(parsed);
  REQUIRE(parsed->scope == domain::MemoryScope::project);
  REQUIRE(parsed->kind == domain::MemoryKind::project_convention);

  REQUIRE_FALSE(runtime::parse_memory_proposal_draft(
      {"application/json",
       R"({"scope":"project","scope":"global","kind":"workflow","content":"x","rationale":"y","evidence_excerpt":"z"})"},
      configuration));
  REQUIRE_FALSE(runtime::parse_memory_proposal_draft(
      {"application/json",
       proposal_arguments("global", "user_preference",
                          "Authorization: Bearer secret-token-value")},
      configuration));
  REQUIRE_FALSE(runtime::parse_memory_proposal_draft(
      {"application/json", proposal_arguments("global", "project_convention")},
      configuration));
}

TEST_CASE("memory tool registration carries a durable executor contract",
          "[memory][tool][registry]") {
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_memory_tool(
      registry, runtime::MemoryToolConfiguration{true, false, {}}));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto* registration = snapshot->find("propose_memory");
  REQUIRE(registration != nullptr);
  const runtime::ToolExecutorContract expected{"aiforge.runtime.propose_memory",
                                               "1"};
  REQUIRE(registration->executor_contract == expected);
}

TEST_CASE("memory settings have conservative bounded defaults",
          "[memory][config][failure]") {
  const std::vector<config::ConfigLayer> layers;
  auto resolved =
      config::resolve_config(config::builtin_config_registry(), layers);
  REQUIRE(resolved);
  auto settings = runtime::resolve_memory_settings(*resolved);
  REQUIRE(settings);
  REQUIRE(settings->global_capture == domain::MemoryCaptureMode::off);
  REQUIRE(settings->project_capture == domain::MemoryCaptureMode::review);
  REQUIRE(settings->context_tokens == 2048);

  const auto mutable_entry = [&](const std::string_view key) {
    for (auto& entry : resolved->entries) {
      if (entry.key == key) return &entry;
    }
    return static_cast<config::ResolvedConfigEntry*>(nullptr);
  };
  auto* global = mutable_entry("memory.global.capture");
  REQUIRE(global != nullptr);
  global->value = std::string{"unbounded"};
  REQUIRE_FALSE(runtime::resolve_memory_settings(*resolved));
  global->value = std::string{"off"};
  auto* tokens = mutable_entry("memory.context.max_tokens");
  REQUIRE(tokens != nullptr);
  tokens->value = std::uint64_t{};
  REQUIRE_FALSE(runtime::resolve_memory_settings(*resolved));
}

TEST_CASE("review memory is journaled accepted selected and expired",
          "[memory][sqlite][projection][context]") {
  Fixture fixture;
  const auto invocation = id<domain::InvocationId>("memory-invocation");
  const auto events = source_events(invocation);
  REQUIRE(fixture.store->append_events(fixture.source_session, events));
  const runtime::MemorySettings settings{
      domain::MemoryCaptureMode::off, domain::MemoryCaptureMode::review, 2048};

  auto captured = fixture.controller->capture_committed(
      fixture.source_session, events, settings, fixture.repository, "0.46.0");
  REQUIRE(captured == 1);
  REQUIRE(fixture.controller->capture_committed(fixture.source_session, events,
                                                settings, fixture.repository,
                                                "0.46.0") == 0);

  const auto journal_id = id<domain::SessionId>("memory-project-1");
  const auto journal_events = fixture.store->replay_events(journal_id);
  REQUIRE(journal_events);
  REQUIRE(journal_events->size() == 2);
  REQUIRE(std::holds_alternative<domain::MemoryProposed>(
      journal_events->front().payload));
  REQUIRE(std::holds_alternative<domain::MemoryPolicyDecided>(
      journal_events->back().payload));
  REQUIRE_FALSE(fixture.store->open_session(journal_id));

  const auto listed = fixture.store->list_sessions(10);
  REQUIRE(listed);
  REQUIRE(listed->size() == 1);
  REQUIRE(listed->front().session_id == fixture.source_session);

  const runtime::MemoryMutationTarget target{domain::MemoryScope::project,
                                             fixture.repository};
  auto state = fixture.controller->inspect(target);
  REQUIRE(state);
  REQUIRE(state->proposals.size() == 1);
  REQUIRE(state->proposals.front().source_available);
  REQUIRE(state->proposals.front().projected.state ==
          domain::ProjectedMemoryProposalState::pending);
  REQUIRE(state->records.empty());

  const auto proposal = state->proposals.front().projected;
  REQUIRE(fixture.controller->accept({target, proposal.proposal.proposal_id,
                                      proposal.proposal_event_id, std::nullopt,
                                      std::nullopt, std::nullopt}));
  state = fixture.controller->inspect(target);
  REQUIRE(state);
  REQUIRE(state->records.size() == 1);
  REQUIRE(state->records.front().projected.state ==
          domain::ProjectedMemoryRecordState::current);

  auto selected = runtime::select_memory_context(
      *fixture.controller, {fixture.repository, 2048, 4096});
  const auto selected_error =
      selected ? std::string{} : selected.error().message;
  INFO(selected_error);
  REQUIRE(selected);
  REQUIRE(selected->size() == 1);
  REQUIRE(selected->front().message.role == domain::Role::evidence);
  REQUIRE(std::get<domain::TextBlock>(selected->front().message.content.front())
              .text.contains("Use snake_case"));

  const auto replaced = state->records.front().projected;
  const auto replacement_session =
      id<domain::SessionId>("replacement-source-session");
  REQUIRE(fixture.store->create_session(
      {replacement_session,
       domain::EventTimestamp{std::chrono::milliseconds{2500}}}));
  auto replacement_json =
      proposal_arguments("project", "project_convention",
                         "Use camelCase for functions", "Use camelCase");
  replacement_json.pop_back();
  replacement_json += ",\"replacement_record_id\":\"" +
                      std::string{replaced.record.record_id.value()} +
                      "\",\"overlap_record_ids\":[\"" +
                      std::string{replaced.record.record_id.value()} + "\"]}";
  const auto replacement_events =
      source_events(id<domain::InvocationId>("replacement-invocation"),
                    replacement_json, "Use camelCase in this project now");
  REQUIRE(
      fixture.store->append_events(replacement_session, replacement_events));
  REQUIRE(fixture.controller->capture_committed(
              replacement_session, replacement_events, settings,
              fixture.repository, "0.46.0") == 1);
  state = fixture.controller->inspect(target);
  REQUIRE(state);
  REQUIRE(state->proposals.size() == 2);
  const auto replacement = state->proposals.back().projected;
  REQUIRE(fixture.controller->accept(
      {target, replacement.proposal.proposal_id, replacement.proposal_event_id,
       std::string{"Use PascalCase for types"}, replaced.record.record_id,
       replaced.record_event_id}));
  state = fixture.controller->inspect(target);
  REQUIRE(state);
  REQUIRE(state->records.size() == 2);
  REQUIRE(state->records.front().projected.state ==
          domain::ProjectedMemoryRecordState::superseded);
  REQUIRE(state->records.back().projected.record.content ==
          "Use PascalCase for types");
  REQUIRE_FALSE(
      fixture.controller->expire({target, replaced.record.record_id,
                                  replaced.record_event_id, "stale expiry"}));

  const auto record = state->records.back().projected;
  REQUIRE(fixture.controller->expire({target, record.record.record_id,
                                      record.record_event_id,
                                      "no longer applies"}));
  selected = runtime::select_memory_context(*fixture.controller,
                                            {fixture.repository, 2048, 4096});
  REQUIRE(selected);
  REQUIRE(selected->empty());

  runtime::MemoryController replayed{
      *fixture.store, [&fixture] { return ++fixture.suffix; },
      [&fixture] {
        return domain::EventTimestamp{
            std::chrono::milliseconds{++fixture.time}};
      }};
  state = replayed.inspect(target);
  REQUIRE(state);
  REQUIRE(state->records.back().projected.state ==
          domain::ProjectedMemoryRecordState::expired);
}

TEST_CASE("auto policy accepts direct preferences and rejects reusable facts",
          "[memory][policy][failure]") {
  Fixture fixture;
  auto preference_events = source_events(
      id<domain::InvocationId>("preference-invocation"),
      proposal_arguments("global", "user_preference", "Prefer concise output",
                         "Prefer concise"),
      "Prefer concise output");
  REQUIRE(
      fixture.store->append_events(fixture.source_session, preference_events));
  const runtime::MemorySettings automatic{domain::MemoryCaptureMode::automatic,
                                          domain::MemoryCaptureMode::automatic,
                                          2048};
  REQUIRE(fixture.controller->capture_committed(
              fixture.source_session, preference_events, automatic,
              fixture.repository, "0.46.0") == 1);
  auto global =
      fixture.controller->inspect({domain::MemoryScope::global, std::nullopt});
  REQUIRE(global);
  REQUIRE(global->records.size() == 1);
  REQUIRE(global->records.front().projected.state ==
          domain::ProjectedMemoryRecordState::current);

  const auto second_session = id<domain::SessionId>("fact-session");
  REQUIRE(fixture.store->create_session(
      {second_session,
       domain::EventTimestamp{std::chrono::milliseconds{3000}}}));
  auto fact_events =
      source_events(id<domain::InvocationId>("fact-invocation"),
                    proposal_arguments("global", "reusable_fact",
                                       "The sky is blue", "sky is blue"),
                    "The sky is blue");
  REQUIRE(fixture.store->append_events(second_session, fact_events));
  REQUIRE(fixture.controller->capture_committed(second_session, fact_events,
                                                automatic, fixture.repository,
                                                "0.46.0") == 1);
  global =
      fixture.controller->inspect({domain::MemoryScope::global, std::nullopt});
  REQUIRE(global);
  REQUIRE(global->records.size() == 1);
  REQUIRE(global->proposals.size() == 2);
  REQUIRE(global->proposals.back().projected.state ==
          domain::ProjectedMemoryProposalState::rejected);
}

TEST_CASE("unavailable source history stays visible but leaves context",
          "[memory][provenance][context][failure]") {
  Fixture fixture;
  const auto events =
      source_events(id<domain::InvocationId>("unavailable-invocation"));
  REQUIRE(fixture.store->append_events(fixture.source_session, events));
  const runtime::MemorySettings settings{
      domain::MemoryCaptureMode::off, domain::MemoryCaptureMode::review, 2048};
  REQUIRE(fixture.controller->capture_committed(fixture.source_session, events,
                                                settings, fixture.repository,
                                                "0.46.0") == 1);
  const runtime::MemoryMutationTarget target{domain::MemoryScope::project,
                                             fixture.repository};
  auto state = fixture.controller->inspect(target);
  REQUIRE(state);
  const auto proposal = state->proposals.front().projected;
  REQUIRE(fixture.controller->accept({target, proposal.proposal.proposal_id,
                                      proposal.proposal_event_id, std::nullopt,
                                      std::nullopt, std::nullopt}));

  execute_sql(fixture.temporary.path() / "aiforge" / "sessions.sqlite3",
              "PRAGMA foreign_keys=OFF;"
              "DELETE FROM events WHERE session_id='source-session';"
              "DELETE FROM sessions WHERE session_id='source-session';");
  state = fixture.controller->inspect(target);
  REQUIRE(state);
  REQUIRE(state->records.size() == 1);
  REQUIRE_FALSE(state->records.front().source_available);
  auto selected = runtime::select_memory_context(
      *fixture.controller, {fixture.repository, 2048, 4096});
  REQUIRE(selected);
  REQUIRE(selected->empty());
}

TEST_CASE("unknown future memory kinds replay but do not enter context",
          "[memory][projection][context][compatibility]") {
  Fixture fixture;
  const auto invocation = id<domain::InvocationId>("future-invocation");
  const auto events = source_events(invocation);
  REQUIRE(fixture.store->append_events(fixture.source_session, events));

  const auto proposal_id = id<domain::MemoryProposalId>("future-proposal");
  const auto record_id = id<domain::MemoryRecordId>("future-record");
  const auto proposal_event_id = id<domain::EventId>("future-proposed");
  const domain::MemoryProposal proposal{
      proposal_id,
      record_id,
      domain::MemoryScope::global,
      std::nullopt,
      domain::MemoryKind::unknown,
      "Future memory content",
      "Preserve a future schema value",
      "Use snake_case",
      {fixture.source_session,
       events.front().metadata.run_id,
       invocation,
       {events.front().metadata.event_id}},
      {id<domain::ModelId>("future-model"), "future-runtime", "2.0"},
      std::nullopt,
      {}};
  auto journal = fixture.store->open_or_create_memory_journal(
      {id<domain::SessionId>("future-memory-journal"),
       domain::MemoryScope::global, std::nullopt,
       domain::EventTimestamp{std::chrono::milliseconds{1500}}});
  REQUIRE(journal);
  const std::vector<domain::RunEvent> future_events{
      event(1, std::string{proposal_event_id.value()},
            domain::MemoryProposed{proposal}),
      event(2, "future-policy",
            domain::MemoryPolicyDecided{
                {proposal_id, domain::MemoryPolicyAction::stage,
                 domain::MemoryDecisionSource::policy,
                 "future kind requires review", proposal_event_id}}),
      event(3, "future-accepted",
            domain::MemoryAccepted{
                {{record_id, proposal_id, domain::MemoryScope::global,
                  std::nullopt, domain::MemoryKind::unknown, proposal.content,
                  proposal.rationale, proposal.source, proposal.producer},
                 domain::MemoryDecisionSource::user,
                 proposal_event_id}})};
  REQUIRE(fixture.store->append_events(journal->session_id, future_events));

  auto state =
      fixture.controller->inspect({domain::MemoryScope::global, std::nullopt});
  REQUIRE(state);
  REQUIRE(state->records.size() == 1);
  REQUIRE(state->records.front().projected.record.kind ==
          domain::MemoryKind::unknown);
  auto selected = runtime::select_memory_context(*fixture.controller,
                                                 {std::nullopt, 2048, 4096});
  REQUIRE(selected);
  REQUIRE(selected->empty());
}
