#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/adapters/process_video.hpp>
#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/event_log.hpp>
#include <aiforge/presentation/video.hpp>
#include <aiforge/testing/scripted_session_store.hpp>
#include <aiforge/video/mp4.hpp>

#include "secure_artifact_export.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using namespace aiforge;

template <typename Id> [[nodiscard]] auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}

[[nodiscard]] auto attributes() -> domain::RunStarted {
  return {id<domain::SurfaceId>("video"), id<domain::WorkspaceId>("workspace"),
          id<domain::PermissionProfileId>("explicit"), std::nullopt};
}

[[nodiscard]] auto artifact(std::string name = "video-artifact")
    -> domain::ArtifactMetadata {
  return {
      id<domain::ArtifactId>(std::move(name)),
      "video/mp4",
      123,
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
}

auto append_u32(std::vector<std::byte>& bytes, const std::uint32_t value)
    -> void {
  for (const auto shift : {24U, 16U, 8U, 0U})
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

auto set_u32(std::vector<std::byte>& bytes, const std::size_t offset,
             const std::uint32_t value) -> void {
  for (std::size_t index{}; index < 4; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>(value >> ((3U - index) * 8U));
  }
}

auto append_text(std::vector<std::byte>& bytes, const std::string_view text)
    -> void {
  for (const unsigned char value : text)
    bytes.push_back(static_cast<std::byte>(value));
}

[[nodiscard]] auto box(const std::string_view type,
                       const std::span<const std::byte> payload = {})
    -> std::vector<std::byte> {
  std::vector<std::byte> result;
  append_u32(result, static_cast<std::uint32_t>(payload.size() + 8));
  append_text(result, type);
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

auto append(std::vector<std::byte>& target,
            const std::span<const std::byte> value) -> void {
  target.insert(target.end(), value.begin(), value.end());
}

[[nodiscard]] auto minimal_mp4() -> std::vector<std::byte> {
  std::vector<std::byte> file_type;
  append_text(file_type, "isom");
  append_u32(file_type, 0);
  auto bytes = box("ftyp", file_type);
  std::vector<std::byte> handler_payload(24);
  for (std::size_t index{}; index < 4; ++index)
    handler_payload[8 + index] = static_cast<std::byte>("vide"[index]);
  append(bytes,
         box("moov", box("trak", box("mdia", box("hdlr", handler_payload)))));
  const std::array media{std::byte{0x42}};
  append(bytes, box("mdat", media));
  return bytes;
}

[[nodiscard]] auto digest_of(const std::span<const std::byte> content)
    -> std::string {
  detail::Sha256 digest;
  digest.update(content);
  return "sha256:" + digest.finish();
}

[[nodiscard]] auto metadata_for(const std::span<const std::byte> content,
                                std::string name = "video-artifact")
    -> domain::ArtifactMetadata {
  auto result = artifact(std::move(name));
  result.byte_size = content.size();
  result.digest = digest_of(content);
  return result;
}

[[nodiscard]] auto event(std::string run, std::uint64_t sequence,
                         domain::RunEventPayload payload) -> domain::RunEvent {
  return {{id<domain::EventId>("event-" + std::to_string(sequence)),
           id<domain::RunId>(std::move(run)), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{1}}, std::nullopt,
           std::nullopt, std::nullopt},
          std::move(payload)};
}

[[nodiscard]] auto published_run(std::string run, std::uint64_t first,
                                 std::string artifact_name = "video-artifact")
    -> std::vector<domain::RunEvent> {
  const auto operation = id<domain::VideoOperationId>("operation-" + run);
  const auto job = id<domain::VideoJobId>("secret-job-" + run);
  const auto metadata = artifact(std::move(artifact_name));
  const auto model = id<domain::ModelId>("video-model");
  auto money = domain::MonetaryAmount::create(
                   "USD", domain::DecimalAmount::from("1.00").value())
                   .value();
  std::vector<domain::RunEvent> events;
  events.push_back(event(run, first, attributes()));
  events.push_back(event(
      run, first + 1,
      domain::VideoGenerationRequested{
          operation,
          domain::VideoGenerationSpec{model, "prompt", std::chrono::seconds{5}},
          metadata.artifact_id}));
  events.push_back(
      event(run, first + 2, domain::VideoQuoteObserved{operation, money}));
  events.push_back(
      event(run, first + 3, domain::VideoJobQueued{operation, job}));
  events.push_back(
      event(run, first + 4,
            domain::VideoJobStatusObserved{operation, job, 1,
                                           domain::VideoJobState::completed}));
  events.push_back(event(run, first + 5, domain::ArtifactCreated{metadata}));
  events.push_back(event(
      run, first + 6,
      domain::VideoArtifactPublished{operation, job, std::move(metadata)}));
  events.push_back(
      event(run, first + 7, domain::VideoCleanupPending{operation, job, 1}));
  return events;
}

[[nodiscard]] auto published_run(domain::ArtifactMetadata metadata)
    -> std::vector<domain::RunEvent> {
  auto events =
      published_run("run", 1, std::string{metadata.artifact_id.value()});
  std::get<domain::ArtifactCreated>(events[5].payload).artifact = metadata;
  std::get<domain::VideoArtifactPublished>(events[6].payload).artifact =
      std::move(metadata);
  return events;
}

[[nodiscard]] auto with_interleaved_unrelated_event(
    std::vector<domain::RunEvent> events) -> std::vector<domain::RunEvent> {
  events.insert(
      events.begin() + 1,
      event("unrelated", 2,
            domain::UnknownEvent{"future.video", {"application/json", "{}"}}));
  for (std::size_t index{}; index < events.size(); ++index) {
    const auto sequence = static_cast<std::uint64_t>(index + 1);
    events[index].metadata.sequence = sequence;
    events[index].metadata.event_id =
        id<domain::EventId>("event-" + std::to_string(sequence));
  }
  return events;
}

class RecordingArtifactStore final : public storage::ArtifactStore {
 public:
  using Outcome =
      std::variant<storage::ArtifactRead, storage::ArtifactStoreError>;

  explicit RecordingArtifactStore(Outcome outcome)
      : m_outcome{std::move(outcome)} {}

  auto put(storage::ArtifactWrite, std::span<const std::byte>, std::stop_token)
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override {
    return std::unexpected(storage::ArtifactStoreError{
        storage::ArtifactStoreErrorCode::internal_failure,
        "unexpected artifact write", false});
  }

  auto get(const domain::ArtifactMetadata& metadata, std::size_t maximum_bytes,
           std::stop_token stop_token)
      -> std::expected<storage::ArtifactRead,
                       storage::ArtifactStoreError> override {
    ++get_calls;
    seen_metadata = metadata;
    seen_maximum_bytes = maximum_bytes;
    if (stop_token.stop_requested()) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::cancelled, "cancelled", false});
    }
    if (const auto* error =
            std::get_if<storage::ArtifactStoreError>(&m_outcome))
      return std::unexpected(*error);
    return std::get<storage::ArtifactRead>(std::move(m_outcome));
  }

  int get_calls{};
  std::optional<domain::ArtifactMetadata> seen_metadata;
  std::size_t seen_maximum_bytes{};

 private:
  Outcome m_outcome;
};

