#include <sqlite3.h>
#include <unistd.h>

#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/domain/conversation_summary.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
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
                           "aiforge-summary-codec-XXXXXX")
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
auto event(domain::RunEventPayload payload, std::uint32_t schema = 1,
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
using namespace domain;
auto sources(std::string prefix = "source", std::uint64_t first = 2)
    -> ConversationSummarySources {
  Message message{
      id<MessageId>(prefix + "-message"), Role::user, {TextBlock{"facts"}}, {}};
  const auto digest = normalized_conversation_message_digest(message).value();
  ConversationAdmittedEntry entry{
      id<EventId>(prefix + "-event"),
      first,
      id<ContextEntryId>(prefix + "-entry"),
      message.message_id,
      {id<ContextSourceId>(prefix + "-provenance"), {}, {}},
      1,
      5,
      ContextContentKind::conversation,
      digest};
  ConversationSummarySources result{1,
                                    1,
                                    id<SessionId>("session"),
                                    10,
                                    {{id<RunId>(prefix + "-run"),
                                      id<EventId>(prefix + "-terminal"),
                                      first + 2,
                                      {entry}}},
                                    {}};
  REQUIRE(seal_conversation_summary_sources(result));
  return result;
}
auto intent(ConversationSummarySources source = sources(),
            std::string name = "summary") -> ConversationSummaryIntent {
  ConversationSummaryIntent result{1,
                                   1,
                                   id<ConversationSummaryId>(name),
                                   std::move(source),
                                   id<RunId>(name + "-producer"),
                                   id<InferenceId>(name + "-inference"),
                                   id<ModelId>("model"),
                                   id<MessageId>(name + "-output"),
                                   "runtime-v1",
                                   {128, 16, 0},
                                   32,
                                   1024,
                                   {}};
  REQUIRE(seal_conversation_summary_intent(result));
  return result;
}
auto candidate(const ConversationSummaryIntent& source,
               std::uint64_t sequence = 11) -> ConversationSummaryCandidate {
  const auto prefix = std::string{source.summary_id.value()};
  ConversationSummaryCandidate result{1,
                                      source.sources.session_id,
                                      source.summary_id,
                                      1,
                                      *source.sources.source_digest,
                                      *source.intent_digest,
                                      id<EventId>(prefix + "-output-event"),
                                      sequence,
                                      id<EventId>(prefix + "-candidate-event"),
                                      sequence + 1,
                                      ConversationSummaryAuthor::model,
                                      {},
                                      "Important facts",
                                      {}};
  REQUIRE(seal_conversation_summary_candidate(result, source));
  return result;
}
auto activation(const ConversationSummaryIntent& source,
                const ConversationSummaryCandidate& text,
                std::uint64_t sequence = 13) -> ConversationSummaryActivation {
  std::vector<RunId> runs;
  for (const auto& group : source.sources.groups)
    runs.push_back(group.run_id);
  ConversationSummaryActivation result{
      1,
      source.sources.session_id,
      {text.summary_id, text.revision, *text.candidate_digest},
      *source.sources.source_digest,
      std::move(runs),
      source.sources.groups.front().entries.front().event_sequence,
      id<EventId>(std::string{text.summary_id.value()} + "-activation"),
      sequence,
      {}};
  REQUIRE(seal_conversation_summary_activation(result, text, source));
  return result;
}
auto rejected_replay(Database& database, const std::string& expression)
    -> void {
  database.mutate("UPDATE events SET payload_json = " + expression);
  const auto replay = database.store->replay_events(database.session);
  REQUIRE_FALSE(replay);
  CHECK(replay.error().code == storage::SessionStoreErrorCode::corrupt);
}
auto published(Database& database) -> domain::ConversationSummaryCandidate {
  const auto value = candidate(intent());
  REQUIRE(database.store->append_events(
      database.session,
      std::array{event(domain::ConversationSummaryCandidatePublished{value})}));
  return value;
}
} // namespace

