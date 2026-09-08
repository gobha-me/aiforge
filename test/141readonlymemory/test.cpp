#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/session_context.hpp>
#include <aiforge/testing/scripted_session_store.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <stop_token>
#include <string>
#include <unistd.h>
namespace {
using namespace aiforge;
using Code = runtime::MemoryControllerErrorCode;
template <class Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}
struct History {
  domain::SessionEventLog log{id<domain::SessionId>("session")};
  auto add(const std::string& run, domain::RunEventPayload payload) -> void {
    const auto sequence = log.last_sequence() + 1;
    const auto schema =
        std::holds_alternative<domain::RunStarted>(payload) ? 3U : 1U;
    REQUIRE(
        log.append({{id<domain::EventId>("event-" + std::to_string(sequence)),
                     id<domain::RunId>(run),
                     sequence,
                     schema,
                     domain::EventTimestamp{},
                     {},
                     {},
                     {}},
                    std::move(payload)}));
  }
  auto start(const std::string& run,
             domain::RunPurpose purpose = domain::RunPurpose::conversation)
      -> void {
    add(run, domain::RunStarted{id<domain::SurfaceId>("chat"),
                                id<domain::WorkspaceId>("chat"),
                                id<domain::PermissionProfileId>("observe"),
                                {},
                                {},
                                purpose});
  }
  auto turn(const std::string& run, std::string text = "old question") -> void {
    start(run);
    add(run, domain::UserContentAdded{{id<domain::MessageId>(run + "-user"),
                                       domain::Role::user,
                                       {domain::TextBlock{std::move(text)}},
                                       {}}});
    // Failed turns preserve complete original user input as a whole group.
    add(run,
        domain::RunFailed{{domain::ErrorCode::unavailable, "offline", false}});
  }
  auto rolling(std::vector<domain::RunId> pins = {}) -> void {
    start("policy", domain::RunPurpose::control);
    add("policy",
        domain::ConversationPolicySet{
            0, {1, domain::ConversationMode::rolling, std::move(pins)}});
    add("policy", domain::RunCompleted{});
  }
};
struct MemoryFixture {
  std::filesystem::path directory;
  std::unique_ptr<adapters::SqliteSessionStore> store;
  std::unique_ptr<runtime::MemoryController> controller;
  std::uint64_t suffix{};
  std::int64_t timestamp{};
  MemoryFixture() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-session-context-XXXXXX")
                       .string();
    pattern.push_back('\0');
    const auto* path = ::mkdtemp(pattern.data());
    REQUIRE(path != nullptr);
    directory = path;
    auto opened =
        adapters::SqliteSessionStore::open(directory / "sessions.sqlite3");
    REQUIRE(opened);
    store = std::move(*opened);
    controller = std::make_unique<runtime::MemoryController>(
        *store, [this] { return ++suffix; },
        [this] {
          return domain::EventTimestamp{std::chrono::milliseconds{++timestamp}};
        });
    History source;
    source.start("memory-source");
    source.add(
        "memory-source",
        domain::UserContentAdded{{id<domain::MessageId>("memory-user"),
                                  domain::Role::user,
                                  {domain::TextBlock{"Prefer concise answers"}},
                                  {}}});
    source.add("memory-source", domain::InferenceStarted{
                                    id<domain::InferenceId>("memory-inference"),
                                    id<domain::ModelId>("model")});
    source.add(
        "memory-source",
        domain::ToolProposed{
            id<domain::InvocationId>("memory-call"),
            "propose_memory",
            {"application/json",
             R"({"scope":"global","kind":"user_preference","content":"Prefer concise answers","rationale":"Keep the user preference","evidence_excerpt":"Prefer concise answers"})"},
            {}});
    source.add("memory-source",
               domain::ToolResultRecorded{
                   id<domain::InvocationId>("memory-call"),
                   {domain::StructuredDataBlock{
                       "application/json",
                       R"({"scope":"global","status":"proposed"})"}},
                   {}});
    REQUIRE(store->create_session(
        {source.log.session_id(), domain::EventTimestamp{}}));
    REQUIRE(store->append_events(source.log.session_id(), source.log.events()));
    REQUIRE(controller->capture_committed(
                source.log.session_id(), source.log.events(),
                {domain::MemoryCaptureMode::automatic,
                 domain::MemoryCaptureMode::automatic,
                 domain::MemoryCaptureMode::automatic, 2048},
                {}, "test") == 1);
  }
  ~MemoryFixture() {
    controller.reset();
    store.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  MemoryFixture(const MemoryFixture&) = delete;
  auto operator=(const MemoryFixture&) -> MemoryFixture& = delete;
};

} // namespace