[[nodiscard]] auto scripted_session(std::vector<domain::RunEvent> events)
    -> std::shared_ptr<testing::ScriptedSessionStore> {
  const auto session = id<domain::SessionId>("session");
  return std::make_shared<testing::ScriptedSessionStore>(
      std::vector<testing::SessionStoreExchange>{
          {testing::ReplayEventsCall{session}, std::move(events)}});
}

[[nodiscard]] auto session_factory(
    const std::shared_ptr<testing::ScriptedSessionStore>& sessions, int& calls)
    -> adapters::ProcessVideoCommand::SessionStoreFactory {
  return [sessions,
          &calls]() -> std::expected<std::shared_ptr<storage::SessionStore>,
                                     cli::CommandFailure> {
    ++calls;
    return sessions;
  };
}

[[nodiscard]] auto artifact_factory(
    const std::shared_ptr<RecordingArtifactStore>& artifacts, int& calls)
    -> adapters::ProcessVideoCommand::ArtifactStoreFactory {
  return [artifacts,
          &calls]() -> std::expected<std::shared_ptr<storage::ArtifactStore>,
                                     cli::CommandFailure> {
    ++calls;
    return artifacts;
  };
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    m_path = std::filesystem::temp_directory_path() /
             ("aiforge-video-presentation-" + suffix);
    std::filesystem::create_directories(m_path);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

class ScopedCurrentDirectory final {
 public:
  explicit ScopedCurrentDirectory(const std::filesystem::path& path)
      : m_previous{std::filesystem::current_path()} {
    std::filesystem::current_path(path);
  }
  ~ScopedCurrentDirectory() {
    std::error_code ignored;
    std::filesystem::current_path(m_previous, ignored);
  }

 private:
  std::filesystem::path m_previous;
};

TEST_CASE("video presentation rejects inconsistent durable publication") {
  const auto session = id<domain::SessionId>("session");
  auto events = published_run("run", 1);
  auto mismatched = artifact("different");
  std::get<domain::VideoArtifactPublished>(events[6].payload).artifact =
      mismatched;
  auto rejected = presentation::select_video_presentation(session, events);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        presentation::VideoPresentationErrorCode::invalid_history);

  events = published_run("run", 1);
  auto& created = std::get<domain::ArtifactCreated>(events[5].payload).artifact;
  created.media_type = "image/png";
  std::get<domain::VideoArtifactPublished>(events[6].payload).artifact =
      created;
  rejected = presentation::select_video_presentation(session, events);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().message.find("secret-job") == std::string::npos);

  events = published_run("run", 1);
  domain::SessionEventLog event_log{session};
  for (const auto& item : events)
    REQUIRE(event_log.append(item));
  auto projection =
      domain::VideoProjection::rebuild(event_log, id<domain::RunId>("run"));
  REQUIRE(projection);
  auto wrong_metadata = artifact();
  ++wrong_metadata.byte_size;
  rejected = presentation::make_video_presentation(session, *projection,
                                                   wrong_metadata);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        presentation::VideoPresentationErrorCode::invalid_history);

  rejected = presentation::make_video_presentation(
      id<domain::SessionId>("other-session"), *projection,
      *projection->artifact());
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        presentation::VideoPresentationErrorCode::invalid_history);
}