TEST_CASE(
    "summary intent codec rejects malformed fields numeric coercion and seals",
    "[storage][summary][failure]") {
  Database database;
  REQUIRE(database.store->append_events(
      database.session,
      std::array{event(
          domain::ConversationSummaryGenerationIntentRecorded{intent()})}));
  std::string expression;
  SECTION("extra envelope") {
    expression = "json_set(payload_json,'$.extra',1)";
  }
  SECTION("missing field") {
    expression = "json_remove(payload_json,'$.intent.runtime_version')";
  }
  SECTION("extra intent") {
    expression = "json_set(payload_json,'$.intent.extra',1)";
  }
  SECTION("extra source") {
    expression = "json_set(payload_json,'$.intent.sources.extra',1)";
  }
  SECTION("extra source entry") {
    expression = "json_set(payload_json,'$.intent.sources.groups[0].entries[0]."
                 "extra',1)";
  }
  SECTION("extra provenance") {
    expression = "json_set(payload_json,'$.intent.sources.groups[0].entries[0]."
                 "provenance.extra',1)";
  }
  SECTION("extra digest") {
    expression = "json_set(payload_json,'$.intent.intent_digest.extra',1)";
  }
  SECTION("extra capacity") {
    expression = "json_set(payload_json,'$.intent.capacity.extra',1)";
  }
  SECTION("real estimate") {
    expression =
        "json_set(payload_json,'$.intent.estimated_input_tokens',32.0)";
  }
  SECTION("string estimate") {
    expression =
        "json_set(payload_json,'$.intent.estimated_input_tokens','32')";
  }
  SECTION("boolean estimate") {
    expression =
        "json_set(payload_json,'$.intent.estimated_input_tokens',json('true'))";
  }
  SECTION("negative sequence") {
    expression =
        "json_set(payload_json,'$.intent.sources.snapshot_sequence',-1)";
  }
  SECTION("overflowing version") {
    expression = "json_set(payload_json,'$.intent.version',4294967297)";
  }
  SECTION("unknown version") {
    expression = "json_set(payload_json,'$.intent.version',2)";
  }
  SECTION("unknown format") {
    expression = "json_set(payload_json,'$.intent.format_version',2)";
  }
  SECTION("unknown estimator") {
    expression =
        "json_set(payload_json,'$.intent.sources.estimator_version',2)";
  }
  SECTION("unknown source kind") {
    expression = "json_set(payload_json,'$.intent.sources.groups[0].entries[0]."
                 "kind','evidence')";
  }
  SECTION("source seal corruption") {
    expression =
        "json_set(payload_json,'$.intent.sources.snapshot_sequence',9)";
  }
  SECTION("intent seal corruption") {
    expression = "json_set(payload_json,'$.intent.model_id','other')";
  }
  SECTION("null seal") {
    expression = "json_set(payload_json,'$.intent.intent_digest',NULL)";
  }
  SECTION("output overflow") {
    expression = "json_set(payload_json,'$.intent.maximum_output_bytes',"
                 "18446744073709551615)";
  }
  SECTION("wrong groups type") {
    expression = "json_set(payload_json,'$.intent.sources.groups',json('{}'))";
  }
  rejected_replay(database, expression);
}

TEST_CASE("summary intent codec bounds arrays before source allocation",
          "[storage][summary][failure]") {
  Database database;
  REQUIRE(database.store->append_events(
      database.session,
      std::array{event(
          domain::ConversationSummaryGenerationIntentRecorded{intent()})}));
  std::string expression;
  SECTION("groups") {
    expression =
        "json_set(payload_json,'$.intent.sources.groups',(WITH RECURSIVE n(x) "
        "AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<129) SELECT "
        "json_group_array(json(json_extract(events.payload_json,'$.intent."
        "sources.groups[0]')||substr(n.x,1,0))) FROM n))";
  }
  SECTION("entries") {
    expression =
        "json_set(payload_json,'$.intent.sources.groups[0].entries',(WITH "
        "RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE "
        "x<4097) SELECT "
        "json_group_array(json(json_extract(events.payload_json,'$.intent."
        "sources.groups[0].entries[0]')||substr(n.x,1,0))) FROM n))";
  }
  SECTION("aggregate entries across groups") {
    expression =
        "json_set(payload_json,'$.intent.sources.groups',(WITH RECURSIVE n(x) "
        "AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<128) SELECT "
        "json_group_array(json(json_set(json_extract(events.payload_json,'$."
        "intent.sources.groups[0]'),'$.entries',(WITH RECURSIVE m(x) AS "
        "(VALUES(1) UNION ALL SELECT x+1 FROM m WHERE x<33) SELECT "
        "json_group_array(json(json_extract(events.payload_json,'$.intent."
        "sources.groups[0].entries[0]')||substr(m.x,1,0))) FROM "
        "m))||substr(n.x,1,0))) FROM n))";
  }
  SECTION("oversized provenance") {
    expression =
        "json_set(payload_json,'$.intent.sources.groups[0].entries[0]."
        "provenance.source_location',replace(hex(zeroblob(2049)),'0','x'))";
  }
  rejected_replay(database, expression);
}

