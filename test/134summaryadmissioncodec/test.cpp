#include <sqlite3.h>
#include <unistd.h>

#include <aiforge/adapters/sqlite_session_store.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <variant>

namespace {
using namespace aiforge;
template <class Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}
class Database final {
 public:
  Database() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "aiforge-summary-admission-codec-XXXXXX")
                              .string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    m_directory = pattern;
    path = m_directory / "session.sqlite3";
    reopen();
    REQUIRE(store->create_session({session, domain::EventTimestamp{}}));
  }
  ~Database() {
    store.reset();
    std::error_code error;
    std::filesystem::remove_all(m_directory, error);
  }
  Database(const Database&) = delete;
  auto operator=(const Database&) -> Database& = delete;
  auto reopen() -> void {
    auto opened = adapters::SqliteSessionStore::open(path);
    REQUIRE(opened);
    store = std::move(*opened);
  }
  auto mutate(const std::string& sql) -> void {
    store.reset();
    sqlite3* connection{};
    REQUIRE(sqlite3_open(path.c_str(), &connection) == SQLITE_OK);
    const auto result =
        sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, nullptr);
    const auto closed = sqlite3_close(connection);
    REQUIRE(result == SQLITE_OK);
    REQUIRE(closed == SQLITE_OK);
    reopen();
  }
  [[nodiscard]] auto payload() const -> std::string {
    sqlite3* connection{};
    REQUIRE(sqlite3_open(path.c_str(), &connection) == SQLITE_OK);
    sqlite3_stmt* statement{};
    REQUIRE(sqlite3_prepare_v2(
                connection,
                "SELECT payload_json FROM events ORDER BY sequence LIMIT 1", -1,
                &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto* bytes = sqlite3_column_text(statement, 0);
    REQUIRE(bytes != nullptr);
    std::string result{reinterpret_cast<const char*>(bytes)};
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    REQUIRE(sqlite3_close(connection) == SQLITE_OK);
    return result;
  }
  std::filesystem::path path;
  domain::SessionId session{id<domain::SessionId>("session")};
  std::unique_ptr<adapters::SqliteSessionStore> store;

 private:
  std::filesystem::path m_directory;
};
auto event(domain::RunEventPayload payload, std::uint32_t schema = 3,
           std::uint64_t sequence = 1) -> domain::RunEvent {
  return {{id<domain::EventId>("event-" + std::to_string(sequence)),
           id<domain::RunId>("run"),
           sequence,
           schema,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           {},
           {},
           {}},
          std::move(payload)};
}
auto started() -> domain::RunStarted {
  return {id<domain::SurfaceId>("test"),
          id<domain::WorkspaceId>("chat"),
          id<domain::PermissionProfileId>("observe"),
          {}};
}
using namespace domain;
auto message() -> Message {
  return {
      id<MessageId>("message"), Role::user, {TextBlock{"story"}}, std::nullopt};
}
auto admission() -> ConversationAdmission {
  const auto text = message();
  const auto digest = normalized_conversation_message_digest(text).value();
  ConversationAdmission value{.session_id = id<SessionId>("session"),
                              .model_id = id<ModelId>("model"),
                              .policy_event_id = {},
                              .capacity = {},
                              .groups = {},
                              .omitted_groups_digest = {},
                              .admission_digest = {}};
  value.source_snapshot_sequence = 10;
  value.capacity = {100, 10, 5};
  value.mandatory_input_tokens = 15;
  value.groups = {
      {id<RunId>("run"),
       {{id<EventId>("event"),
         2,
         id<ContextEntryId>("entry"),
         text.message_id,
         {id<ContextSourceId>("source"), "session:source", "source-digest"},
         1,
         20,
         ContextContentKind::conversation,
         digest}},
       false}};
  return value;
}
auto summary() -> ConversationAdmittedSummary {
  Message text{id<MessageId>("summary-message"),
               Role::evidence,
               {TextBlock{"Derived facts"}},
               {}};
  return {{id<ConversationSummaryId>("summary"),
           1,
           {"sha256", std::string(64, 'a'), 200}},
          id<EventId>("activation"),
          9,
          1,
          id<ContextEntryId>("summary-entry"),
          text.message_id,
          {id<ContextSourceId>("summary-source"), "derived summary",
           "candidate-digest"},
          2,
          10,
          normalized_conversation_message_digest(text).value()};
}
auto with_summary() -> ConversationAdmission {
  auto value = admission();
  value.version = 2;
  value.mode = ConversationMode::rolling;
  value.policy_revision = 1;
  value.policy_event_id = id<EventId>("policy");
  value.summaries.push_back(summary());
  value.groups[0].entries[0].order = 3;
  return value;
}
auto sealed_start(bool summaries = true) -> domain::RunStarted {
  auto value = started();
  value.conversation_admission = summaries ? with_summary() : admission();
  value.conversation_admission->version = 2;
  REQUIRE(domain::seal_conversation_admission(*value.conversation_admission));
  return value;
}
auto rejected_replay(Database& database, const std::string& expression)
    -> void {
  database.mutate("UPDATE events SET payload_json=" + expression);
  const auto replay = database.store->replay_events(database.session);
  REQUIRE_FALSE(replay);
  CHECK(replay.error().code == storage::SessionStoreErrorCode::corrupt);
}
} // namespace