TEST_CASE("video presentation selection is exact and never guesses") {
  const auto session = id<domain::SessionId>("session");
  auto first = published_run("run-one", 1, "artifact-one");
  auto second = published_run("run-two", 9, "artifact-two");
  first.insert(first.end(), second.begin(), second.end());

  auto ambiguous = presentation::select_video_presentation(session, first);
  REQUIRE_FALSE(ambiguous);
  CHECK(ambiguous.error().code ==
        presentation::VideoPresentationErrorCode::ambiguous);

  auto selected = presentation::select_video_presentation(
      session, first, id<domain::ArtifactId>("artifact-one"));
  REQUIRE(selected);
  CHECK(selected->run_id == id<domain::RunId>("run-one"));

  auto missing = presentation::select_video_presentation(
      session, first, id<domain::ArtifactId>("absent"));
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code ==
        presentation::VideoPresentationErrorCode::not_found);

  first = published_run("run-one", 1, "duplicate-artifact");
  second = published_run("run-two", 9, "duplicate-artifact");
  first.insert(first.end(), second.begin(), second.end());
  ambiguous = presentation::select_video_presentation(
      session, first, id<domain::ArtifactId>("duplicate-artifact"));
  REQUIRE_FALSE(ambiguous);
  CHECK(ambiguous.error().code ==
        presentation::VideoPresentationErrorCode::ambiguous);
}