TEST_CASE(
    "summary candidate codec rejects malformed text author and edit metadata",
    "[storage][summary][failure]") {
  Database database;
  static_cast<void>(published(database));
  std::string expression;
  SECTION("extra candidate") {
    expression = "json_set(payload_json,'$.candidate.extra',1)";
  }
  SECTION("unknown author") {
    expression = "json_set(payload_json,'$.candidate.author','agent')";
  }
  SECTION("unknown version") {
    expression = "json_set(payload_json,'$.candidate.version',2)";
  }
  SECTION("zero revision") {
    expression = "json_set(payload_json,'$.candidate.revision',0)";
  }
  SECTION("real revision") {
    expression = "json_set(payload_json,'$.candidate.revision',1.0)";
  }
  SECTION("empty text") {
    expression = "json_set(payload_json,'$.candidate.text','')";
  }
  SECTION("text is array") {
    expression = "json_set(payload_json,'$.candidate.text',json('[]'))";
  }
  SECTION("unsafe control") {
    expression = "json_set(payload_json,'$.candidate.text',char(27)||'unsafe')";
  }
  SECTION("malformed UTF8") {
    expression =
        "json_set(payload_json,'$.candidate.text',cast(x'ff' as text))";
  }
  SECTION("oversized text") {
    expression = "json_set(payload_json,'$.candidate.text',replace(hex("
                 "zeroblob(32769)),'0','x'))";
  }
  SECTION("missing edit parent") {
    expression = "json_set(payload_json,'$.candidate.author','user_edit','$."
                 "candidate.revision',2)";
  }
  SECTION("model cannot skip revision") {
    expression = "json_set(payload_json,'$.candidate.revision',2)";
  }
  SECTION("unordered creation") {
    expression = "json_set(payload_json,'$.candidate.created_sequence',11)";
  }
  SECTION("same output identity") {
    expression = "json_set(payload_json,'$.candidate.created_event_id',json_"
                 "extract(payload_json,'$.candidate.output_event_id'))";
  }
  SECTION("missing seal") {
    expression = "json_set(payload_json,'$.candidate.candidate_digest',NULL)";
  }
  SECTION("malformed seal") {
    expression =
        "json_set(payload_json,'$.candidate.candidate_digest.algorithm','md5')";
  }
  SECTION("extra digest") {
    expression = "json_set(payload_json,'$.candidate.source_digest.extra',1)";
  }
  rejected_replay(database, expression);
}

TEST_CASE("summary activation codec rejects corrupt coverage and replacement "
          "references",
          "[storage][summary][failure]") {
  Database database;
  const auto source = intent();
  const auto text = candidate(source);
  const auto active = activation(source, text);
  REQUIRE(database.store->append_events(
      database.session, std::array{event(domain::ConversationSummaryActivated{
                            0, active, {active.candidate}})}));
  std::string expression;
  SECTION("extra envelope") {
    expression = "json_set(payload_json,'$.extra',1)";
  }
  SECTION("real policy revision") {
    expression = "json_set(payload_json,'$.previous_policy_revision',0.0)";
  }
  SECTION("policy overflow") {
    expression = "json_set(payload_json,'$.previous_policy_revision',"
                 "18446744073709551615)";
  }
  SECTION("extra activation") {
    expression = "json_set(payload_json,'$.activation.extra',1)";
  }
  SECTION("unknown version") {
    expression = "json_set(payload_json,'$.activation.version',2)";
  }
  SECTION("changed source") {
    expression =
        "json_set(payload_json,'$.activation.covered_run_ids[0]','other')";
  }
  SECTION("changed sequence") {
    expression = "json_set(payload_json,'$.activation.activation_sequence',14)";
  }
  SECTION("changed seal") {
    expression = "json_set(payload_json,'$.activation.activation_digest.value',"
                 "replace(hex(zeroblob(32)),'0','a'))";
  }
  SECTION("extra reference") {
    expression = "json_set(payload_json,'$.activation.candidate.extra',1)";
  }
  SECTION("empty coverage") {
    expression =
        "json_set(payload_json,'$.activation.covered_run_ids',json('[]'))";
  }
  SECTION("oversized coverage") {
    expression = "json_set(payload_json,'$.activation.covered_run_ids',(WITH "
                 "RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n "
                 "WHERE x<129) SELECT json_group_array('run') FROM n))";
  }
  SECTION("oversized replacements") {
    expression =
        "json_set(payload_json,'$.replaced_versions',(WITH RECURSIVE n(x) AS "
        "(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<33) SELECT "
        "json_group_array(json(json_extract(events.payload_json,'$.activation."
        "candidate')||substr(n.x,1,0))) FROM n))";
  }
  SECTION("duplicate replacements") {
    expression = "json_insert(payload_json,'$.replaced_versions[#]',json_"
                 "extract(payload_json,'$.activation.candidate'))";
  }
  SECTION("zero reference revision") {
    expression = "json_set(payload_json,'$.replaced_versions[0].revision',0)";
  }
  SECTION("extra replacement field") {
    expression = "json_set(payload_json,'$.replaced_versions[0].extra',1)";
  }
  rejected_replay(database, expression);
}

