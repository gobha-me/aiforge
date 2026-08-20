#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sqlite3.h>
#include <stop_token>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/testing/scripted_session_store.hpp>

namespace {

using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-storage-XXXXXX")
                       .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }
  [[nodiscard]] auto path() const -> const std::filesystem::path& { return m_path; }

 private:
  std::filesystem::path m_path;
};

auto metadata(const std::uint64_t sequence, std::string event_id,
              const std::uint32_t schema_version = 1) -> domain::EventMetadata {
  return {make_id<domain::EventId>(std::move(event_id)),
          make_id<domain::RunId>("run"), sequence, schema_version,
          domain::EventTimestamp{std::chrono::milliseconds{1000 + sequence}},
          std::nullopt, std::nullopt, std::nullopt};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload, std::string id = {})
    -> domain::RunEvent {
  if (id.empty()) id = "event-" + std::to_string(sequence);
  return {metadata(sequence, std::move(id)), std::move(payload)};
}

auto started() -> domain::RunStarted {
  return {make_id<domain::SurfaceId>("surface"),
          make_id<domain::WorkspaceId>("chat"),
          make_id<domain::PermissionProfileId>("observe"), std::nullopt};
}

auto verification_payload(const domain::InvocationId& invocation,
                          const domain::ArtifactId& artifact)
    -> domain::VerificationEvidenceRecorded {
  return {domain::VerificationEvidence{
      make_id<domain::VerificationEvidenceId>("verification"),
      domain::VerificationKind::test,
      std::nullopt,
      domain::VerificationOutcome::passed,
      {make_id<domain::RepositoryId>("repository"),
       {"sha256", "aaaaaaaaaaaaaaaa", 0}},
      std::nullopt,
      domain::ContentDigest{"sha256", "bbbbbbbbbbbbbbbb", 12},
      {"ctest", "3.28", "read", invocation},
      std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{1200}},
      "tests passed",
      {{domain::VerificationOutputStream::standard_output, "passed", 6,
        false, std::nullopt}},
      {{domain::VerificationDiagnosticSeverity::warning, "W1", "warning",
        std::nullopt}},
      {artifact}}};
}

auto open_store(const std::filesystem::path& path,
                storage::SessionStoreLimits limits = {})
    -> std::unique_ptr<adapters::SqliteSessionStore> {
  auto store = adapters::SqliteSessionStore::open(path, limits);
  REQUIRE(store);
  return std::move(*store);
}

auto create(storage::SessionStore& store, const std::string& id,
            const std::int64_t milliseconds)
    -> domain::SessionId {
  auto session = make_id<domain::SessionId>(id);
  REQUIRE(store.create_session(
      {session, domain::EventTimestamp{std::chrono::milliseconds{milliseconds}}}));
  return session;
}