TEST_CASE("video presentation validates the complete interleaved session") {
  const auto session = id<domain::SessionId>("session");
  auto events = with_interleaved_unrelated_event(published_run("run", 1));

  auto selected = presentation::select_video_presentation(
      session, events, id<domain::ArtifactId>("video-artifact"));
  REQUIRE(selected);
  CHECK(selected->run_id == id<domain::RunId>("run"));

  SECTION("zero schema in an unrelated run is rejected") {
    events[1].metadata.schema_version = 0;
  }
  SECTION("duplicate event identity in an unrelated run is rejected") {
    events[1].metadata.event_id = events.front().metadata.event_id;
  }
  SECTION("global sequence regression in an unrelated run is rejected") {
    events[1].metadata.sequence = events.front().metadata.sequence;
  }
  auto rejected = presentation::select_video_presentation(
      session, events, id<domain::ArtifactId>("video-artifact"));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        presentation::VideoPresentationErrorCode::invalid_history);
}

TEST_CASE("video presentation rejects incomplete atomic lifecycle tails") {
  const auto session = id<domain::SessionId>("session");
  auto events = published_run("run", 1);
  events.pop_back();
  auto rejected = presentation::select_video_presentation(session, events);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        presentation::VideoPresentationErrorCode::invalid_history);

  events = published_run("run", 1);
  events.push_back(event("run", 9,
                         domain::VideoCleanupCompleted{
                             id<domain::VideoOperationId>("operation-run"),
                             id<domain::VideoJobId>("secret-job-run"), 1}));
  rejected = presentation::select_video_presentation(session, events);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        presentation::VideoPresentationErrorCode::invalid_history);
}

TEST_CASE("nonterminal video is absent and published cleanup is explicit") {
  const auto session = id<domain::SessionId>("session");
  auto events = published_run("run", 1);
  events.erase(events.begin() + 5, events.end());
  auto nonterminal = presentation::select_video_presentation(session, events);
  REQUIRE_FALSE(nonterminal);
  CHECK(nonterminal.error().code ==
        presentation::VideoPresentationErrorCode::not_found);

  events = published_run("run", 1);
  auto shown = presentation::select_video_presentation(session, events);
  REQUIRE(shown);
  CHECK(shown->lifecycle_state == domain::VideoLifecycleState::cleanup_pending);
  CHECK(shown->cleanup_pending);

  const auto operation = id<domain::VideoOperationId>("operation-run");
  const auto job = id<domain::VideoJobId>("secret-job-run");
  events.push_back(
      event("run", 9,
            domain::VideoCleanupFailed{
                operation,
                job,
                1,
                {domain::ErrorCode::backend, "must not be rendered", true}}));
  shown = presentation::select_video_presentation(session, events);
  REQUIRE(shown);
  CHECK(shown->lifecycle_state == domain::VideoLifecycleState::cleanup_failed);
  CHECK(shown->cleanup_pending);

  events = published_run("run", 1);
  events.push_back(
      event("run", 9, domain::VideoCleanupCompleted{operation, job, 1}));
  events.push_back(event("run", 10, domain::RunCompleted{}));
  shown = presentation::select_video_presentation(session, events);
  REQUIRE(shown);
  CHECK(shown->lifecycle_state == domain::VideoLifecycleState::completed);
  CHECK_FALSE(shown->cleanup_pending);
}

TEST_CASE("plain video presentation is bounded and redacts provider identity") {
  const auto session = id<domain::SessionId>("session");
  auto shown =
      presentation::select_video_presentation(session, published_run("run", 1));
  REQUIRE(shown);
  const auto text = presentation::render_video_presentation_text(*shown);
  CHECK(text.find("session=session") != std::string::npos);
  CHECK(text.find("run=run") != std::string::npos);
  CHECK(text.find("operation=operation-run") != std::string::npos);
  CHECK(text.find("media=video/mp4") != std::string::npos);
  CHECK(text.find("secret-job") == std::string::npos);
  CHECK(text.find("http") == std::string::npos);
  CHECK(text.size() < 1024);
}