TEST_CASE("summary-aware start codec rejects malformed v2 evidence references",
          "[storage][summaryadmission][failure]") {
  Database database;
  REQUIRE(database.store->append_events(database.session,
                                        std::array{event(sealed_start(), 4)}));
  std::string expression;
  SECTION("missing summaries array") {
    expression =
        "json_remove(payload_json,'$.conversation_admission.summaries')";
  }
  SECTION("null summaries array") {
    expression =
        "json_set(payload_json,'$.conversation_admission.summaries',NULL)";
  }
  SECTION("object summaries array") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries',"
                 "json('{}'))";
  }
  SECTION("unknown admission version") {
    expression = "json_set(payload_json,'$.conversation_admission.version',99)";
  }
  SECTION("truncated admission version") {
    expression =
        "json_set(payload_json,'$.conversation_admission.version',4294967298)";
  }
  SECTION("extra reference field") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "extra',1)";
  }
  SECTION("extra candidate field") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "candidate.extra',1)";
  }
  SECTION("extra provenance field") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "provenance.extra',1)";
  }
  SECTION("extra digest field") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "message_digest.extra',1)";
  }
  SECTION("real candidate revision") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "candidate.revision',1.0)";
  }
  SECTION("string activation sequence") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "activation_sequence','9')";
  }
  SECTION("negative estimate") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "estimated_tokens',-1)";
  }
  SECTION("boolean estimate") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "estimated_tokens',json('true'))";
  }
  SECTION("changed activation identity") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "activation_event_id','reactivated')";
  }
  SECTION("changed candidate digest") {
    expression =
        "json_set(payload_json,'$.conversation_admission.summaries[0]."
        "candidate.candidate_digest.value',replace(hex(zeroblob(32)),'0','b'))";
  }
  SECTION("changed evidence order") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "order',1)";
  }
  SECTION("changed evidence digest") {
    expression = "json_set(payload_json,'$.conversation_admission.summaries[0]."
                 "message_digest.value',replace(hex(zeroblob(32)),'0','b'))";
  }
  SECTION("no embedded admission") {
    expression = "json_set(payload_json,'$.conversation_admission',NULL)";
  }
  SECTION("control purpose") {
    expression = "json_set(payload_json,'$.purpose','control')";
  }
  SECTION("summary purpose") {
    expression = "json_set(payload_json,'$.purpose','summary')";
  }
  SECTION("too many summaries") {
    expression =
        "json_set(payload_json,'$.conversation_admission.summaries',(WITH "
        "RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<33) "
        "SELECT "
        "json_group_array(json(json_extract(events.payload_json,'$."
        "conversation_admission.summaries[0]')||substr(n.x,1,0))) FROM n))";
  }
  rejected_replay(database, expression);
}

TEST_CASE("summary-aware start codec refuses version and schema substitution",
          "[storage][summaryadmission][failure]") {
  Database database;
  std::uint32_t schema = 4;
  auto value = sealed_start();
  SECTION("schema3 cannot contain v2") {
    schema = 3;
  }
  SECTION("schema4 cannot contain v1") {
    value.conversation_admission = admission();
    REQUIRE(domain::seal_conversation_admission(*value.conversation_admission));
  }
  SECTION("schema4 requires admission") {
    value.conversation_admission.reset();
  }
  SECTION("schema4 cannot start a control run") {
    value.purpose = domain::RunPurpose::control;
  }
  SECTION("schema4 cannot start a summary producer") {
    value.purpose = domain::RunPurpose::summary;
  }
  const auto appended = database.store->append_events(
      database.session, std::array{event(value, schema)});
  REQUIRE_FALSE(appended);
  CHECK(appended.error().code ==
        storage::SessionStoreErrorCode::invalid_argument);
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  CHECK(replay->empty());
}