TEST_CASE("summary disable codec rejects malformed exact references",
          "[storage][summary][failure]") {
  Database database;
  const auto source = intent();
  const auto text = candidate(source);
  const auto active = activation(source, text);
  REQUIRE(database.store->append_events(
      database.session,
      std::array{event(domain::ConversationSummaryDisabled{
          1, active.candidate, active.activation_event_id})}));
  std::string expression;
  SECTION("extra envelope") {
    expression = "json_set(payload_json,'$.extra',1)";
  }
  SECTION("negative policy revision") {
    expression = "json_set(payload_json,'$.previous_policy_revision',-1)";
  }
  SECTION("string policy revision") {
    expression = "json_set(payload_json,'$.previous_policy_revision','1')";
  }
  SECTION("zero candidate revision") {
    expression = "json_set(payload_json,'$.candidate.revision',0)";
  }
  SECTION("missing activation") {
    expression = "json_remove(payload_json,'$.activation_event_id')";
  }
  SECTION("empty activation") {
    expression = "json_set(payload_json,'$.activation_event_id','')";
  }
  SECTION("extra candidate field") {
    expression = "json_set(payload_json,'$.candidate.extra',1)";
  }
  rejected_replay(database, expression);
}

TEST_CASE("summary codec retains unknown event versions and rejects typed "
          "future writes",
          "[storage][summary][versions]") {
  Database database;
  const auto source = intent();
  const auto text = candidate(source);
  const auto active = activation(source, text);
  const std::array<domain::RunEventPayload, 4> payloads{
      domain::ConversationSummaryGenerationIntentRecorded{source},
      domain::ConversationSummaryCandidatePublished{text},
      domain::ConversationSummaryActivated{0, active, {}},
      domain::ConversationSummaryDisabled{1, active.candidate,
                                          active.activation_event_id}};
  const std::array<std::string, 4> names{
      "session.conversation_summary_intent_recorded",
      "session.conversation_summary_candidate_published",
      "session.conversation_summary_activated",
      "session.conversation_summary_disabled"};
  std::vector<domain::RunEvent> expected;
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    const auto unsupported = database.store->append_events(
        database.session, std::array{event(payloads[index], 2, index + 1)});
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code ==
          storage::SessionStoreErrorCode::unsupported_version);
    expected.push_back(
        event(domain::UnknownEvent{names[index],
                                   {"application/json", R"({"future":true})"}},
              2, index + 1));
  }
  REQUIRE(database.store->append_events(database.session, expected));
  database.store.reset();
  database.reopen();
  const auto replay = database.store->replay_events(database.session);
  INFO((replay ? "replay succeeded" : replay.error().message));
  REQUIRE(replay);
  CHECK(*replay == expected);
}