TEST_CASE(
    "ProcessVideo show replays metadata without artifact or provider effects") {
  const auto bytes = minimal_mp4();
  const auto metadata = metadata_for(bytes);
  auto sessions = scripted_session(published_run(metadata));
  int session_factory_calls{};
  int artifact_factory_calls{};
  adapters::ProcessVideoCommand command{
      session_factory(sessions, session_factory_calls),
      [&](void) -> std::expected<std::shared_ptr<storage::ArtifactStore>,
                                 cli::CommandFailure> {
        ++artifact_factory_calls;
        return std::unexpected(cli::CommandFailure{
            cli::CommandFailureKind::runtime,
            "artifact boundary must not be opened for show"});
      }};
  std::istringstream input;
  cli::CommandEnvironment environment{input, false, false, false, {}};
  std::ostringstream output;
  std::ostringstream error;

  auto shown =
      command.show({id<domain::SessionId>("session"), metadata.artifact_id},
                   environment, output, error);
  REQUIRE(shown);
  CHECK(session_factory_calls == 1);
  CHECK(artifact_factory_calls == 0);
  CHECK(sessions->remaining_exchanges() == 0);
  CHECK(output.str().find("artifact=video-artifact") != std::string::npos);
  CHECK(output.str().find("provider-job") == std::string::npos);
  CHECK(error.str().empty());
}

TEST_CASE("ProcessVideo show rejects unrelated corrupt session envelopes") {
  const auto bytes = minimal_mp4();
  const auto metadata = metadata_for(bytes);
  auto events = with_interleaved_unrelated_event(published_run(metadata));
  events[1].metadata.schema_version = 0;
  auto sessions = scripted_session(std::move(events));
  int session_factory_calls{};
  int artifact_factory_calls{};
  adapters::ProcessVideoCommand command{
      session_factory(sessions, session_factory_calls),
      [&](void) -> std::expected<std::shared_ptr<storage::ArtifactStore>,
                                 cli::CommandFailure> {
        ++artifact_factory_calls;
        return std::unexpected(cli::CommandFailure{
            cli::CommandFailureKind::runtime,
            "artifact boundary must not be opened for invalid replay"});
      }};
  std::istringstream input;
  cli::CommandEnvironment environment{input, false, false, false, {}};
  std::ostringstream output;
  std::ostringstream error;

  auto shown =
      command.show({id<domain::SessionId>("session"), metadata.artifact_id},
                   environment, output, error);
  REQUIRE_FALSE(shown);
  CHECK(shown.error().kind == cli::CommandFailureKind::runtime);
  CHECK(session_factory_calls == 1);
  CHECK(artifact_factory_calls == 0);
  CHECK(sessions->remaining_exchanges() == 0);
  CHECK(output.str().empty());
  CHECK(error.str().empty());
}

