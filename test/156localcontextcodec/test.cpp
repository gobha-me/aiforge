#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/local_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace aiforge;
template <typename Id> auto id(std::string text) -> Id {
  return Id::from(std::move(text)).value();
}
class Database final {
 public:
  Database() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-local-codec-XXXXXX")
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
    store.reset();
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
  const domain::SessionId session{id<domain::SessionId>("session")};
  std::unique_ptr<adapters::SqliteSessionStore> store;

 private:
  std::filesystem::path m_directory;
};

auto admission() -> domain::LocalContextAdmission {
  constexpr std::string_view bytes = "hello";
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{bytes.data(), bytes.size()}));
  domain::LocalContextEvidence entry{
      id<domain::EvidenceId>("local-evidence-" + std::string(64, 'b')),
      id<domain::ContextEntryId>("local-context-entry-" + std::string(64, 'c')),
      id<domain::MessageId>("local-context-message-" + std::string(64, 'd')),
      id<domain::ContextSourceId>("local-context-source-" +
                                  std::string(64, 'e')),
      {{1, std::string(64, 'a')},
       "notes.txt",
       {"sha256", hash.finish(), bytes.size()}},
      3,
      bytes.size(),
      domain::LocalContextDecision::admitted};
  domain::LocalContextAdmission result{
      1, id<domain::SessionId>("session"), 1, {1024, 16, 0}, {entry}, {}};
  const auto sealed = domain::seal_local_context_admission(result);
  INFO((sealed ? "sealed" : sealed.error().message));
  REQUIRE(sealed);
  return result;
}
auto event(domain::RunEventPayload payload, std::uint64_t sequence = 1,
           std::uint32_t schema = 1) -> domain::RunEvent {
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
auto local(domain::LocalContextAdmission value = admission(),
           std::uint64_t sequence = 1, std::uint32_t schema = 1)
    -> domain::RunEvent {
  return event(
      domain::LocalContextAdmitted{id<domain::InferenceId>("inference"),
                                   std::move(value)},
      sequence, schema);
}
auto started(std::optional<std::uint32_t> admission_version = std::nullopt)
    -> domain::RunStarted {
  domain::RunStarted value{id<domain::SurfaceId>("test"),
                           id<domain::WorkspaceId>("chat"),
                           id<domain::PermissionProfileId>("observe"),
                           {}};
  if (admission_version) {
    domain::ConversationAdmission conversation{
        .session_id = id<domain::SessionId>("session"),
        .model_id = id<domain::ModelId>("model"),
        .policy_event_id = {},
        .capacity = {1024, 16, 0},
        .groups = {},
        .omitted_groups_digest = {},
        .admission_digest = {}};
    conversation.version = *admission_version;
    REQUIRE(domain::seal_conversation_admission(conversation));
    value.conversation_admission = std::move(conversation);
  }
  return value;
}
auto persist(Database& database) -> void {
  const auto saved =
      database.store->append_events(database.session, std::array{local()});
  INFO((saved ? "saved" : saved.error().message));
  REQUIRE(saved);
}
auto reject_replay(Database& database, const std::string& expression) -> void {
  database.mutate("UPDATE events SET payload_json=" + expression);
  const auto replay = database.store->replay_events(database.session);
  INFO((replay ? "unexpected replay" : replay.error().message));
  REQUIRE_FALSE(replay);
  CHECK(replay.error().code == storage::SessionStoreErrorCode::corrupt);
}
} // namespace