TEST_CASE("persisted start schema3 rejects a downgraded v2 admission",
          "[storage][summaryadmission][failure]") {
  Database database;
  REQUIRE(database.store->append_events(database.session,
                                        std::array{event(sealed_start(), 4)}));
  database.mutate("UPDATE events SET schema_version=3");
  const auto replay = database.store->replay_events(database.session);
  REQUIRE_FALSE(replay);
  CHECK(replay.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("legacy start admission forbids a summaries field even when empty",
          "[storage][summaryadmission][failure]") {
  Database database;
  auto value = started();
  value.conversation_admission = admission();
  REQUIRE(domain::seal_conversation_admission(*value.conversation_admission));
  REQUIRE(database.store->append_events(database.session,
                                        std::array{event(value, 3)}));
  rejected_replay(
      database,
      "json_set(payload_json,'$.conversation_admission.summaries',json('[]'))");
}

TEST_CASE("v1 start codec retains its frozen canonical JSON document",
          "[storage][summaryadmission][legacy]") {
  Database database;
  auto value = started();
  value.conversation_admission = admission();
  REQUIRE(domain::seal_conversation_admission(*value.conversation_admission));
  const auto expected = event(value, 3);
  REQUIRE(
      database.store->append_events(database.session, std::array{expected}));
  CHECK(
      database.payload() ==
      R"golden({"conversation_admission":{"admission_digest":{"algorithm":"sha256","byte_size":269,"value":"3dde7817c883a8c3ee4740ef7847c9450155456897b85dd5268ae33927a30d50"},"capacity":{"context_window_tokens":100,"reserved_input_tokens":5,"reserved_output_tokens":10},"estimator_version":1,"groups":[{"entries":[{"completed_event_id":"event","entry_id":"entry","estimated_tokens":20,"event_sequence":2,"kind":"conversation","message_digest":{"algorithm":"sha256","byte_size":71,"value":"30687eaa5e6d6d695c3307afdc2865f0ed41f1869843890e9317ceb7f6c697ee"},"message_id":"message","order":1,"provenance":{"digest":"source-digest","source_id":"source","source_location":"session:source"}}],"pinned":false,"run_id":"run"}],"mandatory_input_tokens":15,"mode":"full","model_id":"model","omitted_group_count":0,"omitted_groups_digest":null,"policy_event_id":null,"policy_revision":0,"session_id":"session","source_snapshot_sequence":10,"version":1},"memory_selection":null,"permission_profile_id":"observe","persona_id":null,"purpose":"conversation","surface_id":"test","workspace_id":"chat"})golden");
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  CHECK(*replay == std::vector{expected});
}

TEST_CASE(
    "summary-aware starts preserve explicit empty and populated admissions",
    "[storage][summaryadmission][roundtrip]") {
  Database database;
  auto legacy = started();
  legacy.conversation_admission = admission();
  REQUIRE(domain::seal_conversation_admission(*legacy.conversation_admission));
  auto control = started();
  control.purpose = domain::RunPurpose::control;
  auto producer = control;
  producer.purpose = domain::RunPurpose::summary;
  const std::vector<domain::RunEvent> expected{
      event(legacy, 3, 1), event(sealed_start(false), 4, 2),
      event(sealed_start(), 4, 3), event(control, 3, 4), event(producer, 3, 5)};
  REQUIRE(database.store->append_events(database.session, expected));
  database.store.reset();
  database.reopen();
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  CHECK(*replay == expected);
}

TEST_CASE("unknown future start schemas remain opaque",
          "[storage][summaryadmission][versions]") {
  Database database;
  REQUIRE(database.store->append_events(database.session,
                                        std::array{event(sealed_start(), 4)}));
  const auto payload = database.payload();
  database.mutate("UPDATE events SET schema_version=99");
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  REQUIRE(replay->size() == 1);
  REQUIRE(
      std::holds_alternative<domain::UnknownEvent>(replay->front().payload));
  const auto& unknown = std::get<domain::UnknownEvent>(replay->front().payload);
  CHECK(unknown.type_name == "run.started");
  CHECK(unknown.payload.data == payload);
}