TEST_CASE(
    "ProcessVideo export validates every artifact failure before output") {
  TemporaryDirectory temporary;
  const auto valid = minimal_mp4();
  const auto valid_metadata = metadata_for(valid);
  int case_number{};
  const auto expect_failure = [&](domain::ArtifactMetadata metadata,
                                  RecordingArtifactStore::Outcome outcome,
                                  const int expected_gets) {
    CAPTURE(case_number);
    auto sessions = scripted_session(published_run(metadata));
    auto artifacts =
        std::make_shared<RecordingArtifactStore>(std::move(outcome));
    int session_factory_calls{};
    int artifact_factory_calls{};
    adapters::ProcessVideoCommand command{
        session_factory(sessions, session_factory_calls),
        artifact_factory(artifacts, artifact_factory_calls)};
    std::istringstream input;
    cli::CommandEnvironment environment{input, false, false, false, {}};
    std::ostringstream output;
    std::ostringstream error;
    const auto destination =
        temporary.path() / ("failed-" + std::to_string(case_number++) + ".mp4");
    auto exported =
        command.export_artifact({id<domain::SessionId>("session"),
                                 metadata.artifact_id, destination.string()},
                                environment, output, error);
    REQUIRE_FALSE(exported);
    CHECK(exported.error().kind == cli::CommandFailureKind::runtime);
    CHECK(exported.error().message.find("secret") == std::string::npos);
    CHECK(session_factory_calls == 1);
    CHECK(artifact_factory_calls == 1);
    CHECK(artifacts->get_calls == expected_gets);
    CHECK(output.str().empty());
    CHECK(error.str().empty());
    CHECK_FALSE(std::filesystem::exists(destination));
  };

  expect_failure(
      valid_metadata,
      storage::ArtifactStoreError{storage::ArtifactStoreErrorCode::unavailable,
                                  "secret source path", false},
      1);

  auto digest_mismatch = valid;
  digest_mismatch.back() = std::byte{0x11};
  expect_failure(valid_metadata,
                 storage::ArtifactRead{valid_metadata, digest_mismatch}, 1);

  auto truncated = valid;
  truncated.pop_back();
  expect_failure(valid_metadata,
                 storage::ArtifactRead{valid_metadata, truncated}, 1);

  auto malformed = valid;
  malformed[4] = std::byte{'x'};
  const auto malformed_metadata = metadata_for(malformed);
  expect_failure(malformed_metadata,
                 storage::ArtifactRead{malformed_metadata, malformed}, 1);

  auto returned_metadata = valid_metadata;
  returned_metadata.artifact_id = id<domain::ArtifactId>("other-artifact");
  expect_failure(valid_metadata,
                 storage::ArtifactRead{returned_metadata, valid}, 1);

  auto over = valid_metadata;
  over.byte_size = video::Mp4Limits{}.maximum_bytes + 1U;
  expect_failure(over,
                 storage::ArtifactStoreError{
                     storage::ArtifactStoreErrorCode::internal_failure,
                     "must not read over-limit metadata", false},
                 0);
}

TEST_CASE("ProcessVideo export accepts the exact MP4 byte ceiling") {
  TemporaryDirectory temporary;
  auto exact = minimal_mp4();
  const auto media_offset = exact.size() - 9U;
  const auto maximum = video::Mp4Limits{}.maximum_bytes;
  exact.resize(maximum);
  set_u32(exact, media_offset,
          static_cast<std::uint32_t>(maximum - media_offset));
  const auto metadata = metadata_for(exact);
  auto sessions = scripted_session(published_run(metadata));
  auto artifacts = std::make_shared<RecordingArtifactStore>(
      storage::ArtifactRead{metadata, exact});
  int session_factory_calls{};
  int artifact_factory_calls{};
  adapters::ProcessVideoCommand command{
      session_factory(sessions, session_factory_calls),
      artifact_factory(artifacts, artifact_factory_calls)};
  std::istringstream input;
  cli::CommandEnvironment environment{input, false, false, false, {}};
  std::ostringstream output;
  std::ostringstream error;
  const auto destination = temporary.path() / "exact.mp4";

  auto exported =
      command.export_artifact({id<domain::SessionId>("session"),
                               metadata.artifact_id, destination.string()},
                              environment, output, error);
  REQUIRE(exported);
  CHECK(session_factory_calls == 1);
  CHECK(artifact_factory_calls == 1);
  CHECK(artifacts->get_calls == 1);
  CHECK(artifacts->seen_maximum_bytes == maximum);
  CHECK(std::filesystem::file_size(destination) == maximum);
  CHECK(output.str().find("bytes=" + std::to_string(maximum)) !=
        std::string::npos);
  CHECK(error.str().empty());
}