TEST_CASE("read-only scoped selection never creates missing owner journals",
          "[readonlymemory][missing]") {
  const auto repo = id<domain::RepositoryId>("same-name");
  const auto persona = id<domain::PersonaId>("same-name");
  testing::ScriptedSessionStore store{
      {{testing::FindMemoryJournalCall{domain::MemoryOwner::persona(persona)},
        std::optional<storage::SessionInfo>{}},
       {testing::FindMemoryJournalCall{domain::MemoryOwner::repository(repo)},
        std::optional<storage::SessionInfo>{}},
       {testing::FindMemoryJournalCall{domain::MemoryOwner::global()},
        std::optional<storage::SessionInfo>{}}}};
  std::uint64_t identities{}, timestamps{};
  runtime::MemoryController memory{store, [&] { return ++identities; },
                                   [&] {
                                     ++timestamps;
                                     return domain::EventTimestamp{};
                                   }};
  const auto selected = runtime::select_memory_context_with_provenance(
      memory, {repo, persona, 2048, 4096, true});
  REQUIRE(selected);
  CHECK(selected->content.empty());
  CHECK(selected->selection.entries.empty());
  CHECK(domain::validate_memory_selection(selected->selection));
  CHECK(selected->selection.repository_id == repo);
  CHECK(selected->selection.persona_id == persona);
  CHECK(identities == 0);
  CHECK(timestamps == 0);
  CHECK(store.recorded_calls().size() == 3);
  CHECK(store.remaining_exchanges() == 0);
}

TEST_CASE(
    "read-only inspection validates inputs and propagates lookup failures",
    "[readonlymemory][failure]") {
  testing::ScriptedSessionStore store{{}};
  std::stop_source stop;
  runtime::MemoryController memory{store, {}, {}, stop.get_token()};
  auto owner = domain::MemoryOwner::global();
  SECTION("malformed owner") {
    owner.kind = domain::MemoryOwnerKind::unknown;
    const auto result = memory.inspect({owner}, true);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::invalid_configuration);
  }
  SECTION("cancelled") {
    stop.request_stop();
    const auto result = memory.inspect({owner}, true);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::cancelled);
  }
  CHECK(store.recorded_calls().empty());
}

TEST_CASE("read-only lookup errors are not treated as missing memory",
          "[readonlymemory][errors]") {
  auto error_code = storage::SessionStoreErrorCode::io_failure;
  auto expected = Code::storage_failure;
  SECTION("storage failure") {
  }
  SECTION("unsupported lookup") {
    error_code = storage::SessionStoreErrorCode::unsupported_version;
  }
  SECTION("cancelled lookup") {
    error_code = storage::SessionStoreErrorCode::cancelled;
    expected = Code::cancelled;
  }
  testing::ScriptedSessionStore store{
      {{testing::FindMemoryJournalCall{domain::MemoryOwner::global()},
        storage::SessionStoreError{error_code, "lookup refused", true}}}};
  runtime::MemoryController memory{store, {}, {}};
  const auto result = runtime::select_memory_context_with_provenance(
      memory, {{}, {}, 2048, 4096, true});
  REQUIRE_FALSE(result);
  CHECK(result.error().code == expected);
  CHECK(result.error().message == "lookup refused");
  CHECK(result.error().retryable);
  CHECK(store.recorded_calls().size() == 1);
}

TEST_CASE("read-only journal reconstruction preserves owner and replay guards",
          "[readonlymemory][source]") {
  const auto journal = id<domain::SessionId>("journal");
  std::vector<domain::RunEvent> events;
  auto expected = "memory journal contains a different owner";
  SECTION("foreign owner") {
    domain::MemoryProposal proposal{
        id<domain::MemoryProposalId>("proposal"),
        id<domain::MemoryRecordId>("record"),
        domain::MemoryOwner::repository(id<domain::RepositoryId>("foreign")),
        domain::MemoryKind::project_convention,
        "Use clear names",
        "Remember style",
        "Use clear names",
        {id<domain::SessionId>("source"),
         id<domain::RunId>("source-run"),
         id<domain::InvocationId>("call"),
         {id<domain::EventId>("source-event")}},
        {id<domain::ModelId>("model"), "test", "1"},
        {},
        {}};
    REQUIRE(domain::validate_memory_proposal(proposal));
    events.push_back({{id<domain::EventId>("proposal-event"),
                       id<domain::RunId>("journal-run"),
                       1,
                       1,
                       {},
                       {},
                       {},
                       {}},
                      domain::MemoryProposed{proposal}});
  }
  SECTION("malformed projection sequence") {
    expected = "memory event sequence does not increase";
    events.push_back({{id<domain::EventId>("bad-event"),
                       id<domain::RunId>("journal-run"),
                       0,
                       1,
                       {},
                       {},
                       {},
                       {}},
                      domain::RunCompleted{}});
  }
  testing::ScriptedSessionStore store{
      {{testing::FindMemoryJournalCall{domain::MemoryOwner::global()},
        std::optional{storage::SessionInfo{journal, {}, {}, 1}}},
       {testing::ReplayEventsCall{journal}, events}}};
  runtime::MemoryController memory{store, {}, {}};
  const auto result = memory.inspect({domain::MemoryOwner::global()}, true);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::storage_failure);
  CHECK(result.error().message == expected);
  CHECK(store.recorded_calls().size() == 2);
}