auto all_payloads() -> std::vector<domain::RunEventPayload> {
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto invocation = make_id<domain::InvocationId>("invocation");
  const auto parent_invocation =
      make_id<domain::InvocationId>("parent-invocation");
  const auto question = make_id<domain::QuestionId>("question");
  const auto artifact = make_id<domain::ArtifactId>("artifact");
  const auto view = make_id<domain::ViewId>("view");
  const auto message = make_id<domain::MessageId>("message");
  const domain::DomainError error{domain::ErrorCode::backend, "redacted", true};
  const domain::CapabilityScope scope{domain::Effect::read, "root", "/workspace"};
  const domain::QuestionDefinition definition{
      question, "Choose", domain::QuestionSelection::one,
      {{"yes", "Yes", std::string{"recommended"}}}, true, 1, 1,
      domain::QuestionOtherInput{"Other", std::nullopt, 4096}};
  const domain::ArtifactMetadata artifact_metadata{
      artifact, "text/plain", 3, "sha256:abc", invocation, 1, 2};
  return {
      started(),
      domain::RunAwaitingInput{question},
      domain::RunResumed{question},
      domain::RunCompletionRequested{},
      domain::RunCompleted{},
      domain::RunFailed{error},
      domain::RunCancelRequested{std::string{"requested"}},
      domain::RunCancelled{std::string{"cancelled"}},
      domain::UserContentAdded{domain::Message{
          message, domain::Role::user,
          {domain::TextBlock{"text"},
           domain::StructuredDataBlock{"application/example", "data"},
           domain::CitationBlock{"https://example.test", std::string{"title"}},
           domain::ArtifactReferenceBlock{artifact, std::string{"label"}},
           domain::UnknownContentBlock{"future.content"}},
          std::nullopt}},
      domain::AssistantContentStarted{message, inference},
      domain::AssistantContentDeltaAdded{message, inference,
                                         domain::TextBlock{"delta"}},
      domain::AssistantContentFinished{message, inference},
      domain::InferenceStarted{inference, make_id<domain::ModelId>("model")},
      domain::ReasoningMetadataAdded{
          inference, std::string{"summary"}, {{"visibility", "summary"}}},
      domain::UsageRecorded{inference, domain::Usage{1, 2, 3, 4}},
      domain::InferenceFinished{inference, domain::FinishReason::tool_call},
      domain::InferenceFailed{inference, error},
      domain::InferenceCancelled{inference, std::string{"cancelled"}},
      domain::ToolProposed{invocation, "read",
                           {"application/json", "{}"},
                           {domain::Effect::read, domain::Effect::network},
                           parent_invocation, true, {scope}, {scope}, message},
      domain::ToolPolicyDecided{invocation, domain::PolicyDecision::allow,
                                {scope}, std::string{"allowed"},
                                domain::PolicyDecisionSource::saved_grant},
      domain::ToolApprovalRequested{invocation, {scope},
                                    std::string{"runtime-owned reason"}},
      domain::ToolApprovalDecided{invocation,
                                  domain::ApprovalDecision::approved, {scope},
                                  domain::ApprovalGrantLifetime::saved},
      domain::ToolPolicyFailed{invocation, error},
      domain::ToolStarted{invocation},
      domain::ToolProgressed{invocation, {domain::TextBlock{"working"}}},
      domain::ToolResultRecorded{invocation, {domain::TextBlock{"done"}},
                                 message},
      domain::ToolErrored{invocation, error, message},
      domain::QuestionRequested{definition},
      domain::QuestionAnswered{
          domain::QuestionAnswer{question, {"yes"}, std::string{"other"}}},
      domain::QuestionCancelled{question, std::string{"cancelled"}},
      domain::ArtifactCreated{artifact_metadata},
      domain::ArtifactReferenced{artifact, message},
      domain::ArtifactDisplayed{artifact, view, "right-pane"},
      domain::ArtifactRemovedFromView{artifact, view},
      verification_payload(invocation, artifact),
      domain::ChildRunCreated{make_id<domain::RunId>("child")},
      domain::InterRunMessageSent{make_id<domain::RunId>("target"),
                                  {domain::TextBlock{"message"}}},
      domain::UnknownEvent{"future.event",
                           {"application/json", "{\"nested\":{\"value\":1}}"}},
  };
}

auto execute_sql(const std::filesystem::path& path, const std::string& sql)
    -> void {
  sqlite3* database{};
  REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  char* message{};
  const auto result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &message);
  if (message != nullptr) sqlite3_free(message);
  REQUIRE(result == SQLITE_OK);
  REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

}  // namespace

TEST_CASE("session-store path resolution follows XDG state semantics",
          "[storage][path][failure]") {
  auto path = adapters::resolve_session_store_path(
      {std::filesystem::path{"/tmp/state"},
       std::filesystem::path{"/home/user"}});
  REQUIRE(path == "/tmp/state/aiforge/sessions.sqlite3");

  path = adapters::resolve_session_store_path(
      {std::filesystem::path{"relative"},
       std::filesystem::path{"/home/user"}});
  REQUIRE(path == "/home/user/.local/state/aiforge/sessions.sqlite3");

  path = adapters::resolve_session_store_path({std::nullopt, std::nullopt});
  REQUIRE_FALSE(path);
  REQUIRE(path.error().code == storage::SessionStoreErrorCode::invalid_argument);
}