TEST_CASE("secure export refuses existing symlinked and unsafe destinations") {
#ifdef _WIN32
  SKIP("POSIX secure export contract");
#else
  TemporaryDirectory temporary;
  const std::vector content{std::byte{1}, std::byte{2}};
  const auto existing = temporary.path() / "existing.mp4";
  REQUIRE(adapters::detail::secure_export_bytes(content, existing));
  auto rejected = adapters::detail::secure_export_bytes(content, existing);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().kind == cli::CommandFailureKind::usage);

  const auto target = temporary.path() / "target";
  std::filesystem::create_directory(target);
  const auto linked = temporary.path() / "linked";
  std::filesystem::create_directory_symlink(target, linked);
  rejected =
      adapters::detail::secure_export_bytes(content, linked / "escaped.mp4");
  REQUIRE_FALSE(rejected);
  CHECK_FALSE(std::filesystem::exists(target / "escaped.mp4"));

  const auto final_link = temporary.path() / "linked.mp4";
  std::filesystem::create_symlink(existing, final_link);
  rejected = adapters::detail::secure_export_bytes(content, final_link);
  REQUIRE_FALSE(rejected);
  CHECK(std::filesystem::file_size(existing) == content.size());

  rejected = adapters::detail::secure_export_bytes(
      content, temporary.path() / ".." / "escaped.mp4");
  REQUIRE_FALSE(rejected);

  ScopedCurrentDirectory current{temporary.path()};
  REQUIRE(adapters::detail::secure_export_bytes(content, "./relative.mp4"));
  CHECK(std::filesystem::file_size(temporary.path() / "relative.mp4") ==
        content.size());
#endif
}

TEST_CASE("secure export completes short writes and removes cancellation") {
#ifdef _WIN32
  SKIP("POSIX secure export contract");
#else
  TemporaryDirectory temporary;
  const std::vector content{std::byte{1}, std::byte{2}, std::byte{3},
                            std::byte{4}, std::byte{5}};
  const auto output = temporary.path() / "short.mp4";
  std::size_t writes{};
  adapters::detail::SecureExportTestHooks short_write;
  short_write.write = [&](const int descriptor,
                          const std::span<const std::byte> remaining)
      -> std::expected<std::size_t, int> {
    ++writes;
    const auto amount = std::min<std::size_t>(2, remaining.size());
    const auto count = ::write(descriptor, remaining.data(), amount);
    if (count < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(count);
  };
  auto exported = adapters::detail::secure_export_bytes(content, output, {},
                                                        std::move(short_write));
  REQUIRE(exported);
  CHECK(writes == 3);
  CHECK(std::filesystem::file_size(output) == content.size());

  std::stop_source cancellation;
  const auto partial = temporary.path() / "cancelled.mp4";
  adapters::detail::SecureExportTestHooks cancel_write;
  cancel_write.write = [&](const int descriptor,
                           const std::span<const std::byte> remaining)
      -> std::expected<std::size_t, int> {
    const auto count = ::write(descriptor, remaining.data(), 1);
    if (count < 0) return std::unexpected(errno);
    cancellation.request_stop();
    return static_cast<std::size_t>(count);
  };
  exported = adapters::detail::secure_export_bytes(
      content, partial, cancellation.get_token(), std::move(cancel_write));
  REQUIRE_FALSE(exported);
  CHECK(exported.error().kind == cli::CommandFailureKind::cancelled);
  CHECK_FALSE(std::filesystem::exists(partial));

  const auto zero = temporary.path() / "zero.mp4";
  adapters::detail::SecureExportTestHooks zero_write;
  zero_write.write =
      [](int, std::span<const std::byte>) -> std::expected<std::size_t, int> {
    return 0;
  };
  exported = adapters::detail::secure_export_bytes(content, zero, {},
                                                   std::move(zero_write));
  REQUIRE_FALSE(exported);
  CHECK_FALSE(std::filesystem::exists(zero));
#endif
}

TEST_CASE("secure export never deletes substituted destination names") {
#ifdef _WIN32
  SKIP("POSIX secure export contract");
#else
  TemporaryDirectory temporary;
  const std::vector content{std::byte{1}, std::byte{2}, std::byte{3}};
  const auto create_replacement = [](const int parent,
                                     const std::string_view name) {
    const auto descriptor =
        ::openat(parent, std::string{name}.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    REQUIRE(descriptor >= 0);
    const std::byte marker{0x7f};
    REQUIRE(::write(descriptor, &marker, 1) == 1);
    REQUIRE(::close(descriptor) == 0);
  };

  const auto before = temporary.path() / "before.mp4";
  adapters::detail::SecureExportTestHooks before_publish;
  before_publish.before_publish = create_replacement;
  auto result = adapters::detail::secure_export_bytes(
      content, before, {}, std::move(before_publish));
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == cli::CommandFailureKind::usage);
  CHECK(std::filesystem::file_size(before) == 1);

  const auto after = temporary.path() / "after.mp4";
  const auto moved = temporary.path() / "moved-task-output.mp4";
  adapters::detail::SecureExportTestHooks after_publish;
  after_publish.after_publish = [&](const int parent,
                                    const std::string_view name) {
    REQUIRE(::renameat(parent, std::string{name}.c_str(), parent,
                       moved.filename().c_str()) == 0);
    create_replacement(parent, name);
  };
  result = adapters::detail::secure_export_bytes(content, after, {},
                                                 std::move(after_publish));
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == cli::CommandFailureKind::runtime);
  CHECK(std::filesystem::file_size(after) == 1);
  CHECK(std::filesystem::file_size(moved) == content.size());
#endif
}