TEST_CASE("memory lookup defaults to unsupported without listing sessions",
          "[readonlymemory][compatibility]") {
  testing::ScriptedSessionStore store{{}};
  using Base = storage::SessionStore;
  const auto result =
      store.Base::find_memory_journal(domain::MemoryOwner::global());
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        storage::SessionStoreErrorCode::unsupported_version);
  CHECK(store.recorded_calls().empty());
}

TEST_CASE("SQLite read-only lookup keeps absent scopes absent and returns the "
          "same existing selection",
          "[readonlymemory][sqlite]") {
  MemoryFixture f;
  const auto repo = id<domain::RepositoryId>("same-name");
  const auto persona = id<domain::PersonaId>("same-name");
  const auto global =
      f.store->find_memory_journal(domain::MemoryOwner::global());
  REQUIRE(global);
  REQUIRE(*global);
  const auto history = f.store->replay_events((*global)->session_id);
  REQUIRE(history);
  const auto identities = f.suffix;
  const auto timestamps = f.timestamp;
  // A concurrent writer reservation permits reads but rejects an attempted
  // implicit journal-creation transaction on the store's connection.
  sqlite3* writer{};
  REQUIRE(sqlite3_open((f.directory / "sessions.sqlite3").c_str(), &writer) ==
          SQLITE_OK);
  REQUIRE(sqlite3_exec(writer, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
  auto readonly = runtime::select_memory_context_with_provenance(
      *f.controller, {repo, persona, 2048, 4096, true});
  REQUIRE(sqlite3_exec(writer, "ROLLBACK", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
  REQUIRE(sqlite3_close(writer) == SQLITE_OK);
  REQUIRE(readonly);
  REQUIRE(readonly->content.size() == 1);
  CHECK(f.suffix == identities);
  CHECK(f.timestamp == timestamps);
  const auto project =
      f.store->find_memory_journal(domain::MemoryOwner::repository(repo));
  const auto character =
      f.store->find_memory_journal(domain::MemoryOwner::persona(persona));
  REQUIRE(project);
  REQUIRE(character);
  CHECK_FALSE(*project);
  CHECK_FALSE(*character);
  CHECK(f.store->replay_events((*global)->session_id) == history);
  auto normal = runtime::select_memory_context_with_provenance(
      *f.controller, {repo, persona, 2048, 4096});
  REQUIRE(normal);
  CHECK(normal->selection == readonly->selection);
  CHECK(normal->content == readonly->content);
  const auto project_created =
      f.store->find_memory_journal(domain::MemoryOwner::repository(repo));
  const auto persona_created =
      f.store->find_memory_journal(domain::MemoryOwner::persona(persona));
  REQUIRE(project_created);
  REQUIRE(persona_created);
  REQUIRE(*project_created);
  REQUIRE(*persona_created);
  CHECK((*project_created)->session_id != (*persona_created)->session_id);
  CHECK((*project_created)->session_id != (*global)->session_id);
  CHECK((*persona_created)->session_id != (*global)->session_id);
}

TEST_CASE("SQLite memory lookup rejects malformed owners and cancellation",
          "[readonlymemory][sqlite][failure]") {
  MemoryFixture f;
  auto owner = domain::MemoryOwner::global();
  std::stop_source stop;
  auto expected = storage::SessionStoreErrorCode::invalid_argument;
  SECTION("invalid owner") {
    owner.persona_id = id<domain::PersonaId>("foreign");
  }
  SECTION("cancelled") {
    stop.request_stop();
    expected = storage::SessionStoreErrorCode::cancelled;
  }
  const auto found = f.store->find_memory_journal(owner, stop.get_token());
  REQUIRE_FALSE(found);
  CHECK(found.error().code == expected);
}