TEST_CASE("local admission codec rejects extra missing malformed and coerced "
          "fields") {
  Database database;
  persist(database);
  std::string expression;
  SECTION("extra envelope") {
    expression = "json_set(payload_json,'$.extra',1)";
  }
  SECTION("missing inference") {
    expression = "json_remove(payload_json,'$.inference_id')";
  }
  SECTION("extra admission") {
    expression = "json_set(payload_json,'$.admission.extra',1)";
  }
  SECTION("extra evidence") {
    expression = "json_set(payload_json,'$.admission.evidence[0].extra',1)";
  }
  SECTION("extra source") {
    expression =
        "json_set(payload_json,'$.admission.evidence[0].source.extra',1)";
  }
  SECTION("extra root") {
    expression =
        "json_set(payload_json,'$.admission.evidence[0].source.root.extra',1)";
  }
  SECTION("extra digest") {
    expression =
        "json_set(payload_json,'$.admission.admission_digest.extra',1)";
  }
  SECTION("extra capacity") {
    expression = "json_set(payload_json,'$.admission.capacity.extra',1)";
  }
  SECTION("real revision") {
    expression = "json_set(payload_json,'$.admission.selection_revision',1.0)";
  }
  SECTION("negative order") {
    expression = "json_set(payload_json,'$.admission.evidence[0].order',-1)";
  }
  SECTION("boolean estimate") {
    expression = "json_set(payload_json,'$.admission.evidence[0].estimated_"
                 "tokens',json('true'))";
  }
  SECTION("string capacity") {
    expression = "json_set(payload_json,'$.admission.capacity.context_window_"
                 "tokens','1024')";
  }
  SECTION("wrong evidence type") {
    expression = "json_set(payload_json,'$.admission.evidence',json('{}'))";
  }
  SECTION("unknown decision") {
    expression =
        "json_set(payload_json,'$.admission.evidence[0].decision','optional')";
  }
  SECTION("missing seal") {
    expression = "json_remove(payload_json,'$.admission.admission_digest')";
  }
  SECTION("corrupt seal") {
    expression = "json_set(payload_json,'$.admission.selection_revision',2)";
  }
  SECTION("unsupported digest") {
    expression = "json_set(payload_json,'$.admission.evidence[0].source."
                 "content_digest.algorithm','md5')";
  }
  SECTION("absolute raw path") {
    expression = "json_set(payload_json,'$.admission.evidence[0].source."
                 "relative_path','/private/notes')";
  }
  SECTION("parent traversal") {
    expression = "json_set(payload_json,'$.admission.evidence[0].source."
                 "relative_path','../notes')";
  }
  SECTION("nested admission version") {
    expression = "json_set(payload_json,'$.admission.version',2)";
  }
  SECTION("overflowed admission version") {
    expression = "json_set(payload_json,'$.admission.version',4294967297)";
  }
  SECTION("nested root version") {
    expression = "json_set(payload_json,'$.admission.evidence[0].source.root."
                 "version',2)";
  }
  SECTION("duplicate envelope keys") {
    expression =
        "'{\"inference_id\":\"inference\",\"inference_id\":\"other\","
        "\"admission\":'||json_extract(payload_json,'$.admission')||'}'";
  }
  reject_replay(database, expression);
}

TEST_CASE("local admission codec bounds references strings aggregate metadata "
          "and shape") {
  Database database;
  persist(database);
  std::string expression;
  SECTION("evidence count") {
    expression =
        "json_set(payload_json,'$.admission.evidence',(WITH RECURSIVE n(x) AS "
        "(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<65) SELECT "
        "json_group_array(json(json_extract(events.payload_json,'$.admission."
        "evidence[0]')||substr(n.x,1,0))) FROM n))";
  }
  SECTION("oversized path") {
    expression = "json_set(payload_json,'$.admission.evidence[0].source."
                 "relative_path',replace(hex(zeroblob(2049)),'0','x'))";
  }
  SECTION("aggregate strings") {
    expression = "json_set(payload_json,'$.padding',(WITH RECURSIVE n(x) AS "
                 "(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<140) SELECT "
                 "json_group_array(replace(hex(zeroblob(2048)),'0','x')||"
                 "substr(n.x,1,0)) FROM n))";
  }
  SECTION("queued nodes") {
    expression = "json_set(payload_json,'$.padding',(WITH RECURSIVE n(x) AS "
                 "(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<4097) SELECT "
                 "json_group_array(x) FROM n))";
  }
  SECTION("deep shape") {
    expression =
        "json_set(payload_json,'$.padding',json('[[[[[[[[[[[[0]]]]]]]]]]]]'))";
  }
  reject_replay(database, expression);
}