TEST_CASE(
    "secure export has explicit cancellation error and cleanup precedence") {
#ifdef _WIN32
  SKIP("POSIX secure export contract");
#else
  TemporaryDirectory temporary;
  const std::vector content{std::byte{1}, std::byte{2}};

  std::stop_source before_commit_stop;
  adapters::detail::SecureExportTestHooks cancel_before_publish;
  cancel_before_publish.before_publish = [&](int, std::string_view) {
    before_commit_stop.request_stop();
  };
  const auto cancelled_path = temporary.path() / "cancelled-before.mp4";
  auto result = adapters::detail::secure_export_bytes(
      content, cancelled_path, before_commit_stop.get_token(),
      std::move(cancel_before_publish));
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == cli::CommandFailureKind::cancelled);
  CHECK_FALSE(std::filesystem::exists(cancelled_path));

  std::stop_source write_stop;
  adapters::detail::SecureExportTestHooks write_failure;
  write_failure.write =
      [&](int, std::span<const std::byte>) -> std::expected<std::size_t, int> {
    write_stop.request_stop();
    return std::unexpected(EIO);
  };
  const auto failed_path = temporary.path() / "write-failed.mp4";
  result = adapters::detail::secure_export_bytes(
      content, failed_path, write_stop.get_token(), std::move(write_failure));
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == cli::CommandFailureKind::runtime);
  CHECK(result.error().message == "output file could not be written");
  CHECK_FALSE(std::filesystem::exists(failed_path));

  std::stop_source cleanup_stop;
  adapters::detail::SecureExportTestHooks cleanup_failure;
  cleanup_failure.write =
      [&](int, std::span<const std::byte>) -> std::expected<std::size_t, int> {
    cleanup_stop.request_stop();
    return std::unexpected(EIO);
  };
  cleanup_failure.before_cleanup = [](const int descriptor) {
    REQUIRE(::close(descriptor) == 0);
  };
  const auto cleanup_path = temporary.path() / "cleanup-failed.mp4";
  result = adapters::detail::secure_export_bytes(content, cleanup_path,
                                                 cleanup_stop.get_token(),
                                                 std::move(cleanup_failure));
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == cli::CommandFailureKind::runtime);
  CHECK(result.error().message == "temporary output cleanup failed");
  CHECK_FALSE(std::filesystem::exists(cleanup_path));

  std::stop_source after_commit_stop;
  adapters::detail::SecureExportTestHooks cancel_after_publish;
  cancel_after_publish.after_publish = [&](int, std::string_view) {
    after_commit_stop.request_stop();
  };
  const auto committed_path = temporary.path() / "committed.mp4";
  result = adapters::detail::secure_export_bytes(
      content, committed_path, after_commit_stop.get_token(),
      std::move(cancel_after_publish));
  REQUIRE(result);
  CHECK(std::filesystem::file_size(committed_path) == content.size());
#endif
}

} // namespace
