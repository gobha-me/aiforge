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
                           "aiforge-conversation-codec-XXXXXX")
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
auto admission(bool populated = false) -> domain::ConversationAdmission {
  domain::ConversationAdmission result{1,
                                       1,
                                       id<domain::SessionId>("session"),
                                       id<domain::ModelId>("model"),
                                       10,
                                       {},
                                       0,
                                       domain::ConversationMode::full,
                                       {1000, 100, 50},
                                       20,
                                       {},
                                       0,
                                       {},
                                       {}};
  if (populated) {
    result.policy_event_id = id<domain::EventId>("policy");
    result.policy_revision = 1;
    result.mode = domain::ConversationMode::rolling;
    result.omitted_group_count = 1;
    result.omitted_groups_digest =
        domain::ContentDigest{"sha256", std::string(64, 'b'), 10};
    domain::ConversationAdmittedGroup group{
        id<domain::RunId>("source-run"), {}, true};
    for (const auto& [sequence, order] :
         std::array{std::pair{2U, 1U}, std::pair{5U, 2U}, std::pair{4U, 3U}}) {
      const auto suffix = std::to_string(order);
      group.entries.push_back({id<domain::EventId>("source-event-" + suffix),
                               sequence,
                               id<domain::ContextEntryId>("entry-" + suffix),
                               id<domain::MessageId>("message-" + suffix),
                               {id<domain::ContextSourceId>("source-" + suffix),
                                "session history", "digest"},
                               order,
                               10,
                               order == 3
                                   ? domain::ContextContentKind::tool_result
                                   : domain::ContextContentKind::conversation,
                               {"sha256", std::string(64, 'a'), 10}});
    }
    result.groups.push_back(std::move(group));
  }
  REQUIRE(domain::seal_conversation_admission(result));
  return result;
}
auto rejected_replay(Database& database) -> void {
  auto result = database.store->replay_events(database.session);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == storage::SessionStoreErrorCode::corrupt);
}
} // namespace

TEST_CASE("conversation start codec rejects malformed admission metadata",
          "[storage][conversation][failure]") {
  Database database;
  auto start = started();
  start.conversation_admission = admission();
  REQUIRE(database.store->append_events(database.session,
                                        std::array{event(start)}));
  std::string expression;
  SECTION("unknown purpose") {
    expression = "json_set(payload_json,'$.purpose','other')";
  }
  SECTION("missing purpose") {
    expression = "json_remove(payload_json,'$.purpose')";
  }
  SECTION("extra envelope field") {
    expression = "json_set(payload_json,'$.extra',1)";
  }
  SECTION("extra admission field") {
    expression = "json_set(payload_json,'$.conversation_admission.extra',1)";
  }
  SECTION("numeric coercion") {
    expression = "json_set(payload_json,'$.conversation_admission.mandatory_"
                 "input_tokens',20.0)";
  }
  SECTION("negative value") {
    expression = "json_set(payload_json,'$.conversation_admission.source_"
                 "snapshot_sequence',-1)";
  }
  SECTION("overflowing version") {
    expression =
        "json_set(payload_json,'$.conversation_admission.version',4294967297)";
  }
  SECTION("unknown admission version") {
    expression = "json_set(payload_json,'$.conversation_admission.version',2)";
  }
  SECTION("unknown estimator") {
    expression =
        "json_set(payload_json,'$.conversation_admission.estimator_version',2)";
  }
  SECTION("unknown mode") {
    expression =
        "json_set(payload_json,'$.conversation_admission.mode','automatic')";
  }
  SECTION("unsealed") {
    expression = "json_set(payload_json,'$.conversation_admission.admission_"
                 "digest',NULL)";
  }
  SECTION("seal corrupted") {
    expression = "json_set(payload_json,'$.conversation_admission.mandatory_"
                 "input_tokens',21)";
  }
  SECTION("oversized group array") {
    expression = "json_set(payload_json,'$.conversation_admission.groups',"
                 "(WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM "
                 "n WHERE x<4097) "
                 "SELECT "
                 "json_group_array(json_object('run_id','r','entries',json('[]'"
                 "),'pinned',json('false'))) FROM n))";
  }
  SECTION("aggregate entry bound across groups") {
    expression = "json_set(payload_json,'$.conversation_admission.groups',"
                 "(WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM "
                 "n WHERE x<8193), "
                 "a(v) AS (SELECT json_group_array(json('{}')) FROM n) "
                 "SELECT "
                 "json_array(json_object('run_id','r1','entries',json(v),'"
                 "pinned',json('false')),"
                 "json_object('run_id','r2','entries',json(v),'pinned',json('"
                 "false'))) FROM a))";
  }
  REQUIRE_FALSE(expression.empty());
  database.mutate("UPDATE events SET payload_json=" + expression);
  rejected_replay(database);
}