TEST_CASE("local admission write failures preserve an atomic event batch") {
  Database database;
  auto value = admission();
  SECTION("unsealed") {
    value.admission_digest.reset();
  }
  SECTION("changed bytes metadata") {
    value.evidence.front().source.content_digest.byte_size++;
  }
  SECTION("excessive references") {
    value.evidence.resize(65, value.evidence.front());
  }
  const auto before = database.store->replay_events(database.session);
  REQUIRE(before);
  const auto result = database.store->append_events(
      database.session,
      std::array{event(domain::RunCompleted{}), local(value, 2)});
  REQUIRE_FALSE(result);
  const auto after = database.store->replay_events(database.session);
  REQUIRE(after);
  CHECK(*after == *before);
}

TEST_CASE("local admission and inference roll back together after database "
          "insert failure") {
  Database database;
  database.mutate(
      "CREATE TRIGGER reject_inference BEFORE INSERT ON events WHEN "
      "NEW.sequence=2 BEGIN SELECT RAISE(ABORT,'fixture failure'); END");
  const auto result = database.store->append_events(
      database.session,
      std::array{
          local(),
          event(domain::InferenceStarted{id<domain::InferenceId>("inference"),
                                         id<domain::ModelId>("model")},
                2)});
  REQUIRE_FALSE(result);
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  CHECK(replay->empty());
}

TEST_CASE("future local event envelopes remain opaque while typed and forged "
          "old envelopes refuse") {
  Database database;
  SECTION("future opaque envelope") {
    const auto opaque = event(
        domain::UnknownEvent{"run.local_context_admitted",
                             domain::StructuredDataBlock{"application/json",
                                                         "{\"future\":true}"}},
        1, 2);
    REQUIRE(
        database.store->append_events(database.session, std::array{opaque}));
    database.reopen();
    const auto replay = database.store->replay_events(database.session);
    REQUIRE(replay);
    REQUIRE(replay->size() == 1);
    CHECK(replay->front() == opaque);
  }
  SECTION("typed event cannot claim future envelope") {
    const auto result = database.store->append_events(
        database.session, std::array{local(admission(), 1, 2)});
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          storage::SessionStoreErrorCode::unsupported_version);
  }
  SECTION("opaque payload cannot spoof known envelope") {
    const auto opaque = event(domain::UnknownEvent{
        "run.local_context_admitted",
        domain::StructuredDataBlock{"application/json", "{}"}});
    REQUIRE_FALSE(
        database.store->append_events(database.session, std::array{opaque}));
  }
}

TEST_CASE("local admission v1 round trips exact source references and "
          "decisions on reopen") {
  Database database;
  auto value = admission();
  SECTION("admitted") {
  }
  SECTION("budget omission") {
    value.evidence.front().decision =
        domain::LocalContextDecision::omitted_budget;
  }
  SECTION("class omission") {
    value.evidence.front().decision =
        domain::LocalContextDecision::omitted_class_budget;
  }
  SECTION("explicit empty selection") {
    value.evidence.clear();
  }
  REQUIRE(domain::seal_local_context_admission(value));
  const auto expected = std::array{
      local(value),
      event(domain::InferenceStarted{id<domain::InferenceId>("inference"),
                                     id<domain::ModelId>("model")},
            2)};
  REQUIRE(database.store->append_events(database.session, expected));
  database.reopen();
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  CHECK(*replay ==
        std::vector<domain::RunEvent>{expected.begin(), expected.end()});
  const auto& restored =
      std::get<domain::LocalContextAdmitted>(replay->front().payload);
  CHECK(restored.admission.admission_digest == value.admission_digest);
  CHECK(restored.admission == value);
}