TEST_CASE("SQLite store creates restrictive state and rejects unsafe paths",
          "[storage][sqlite][path][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "state" / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  struct stat directory_info {};
  struct stat file_info {};
  REQUIRE(::stat(path.parent_path().c_str(), &directory_info) == 0);
  REQUIRE(::stat(path.c_str(), &file_info) == 0);
  REQUIRE((directory_info.st_mode & 0777) == 0700);
  REQUIRE((file_info.st_mode & 0777) == 0600);
  store.reset();

  REQUIRE(::chmod(path.c_str(), 0644) == 0);
  auto insecure = adapters::SqliteSessionStore::open(path);
  REQUIRE_FALSE(insecure);
  REQUIRE(insecure.error().code ==
          storage::SessionStoreErrorCode::permission_denied);

  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  const auto target = temporary.path() / "target.sqlite3";
  std::ofstream{target} << "target";
  const auto linked = temporary.path() / "state" / "aiforge" / "linked.sqlite3";
  std::filesystem::create_symlink(target, linked);
  auto symlink = adapters::SqliteSessionStore::open(linked);
  REQUIRE_FALSE(symlink);
  REQUIRE(symlink.error().code ==
          storage::SessionStoreErrorCode::permission_denied);
}

TEST_CASE("all typed payloads and opaque future payloads round trip",
          "[storage][sqlite][codec]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  auto payloads = all_payloads();
  std::vector<domain::RunEvent> events;
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    events.push_back(event(index + 1, std::move(payloads[index])));
  }
  REQUIRE(store->append_events(session, events));
  const auto replayed = store->replay_events(session);
  const auto replay_error = replayed ? std::string{} : replayed.error().message;
  INFO(replay_error);
  REQUIRE(replayed);
  REQUIRE(*replayed == events);

  const auto info = store->open_session(session);
  REQUIRE(info);
  REQUIRE(info->last_sequence == events.size());
  REQUIRE(info->last_activity_at == events.back().metadata.timestamp);
  const auto listed = store->list_sessions(10);
  REQUIRE(listed);
  REQUIRE(*listed == std::vector<storage::SessionInfo>{*info});

  const auto future_session = create(*store, "future-session", 200);
  auto future = event(
      1,
      domain::UnknownEvent{"run.started",
                           {"application/json", "{\"future_field\":true}"}},
      "future-event");
  future.metadata.schema_version = 2;
  REQUIRE(store->append_events(future_session, std::array{future}));
  const auto future_replay = store->replay_events(future_session);
  REQUIRE(future_replay);
  REQUIRE(*future_replay == std::vector<domain::RunEvent>{future});

  auto noncanonical = event(
      2, domain::UnknownEvent{"future.noncanonical",
                              {"application/json", "{ \"value\": 1 }"}},
      "noncanonical");
  const auto rejected =
      store->append_events(future_session, std::array{noncanonical});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);
}

TEST_CASE("append validation and constraints leave prior history readable",
          "[storage][sqlite][transaction][failure]") {
  TemporaryDirectory temporary;
  storage::SessionStoreLimits limits;
  limits.maximum_payload_bytes = 512;
  auto store = open_store(temporary.path() / "aiforge" / "sessions.sqlite3",
                          limits);
  const auto session = create(*store, "session", 100);
  const auto first = event(1, started(), "one");
  REQUIRE(store->append_events(session, std::array{first}));

  const std::array duplicate_batch{
      event(2, domain::RunCompletionRequested{}, "two"),
      event(3, domain::RunCompleted{}, "one")};
  auto appended = store->append_events(session, duplicate_batch);
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code == storage::SessionStoreErrorCode::conflict);
  const auto after_duplicate = store->replay_events(session);
  const auto duplicate_replay_error =
      after_duplicate ? std::string{} : after_duplicate.error().message;
  INFO(duplicate_replay_error);
  REQUIRE(after_duplicate);
  REQUIRE(after_duplicate->size() == 1);

  const std::array regressing{
      event(4, domain::RunCompletionRequested{}, "four"),
      event(3, domain::RunCompleted{}, "three")};
  appended = store->append_events(session, regressing);
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  const auto oversized = event(
      2, domain::UserContentAdded{domain::Message{
             make_id<domain::MessageId>("large"), domain::Role::user,
             {domain::TextBlock{std::string(1024, 'x')}}, std::nullopt}},
      "large-event");
  appended = store->append_events(session, std::array{oversized});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::resource_exhausted);

  auto invalid_verification = verification_payload(
      make_id<domain::InvocationId>("verification-invocation"),
      make_id<domain::ArtifactId>("verification-artifact"));
  invalid_verification.evidence.summary.clear();
  appended = store->append_events(
      session, std::array{event(2, std::move(invalid_verification),
                                "invalid-verification")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  std::stop_source cancellation;
  cancellation.request_stop();
  appended = store->append_events(
      session,
      std::array{event(2, domain::RunCompletionRequested{}, "cancelled")},
      cancellation.get_token());
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code == storage::SessionStoreErrorCode::cancelled);
  REQUIRE(store->replay_events(session)->size() == 1);
}