TEST_CASE("summary codec roundtrips generation edits activation and disable",
          "[storage][summary][roundtrip]") {
  Database database;
  auto source = intent();
  source.maximum_output_bytes = domain::summary_maximum_text_bytes;
  REQUIRE(domain::seal_conversation_summary_intent(source));
  const auto original = candidate(source);
  auto edited = original;
  edited.author = domain::ConversationSummaryAuthor::user_edit;
  edited.revision = 2;
  edited.edited_from = domain::ConversationSummaryVersion{
      original.summary_id, original.revision, *original.candidate_digest};
  edited.created_sequence = 14;
  edited.created_event_id = id<domain::EventId>("edit-event");
  edited.text = std::string(8192, 'x') + " 雪";
  REQUIRE(
      domain::seal_conversation_summary_candidate(edited, source, &original));
  auto active = activation(source, original);
  auto updated = active;
  updated.candidate = {edited.summary_id, edited.revision,
                       *edited.candidate_digest};
  updated.activation_event_id = id<domain::EventId>("edit-activation");
  updated.activation_sequence = 15;
  REQUIRE(domain::seal_conversation_summary_activation(updated, edited, source,
                                                       &original));
  std::vector<domain::RunEvent> expected{
      event(domain::ConversationSummaryGenerationIntentRecorded{source}, 1, 1),
      event(domain::ConversationSummaryCandidatePublished{original}, 1, 2),
      event(domain::ConversationSummaryCandidatePublished{edited}, 1, 3),
      event(domain::ConversationSummaryActivated{0, active, {}}, 1, 4),
      event(
          domain::ConversationSummaryActivated{1, updated, {active.candidate}},
          1, 5),
      event(domain::ConversationSummaryDisabled{2, updated.candidate,
                                                updated.activation_event_id},
            1, 6)};
  REQUIRE(database.store->append_events(database.session, expected));
  database.store.reset();
  database.reopen();
  const auto replay = database.store->replay_events(database.session);
  INFO((replay ? "replay succeeded" : replay.error().message));
  REQUIRE(replay);
  CHECK(*replay == expected);
}

TEST_CASE(
    "summary codec rejects invalid typed writes without persisting events",
    "[storage][summary][failure]") {
  Database database;
  auto source = intent();
  auto text = candidate(source);
  auto active = activation(source, text);
  domain::RunEventPayload payload =
      domain::ConversationSummaryCandidatePublished{text};
  SECTION("intent seal drift") {
    source.model_id = id<domain::ModelId>("other");
    payload = domain::ConversationSummaryGenerationIntentRecorded{source};
  }
  SECTION("missing candidate seal") {
    text.candidate_digest.reset();
    payload = domain::ConversationSummaryCandidatePublished{text};
  }
  SECTION("candidate author") {
    text.author = static_cast<domain::ConversationSummaryAuthor>(99);
    payload = domain::ConversationSummaryCandidatePublished{text};
  }
  SECTION("candidate text") {
    text.text.assign(domain::summary_maximum_text_bytes + 1, 'x');
    payload = domain::ConversationSummaryCandidatePublished{text};
  }
  SECTION("activation seal drift") {
    active.covered_run_ids[0] = id<domain::RunId>("other");
    payload = domain::ConversationSummaryActivated{0, active, {}};
  }
  SECTION("duplicate replacement") {
    payload = domain::ConversationSummaryActivated{
        0, active, {active.candidate, active.candidate}};
  }
  SECTION("disable policy overflow") {
    payload = domain::ConversationSummaryDisabled{
        std::numeric_limits<std::uint64_t>::max(), active.candidate,
        active.activation_event_id};
  }
  const auto appended = database.store->append_events(
      database.session, std::array{event(payload)});
  REQUIRE_FALSE(appended);
  CHECK(appended.error().code ==
        storage::SessionStoreErrorCode::invalid_argument);
  const auto replay = database.store->replay_events(database.session);
  INFO((replay ? "replay succeeded" : replay.error().message));
  REQUIRE(replay);
  CHECK(replay->empty());
}

TEST_CASE(
    "summary candidate codec leaves recorded intent resolution to runtime",
    "[storage][summary][boundary]") {
  Database database;
  static_cast<void>(published(database));
  database.mutate("UPDATE events SET "
                  "payload_json=json_set(payload_json,'$.candidate.text','"
                  "changed after sealing')");
  const auto replay = database.store->replay_events(database.session);
  INFO((replay ? "replay succeeded" : replay.error().message));
  REQUIRE(replay);
  REQUIRE(replay->size() == 1);
  const auto& value = std::get<domain::ConversationSummaryCandidatePublished>(
                          replay->front().payload)
                          .candidate;
  REQUIRE_FALSE(
      domain::validate_conversation_summary_candidate(value, intent()));
}