TEST_CASE("local-required start schema rejects absent false coerced and extra "
          "markers") {
  Database database;
  auto value = started(2);
  value.local_context_admission_required = true;
  REQUIRE(database.store->append_events(database.session,
                                        std::array{event(value, 1, 5)}));
  std::string expression;
  SECTION("missing marker") {
    expression =
        "json_remove(payload_json,'$.local_context_admission_required')";
  }
  SECTION("false marker") {
    expression = "json_set(payload_json,'$.local_context_admission_required',"
                 "json('false'))";
  }
  SECTION("integer marker") {
    expression =
        "json_set(payload_json,'$.local_context_admission_required',1)";
  }
  SECTION("string marker") {
    expression =
        "json_set(payload_json,'$.local_context_admission_required','true')";
  }
  SECTION("extra field") {
    expression = "json_set(payload_json,'$.extra',1)";
  }
  SECTION("control purpose") {
    expression = "json_set(payload_json,'$.purpose','control')";
  }
  SECTION("summary purpose") {
    expression = "json_set(payload_json,'$.purpose','summary')";
  }
  reject_replay(database, expression);
}

TEST_CASE("legacy start schemas preserve false marker and reject silently "
          "losing required proof") {
  Database database;
  unsigned schema = 1;
  auto value = started();
  SECTION("schema1") {
  }
  SECTION("schema2") {
    schema = 2;
    value.memory_selection = domain::MemorySelection{};
    REQUIRE(domain::seal_memory_selection(*value.memory_selection));
  }
  SECTION("schema3") {
    schema = 3;
    value = started(1);
  }
  SECTION("schema4") {
    schema = 4;
    value = started(2);
  }
  const auto original = event(value, 1, schema);
  REQUIRE(
      database.store->append_events(database.session, std::array{original}));
  database.reopen();
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  REQUIRE(replay->size() == 1);
  CHECK(replay->front() == original);
  CHECK_FALSE(std::get<domain::RunStarted>(replay->front().payload)
                  .local_context_admission_required);
  value.local_context_admission_required = true;
  REQUIRE_FALSE(database.store->append_events(
      database.session, std::array{event(value, 2, schema)}));
  CHECK(database.store->replay_events(database.session).value().size() == 1);
}

TEST_CASE(
    "local-required start round trips marker and preserved conversation seal") {
  Database database;
  std::optional<std::uint32_t> version = 2;
  SECTION("without conversation selection") {
    version.reset();
  }
  SECTION("legacy v1 conversation selection") {
    version = 1;
  }
  SECTION("v2 conversation selection") {
  }
  auto value = started(version);
  value.local_context_admission_required = true;
  const auto original = event(value, 1, 5);
  REQUIRE(database.store->append_events(
      database.session, std::array{original, local(admission(), 2)}));
  database.reopen();
  const auto replay = database.store->replay_events(database.session);
  REQUIRE(replay);
  REQUIRE(replay->size() == 2);
  CHECK(replay->front() == original);
  const auto& restored = std::get<domain::RunStarted>(replay->front().payload);
  CHECK(restored.local_context_admission_required);
  CHECK(restored.conversation_admission == value.conversation_admission);
  if (version) {
    REQUIRE(restored.conversation_admission);
    CHECK(restored.conversation_admission->admission_digest ==
          value.conversation_admission->admission_digest);
  }
  value.local_context_admission_required = false;
  REQUIRE_FALSE(database.store->append_events(database.session,
                                              std::array{event(value, 3, 5)}));
  database.mutate("UPDATE events SET schema_version=99 WHERE sequence=1");
  const auto future = database.store->replay_events(database.session);
  REQUIRE(future);
  CHECK(std::holds_alternative<domain::UnknownEvent>(future->front().payload));
}