TEST_CASE("legacy run start schemas cannot hide new admission fields",
          "[storage][conversation][compatibility][failure]") {
  Database database;
  auto start = started();
  SECTION("schema one rejects admission on write") {
    start.conversation_admission = admission();
    CHECK_FALSE(database.store->append_events(database.session,
                                              std::array{event(start, 1)}));
  }
  SECTION("schema one rejects nonconversation purpose") {
    start.purpose = domain::RunPurpose::control;
    CHECK_FALSE(database.store->append_events(database.session,
                                              std::array{event(start, 1)}));
  }
  SECTION("schema two requires memory selection") {
    CHECK_FALSE(database.store->append_events(database.session,
                                              std::array{event(start, 2)}));
  }
  SECTION("schema one does not reinterpret admission on read") {
    start.conversation_admission = admission();
    REQUIRE(database.store->append_events(database.session,
                                          std::array{event(start)}));
    database.mutate("UPDATE events SET schema_version=1");
    rejected_replay(database);
  }
  SECTION("unknown start schema remains opaque") {
    REQUIRE(database.store->append_events(database.session,
                                          std::array{event(start)}));
    database.mutate("UPDATE events SET schema_version=99");
    auto replay = database.store->replay_events(database.session);
    REQUIRE(replay);
    REQUIRE(
        std::holds_alternative<domain::UnknownEvent>(replay->front().payload));
  }
}

TEST_CASE("conversation policy codec rejects invalid revisions and shapes",
          "[storage][conversation][failure]") {
  Database database;
  const auto policy = event(
      domain::ConversationPolicySet{
          0,
          {1, domain::ConversationMode::rolling, {id<domain::RunId>("pin")}}},
      1);
  REQUIRE(database.store->append_events(database.session, std::array{policy}));
  SECTION("unknown policy mode") {
    database.mutate(
        "UPDATE events SET "
        "payload_json=json_set(payload_json,'$.policy.mode','automatic')");
  }
  SECTION("numeric coercion") {
    database.mutate(
        "UPDATE events SET "
        "payload_json=json_set(payload_json,'$.previous_revision',0.0)");
  }
  SECTION("revision transition mismatch") {
    database.mutate(
        "UPDATE events SET "
        "payload_json=json_set(payload_json,'$.previous_revision',1)");
  }
  SECTION("extra policy field") {
    database.mutate("UPDATE events SET "
                    "payload_json=json_set(payload_json,'$.policy.extra',1)");
  }
  SECTION("too many pins") {
    database.mutate(
        "UPDATE events SET "
        "payload_json=json_set(payload_json,'$.policy.pinned_run_ids',"
        "(WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE "
        "x<4097) "
        "SELECT json_group_array('pin') FROM n))");
  }
  rejected_replay(database);
}

TEST_CASE("conversation policy unknown schema is preserved for inspection",
          "[storage][conversation][compatibility]") {
  Database database;
  REQUIRE(database.store->append_events(
      database.session, std::array{event(
                            domain::ConversationPolicySet{
                                0, {1, domain::ConversationMode::rolling, {}}},
                            1)}));
  database.mutate("UPDATE events SET schema_version=2");
  auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  CHECK(std::holds_alternative<domain::UnknownEvent>(replay->front().payload));
}

TEST_CASE("conversation start and policy codecs preserve exact values",
          "[storage][conversation][roundtrip]") {
  Database database;
  std::vector<domain::RunEvent> events;
  events.push_back(event(started(), 1, 1));
  auto with_memory = started();
  with_memory.memory_selection =
      domain::MemorySelection{1, {}, {}, 100, 100, {}};
  REQUIRE(domain::seal_memory_selection(*with_memory.memory_selection));
  events.push_back(event(with_memory, 2, 2));
  events.push_back(event(started(), 3, 3));
  auto empty = started();
  empty.conversation_admission = admission();
  events.push_back(event(empty, 3, 4));
  auto populated = with_memory;
  populated.conversation_admission = admission(true);
  events.push_back(event(populated, 3, 5));
  auto control = started();
  control.purpose = domain::RunPurpose::control;
  events.push_back(event(control, 3, 6));
  control.purpose = domain::RunPurpose::summary;
  events.push_back(event(control, 3, 7));
  events.push_back(event(
      domain::ConversationPolicySet{
          0,
          {1, domain::ConversationMode::rolling, {id<domain::RunId>("pin")}}},
      1, 8));
  REQUIRE(database.store->append_events(database.session, events));
  database.store.reset();
  database.reopen();
  auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  CHECK(*replay == events);
  CHECK_FALSE(std::get<domain::RunStarted>((*replay)[2].payload)
                  .conversation_admission);
  CHECK(std::get<domain::RunStarted>((*replay)[3].payload)
            .conversation_admission);
}