TEST_CASE("missing sessions malformed JSON and newer stores fail explicitly",
          "[storage][sqlite][corrupt][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto missing = make_id<domain::SessionId>("missing");
  REQUIRE_FALSE(store->open_session(missing));
  REQUIRE(store->open_session(missing).error().code ==
          storage::SessionStoreErrorCode::not_found);
  REQUIRE_FALSE(store->replay_events(missing));

  const auto session = create(*store, "session", 100);
  REQUIRE(store->append_events(session,
                               std::array{event(1, started(), "one")}));
  store.reset();
  execute_sql(path,
              "UPDATE events SET payload_json='{\"x\":1,\"x\":2}' "
              "WHERE event_id='one'");
  store = open_store(path);
  const auto corrupt = store->replay_events(session);
  REQUIRE_FALSE(corrupt);
  REQUIRE(corrupt.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();

  execute_sql(path, "PRAGMA user_version=999");
  const auto newer = adapters::SqliteSessionStore::open(path);
  REQUIRE_FALSE(newer);
  REQUIRE(newer.error().code ==
          storage::SessionStoreErrorCode::unsupported_version);
}

TEST_CASE("bounded SQLite writer contention is retryable",
          "[storage][sqlite][concurrency][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  storage::SessionStoreLimits limits;
  limits.busy_timeout = std::chrono::milliseconds{10};
  auto store = open_store(path, limits);
  const auto session = create(*store, "session", 100);

  sqlite3* competing{};
  REQUIRE(sqlite3_open(path.c_str(), &competing) == SQLITE_OK);
  REQUIRE(sqlite3_exec(competing, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
  const auto result = store->append_events(
      session, std::array{event(1, started(), "one")});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == storage::SessionStoreErrorCode::contention);
  REQUIRE(result.error().retryable);
  REQUIRE(sqlite3_exec(competing, "ROLLBACK", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
  REQUIRE(sqlite3_close(competing) == SQLITE_OK);
  REQUIRE(store->replay_events(session)->empty());
}

TEST_CASE("closing an interrupted SQLite transaction preserves committed history",
          "[storage][sqlite][crash][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  const auto first = event(1, started(), "one");
  REQUIRE(store->append_events(session, std::array{first}));
  store.reset();

  execute_sql(
      path,
      "BEGIN IMMEDIATE;"
      "INSERT INTO events(session_id,sequence,event_id,run_id,schema_version,"
      "timestamp_ms,payload_type,payload_json) VALUES("
      "'session',2,'interrupted','run',1,2000,'run.completed','{}')");

  store = open_store(path);
  const auto replay = store->replay_events(session);
  REQUIRE(replay);
  REQUIRE(*replay == std::vector<domain::RunEvent>{first});
}

TEST_CASE("scripted session store records exact calls and deterministic failures",
          "[storage][fake][failure]") {
  const auto session = make_id<domain::SessionId>("session");
  const storage::SessionInfo info{
      session, domain::EventTimestamp{std::chrono::milliseconds{1}},
      domain::EventTimestamp{std::chrono::milliseconds{2}}, 3};
  testing::ScriptedSessionStore fake{{
      {testing::OpenSessionCall{session}, info},
      {testing::ReplayEventsCall{session},
       storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                  "injected failure", true}},
  }};
  REQUIRE(fake.open_session(session) == info);
  const auto replay = fake.replay_events(session);
  REQUIRE_FALSE(replay);
  REQUIRE(replay.error().code == storage::SessionStoreErrorCode::io_failure);
  REQUIRE(fake.recorded_calls().size() == 2);
  REQUIRE(fake.remaining_exchanges() == 0);
}
