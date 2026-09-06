#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/event_log.hpp>
#include <aiforge/runtime/video_coordinator.hpp>
#include <aiforge/testing/scripted_artifact_store.hpp>
#include <aiforge/testing/scripted_video_service.hpp>

namespace {

using namespace aiforge;

template <typename IdType>
[[nodiscard]] auto id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

[[nodiscard]] auto attributes() -> domain::RunStarted {
  return {id<domain::SurfaceId>("video"), id<domain::WorkspaceId>("media"),
          id<domain::PermissionProfileId>("explicit"), std::nullopt};
}

[[nodiscard]] auto spec(std::string prompt = "a quiet ocean")
    -> domain::VideoGenerationSpec {
  return {id<domain::ModelId>("video-model"), std::move(prompt),
          std::chrono::seconds{5}};
}

[[nodiscard]] auto quote() -> domain::MonetaryAmount {
  return domain::MonetaryAmount::create(
             "USD", domain::DecimalAmount::from("1.25").value())
      .value();
}

[[nodiscard]] auto generation() -> runtime::VideoGenerationOperation {
  return {id<domain::RunId>("video-run"), attributes(),
          id<domain::VideoOperationId>("video-operation"), spec(),
          id<domain::ArtifactId>("video-artifact")};
}

auto append_u32(std::vector<std::byte>& bytes, const std::uint32_t value)
    -> void {
  for (const auto shift : {24U, 16U, 8U, 0U})
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

auto append_text(std::vector<std::byte>& bytes, const std::string_view text)
    -> void {
  for (const unsigned char value : text)
    bytes.push_back(static_cast<std::byte>(value));
}

[[nodiscard]] auto box(const std::string_view type,
                       const std::span<const std::byte> payload = {})
    -> std::vector<std::byte> {
  std::vector<std::byte> bytes;
  append_u32(bytes, static_cast<std::uint32_t>(payload.size() + 8));
  append_text(bytes, type);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

auto append(std::vector<std::byte>& into,
            const std::span<const std::byte> value) -> void {
  into.insert(into.end(), value.begin(), value.end());
}

[[nodiscard]] auto minimal_mp4() -> std::vector<std::byte> {
  std::vector<std::byte> file_type;
  append_text(file_type, "isom");
  append_u32(file_type, 0);
  auto bytes = box("ftyp", file_type);
  std::vector<std::byte> handler_payload(24);
  for (std::size_t index{}; index < 4; ++index)
    handler_payload[8 + index] = static_cast<std::byte>("vide"[index]);
  const auto handler = box("hdlr", handler_payload);
  const auto media = box("mdia", handler);
  const auto track = box("trak", media);
  append(bytes, box("moov", track));
  const std::array payload{std::byte{0x42}};
  append(bytes, box("mdat", payload));
  return bytes;
}

[[nodiscard]] auto digest_of(const std::span<const std::byte> content)
    -> std::string {
  detail::Sha256 digest;
  digest.update(content);
  return "sha256:" + digest.finish();
}

[[nodiscard]] auto metadata(const std::span<const std::byte> content)
    -> domain::ArtifactMetadata {
  return {id<domain::ArtifactId>("video-artifact"),
          "video/mp4",
          static_cast<std::uint64_t>(content.size()),
          digest_of(content),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

class MemorySessionStore final : public storage::SessionStore {
 public:
  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (m_session)
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::already_exists,
          "session already exists", false});
    m_session = std::move(session);
    return {};
  }

  auto create_session_with_events(
      storage::SessionCreate session,
      const std::span<const domain::RunEvent> initial_events,
      const std::stop_token stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (stop_token.stop_requested())
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::cancelled,
                                     "atomic creation cancelled", false});
    if (m_session)
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::already_exists,
          "session already exists", false});
    ++append_calls;
    if (fail_append_call && append_calls == *fail_append_call) {
      fail_append_call.reset();
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "injected atomic creation failure", true});
    }
    if (initial_events.empty())
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::invalid_argument,
          "initial event batch is empty", false});
    for (std::size_t index{}; index < initial_events.size(); ++index) {
      if (initial_events[index].metadata.sequence != index + 1)
        return std::unexpected(storage::SessionStoreError{
            storage::SessionStoreErrorCode::conflict,
            "initial event sequence is invalid", false});
    }
    if (stop_token.stop_requested())
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::cancelled,
                                     "atomic creation cancelled", false});
    m_session = std::move(session);
    m_events.assign(initial_events.begin(), initial_events.end());
    return {};
  }

  auto open_session(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    if (!m_session || m_session->session_id != session_id)
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::not_found,
                                     "session not found", false});
    const auto last_activity = m_events.empty()
                                   ? m_session->created_at
                                   : m_events.back().metadata.timestamp;
    std::vector<domain::RunId> runs;
    for (const auto& event : m_events) {
      if (std::ranges::none_of(runs, [&](const auto& run_id) {
            return run_id == event.metadata.run_id;
          })) {
        runs.push_back(event.metadata.run_id);
      }
    }
    return storage::SessionInfo{session_id, m_session->created_at,
                                last_activity,
                                static_cast<std::uint64_t>(m_events.size()),
                                static_cast<std::uint64_t>(runs.size())};
  }

  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }

  auto append_events(const domain::SessionId& session_id,
                     const std::span<const domain::RunEvent> events,
                     std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (!m_session || m_session->session_id != session_id)
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::not_found,
                                     "session not found", false});
    ++append_calls;
    if (fail_append_call && append_calls == *fail_append_call) {
      fail_append_call.reset();
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "injected append failure", true});
    }
    if (conflict_append_call && conflict_run_id &&
        append_calls == *conflict_append_call) {
      conflict_append_call.reset();
      const auto sequence = m_events.size() + 1;
      m_events.push_back(
          {{id<domain::EventId>("concurrent-event-" + std::to_string(sequence)),
            *conflict_run_id, sequence, 1,
            domain::EventTimestamp{std::chrono::milliseconds{1}}, std::nullopt,
            std::nullopt, std::nullopt},
           domain::UnknownEvent{"future.concurrent",
                                {"application/json", "{}"}}});
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::conflict,
                                     "injected sequence conflict", true});
    }
    for (const auto& event : events) {
      if (event.metadata.sequence != m_events.size() + 1)
        return std::unexpected(
            storage::SessionStoreError{storage::SessionStoreErrorCode::conflict,
                                       "sequence conflict", false});
      m_events.push_back(event);
    }
    return {};
  }

  auto replay_events(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    if (!m_session || m_session->session_id != session_id)
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::not_found,
                                     "session not found", false});
    ++replay_calls;
    return m_events;
  }

  [[nodiscard]] auto events() const noexcept
      -> const std::vector<domain::RunEvent>& {
    return m_events;
  }

  int append_calls{};
  int replay_calls{};
  std::optional<int> fail_append_call;
  std::optional<int> conflict_append_call;
  std::optional<domain::RunId> conflict_run_id;

 private:
  std::optional<storage::SessionCreate> m_session;
  std::vector<domain::RunEvent> m_events;
};

[[nodiscard]] auto session(const runtime::VideoSessionMode mode)
    -> runtime::VideoSessionOpen {
  return {id<domain::SessionId>("video-session"), mode,
          domain::EventTimestamp{std::chrono::milliseconds{1}}};
}

[[nodiscard]] auto event_for(const std::uint64_t sequence,
                             const domain::RunId& run_id,
                             domain::RunEventPayload payload)
    -> domain::RunEvent {
  return {{id<domain::EventId>("event-" + std::to_string(sequence)), run_id,
           sequence, 1, domain::EventTimestamp{std::chrono::milliseconds{1}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

[[nodiscard]] auto event(const std::uint64_t sequence,
                         domain::RunEventPayload payload) -> domain::RunEvent {
  return event_for(sequence, id<domain::RunId>("video-run"),
                   std::move(payload));
}

TEST_CASE("video identities specifications and transient URLs are bounded") {
  CHECK_FALSE(domain::VideoOperationId::from(""));
  CHECK_FALSE(domain::VideoJobId::from(std::string(129, 'x')));
  CHECK_FALSE(domain::validate_video_generation_spec(spec("")));
  auto overlong =
      spec(std::string(domain::maximum_video_prompt_bytes + 1, 'x'));
  CHECK_FALSE(domain::validate_video_generation_spec(overlong));
  auto zero_duration = spec();
  zero_duration.duration = std::chrono::seconds::zero();
  CHECK_FALSE(domain::validate_video_generation_spec(zero_duration));
  CHECK_FALSE(backend::TransientVideoUrl::from(""));
  CHECK_FALSE(backend::TransientVideoUrl::from("https://example.test/a b"));
  CHECK_FALSE(backend::TransientVideoUrl::from("http://example.test/video"));
  CHECK_FALSE(backend::TransientVideoUrl::from("https:///video"));
  CHECK_FALSE(
      backend::TransientVideoUrl::from("https://user@example.test/video"));
  CHECK(backend::TransientVideoUrl::from("https://example.test/private"));
  const auto operation = id<domain::VideoOperationId>("transcription");
  CHECK_FALSE(
      domain::validate_video_transcription({operation, "", std::nullopt}));
  CHECK_FALSE(domain::validate_video_transcription(
      {operation, std::string{"bad\x01text"}, std::nullopt}));
  CHECK_FALSE(domain::validate_video_transcription(
      {operation,
       std::string(domain::maximum_video_transcription_bytes + 1, 'x'),
       std::nullopt}));
  CHECK_FALSE(domain::validate_video_transcription(
      {operation, "valid", std::string{"bad language"}}));
}

TEST_CASE("failed atomic video creation leaves no empty session") {
  const auto session_id = id<domain::SessionId>("video-session");
  const auto run_id = id<domain::RunId>("video-run");
  const auto operation_id = id<domain::VideoOperationId>("transcription");
  const auto model_id = id<domain::ModelId>("transcription-model");
  MemorySessionStore sessions;
  sessions.fail_append_call = 1;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {},
      {},
      {},
      {},
      {{{operation_id, model_id},
        backend::VideoTranscription{"transcript", std::nullopt}}}};
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  auto first_url =
      backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(first_url);

  const auto interrupted = coordinator.transcribe(
      {session_id, runtime::VideoSessionMode::create_session,
       domain::EventTimestamp{std::chrono::milliseconds{1}}},
      {run_id, attributes(), operation_id, model_id, std::move(*first_url)});

  REQUIRE_FALSE(interrupted);
  CHECK(interrupted.error().code ==
        runtime::VideoCoordinatorErrorCode::storage_failure);
  const auto absent = sessions.open_session(session_id, {});
  REQUIRE_FALSE(absent);
  CHECK(absent.error().code == storage::SessionStoreErrorCode::not_found);
  CHECK(service.recorded_calls().empty());

  auto retry_url =
      backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(retry_url);
  const auto retried = coordinator.transcribe(
      {session_id, runtime::VideoSessionMode::create_session,
       domain::EventTimestamp{std::chrono::milliseconds{1}}},
      {run_id, attributes(), operation_id, model_id, std::move(*retry_url)});
  REQUIRE(retried);
  CHECK(retried->text == "transcript");
  REQUIRE(sessions.events().size() == 4);
  CHECK(std::holds_alternative<domain::RunStarted>(
      sessions.events().front().payload));
  CHECK(std::holds_alternative<domain::RunCompleted>(
      sessions.events().back().payload));
}

TEST_CASE("video projection rejects repeated stale and out-of-order facts") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  domain::VideoProjection projection;
  REQUIRE(projection.apply(event(1, attributes())));
  REQUIRE(projection.apply(event(
      2, domain::VideoGenerationRequested{
             operation.operation_id, operation.spec, operation.artifact_id})));
  REQUIRE(projection.apply(
      event(3, domain::VideoQuoteObserved{operation.operation_id, quote()})));
  REQUIRE(projection.apply(
      event(4, domain::VideoJobQueued{operation.operation_id, job})));
  REQUIRE(projection.apply(event(
      5, domain::VideoJobStatusObserved{operation.operation_id, job, 1,
                                        domain::VideoJobState::processing})));

  CHECK_FALSE(projection.apply(event(
      6, domain::VideoJobStatusObserved{operation.operation_id, job, 1,
                                        domain::VideoJobState::processing})));
  CHECK_FALSE(projection.apply(
      event(6, domain::VideoJobStatusObserved{operation.operation_id, job, 2,
                                              domain::VideoJobState::queued})));
  CHECK_FALSE(projection.apply(
      event(6, domain::VideoJobStatusObserved{
                   operation.operation_id, job, 2,
                   static_cast<domain::VideoJobState>(
                       std::numeric_limits<unsigned char>::max())})));
  CHECK_FALSE(projection.apply(
      event(6, domain::ArtifactCreated{metadata(minimal_mp4())})));
  REQUIRE(projection.apply(event(
      6, domain::UnknownEvent{"future.video", {"application/json", "{}"}})));
  CHECK(projection.last_sequence() == 6);
}

TEST_CASE("video projection seals cancellation and failed-job handoffs") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  domain::VideoProjection cancelled;
  REQUIRE(cancelled.apply(event(1, attributes())));
  REQUIRE(cancelled.apply(event(
      2, domain::VideoGenerationRequested{
             operation.operation_id, operation.spec, operation.artifact_id})));
  REQUIRE(cancelled.apply(
      event(3, domain::RunCancelRequested{std::string{"cancelled"}})));
  REQUIRE(cancelled.apply(event(
      4, domain::UnknownEvent{"future.video", {"application/json", "{}"}})));
  CHECK_FALSE(cancelled.apply(
      event(5, domain::VideoQuoteObserved{operation.operation_id, quote()})));
  REQUIRE(cancelled.apply(
      event(5, domain::RunCancelled{std::string{"cancelled"}})));

  domain::VideoProjection failed;
  REQUIRE(failed.apply(event(1, attributes())));
  REQUIRE(failed.apply(event(
      2, domain::VideoGenerationRequested{
             operation.operation_id, operation.spec, operation.artifact_id})));
  REQUIRE(failed.apply(
      event(3, domain::VideoQuoteObserved{operation.operation_id, quote()})));
  REQUIRE(failed.apply(
      event(4, domain::VideoJobQueued{operation.operation_id, job})));
  REQUIRE(failed.apply(
      event(5, domain::VideoJobStatusObserved{operation.operation_id, job, 1,
                                              domain::VideoJobState::failed})));
  REQUIRE(failed.apply(event(
      6, domain::UnknownEvent{"future.video", {"application/json", "{}"}})));
  CHECK_FALSE(failed.apply(
      event(7, domain::RunCancelRequested{std::string{"cancelled"}})));
  REQUIRE(failed.apply(event(
      7, domain::RunFailed{{domain::ErrorCode::backend, "failed", false}})));

  domain::VideoProjection no_publication;
  REQUIRE(no_publication.apply(event(1, attributes())));
  REQUIRE(no_publication.apply(event(
      2, domain::VideoGenerationRequested{
             operation.operation_id, operation.spec, operation.artifact_id})));
  CHECK_FALSE(no_publication.apply(
      event(3, domain::VideoCleanupPending{operation.operation_id, job, 1})));
}

TEST_CASE("video rebuild rejects incomplete atomic lifecycle tails") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  const auto artifact = metadata(minimal_mp4());
  const auto session_id = id<domain::SessionId>("video-session");
  const auto check_rejected = [&](std::vector<domain::RunEvent> events) {
    domain::SessionEventLog log{session_id};
    for (auto& value : events)
      REQUIRE(log.append(std::move(value)));
    const auto rebuilt =
        domain::VideoProjection::rebuild(log, operation.run_id);
    REQUIRE_FALSE(rebuilt);
    CHECK(rebuilt.error().code ==
          domain::VideoProjectionErrorCode::invalid_transition);
  };
  const auto generation_prefix = [&] {
    return std::vector<domain::RunEvent>{
        event(1, attributes()),
        event(2, domain::VideoGenerationRequested{operation.operation_id,
                                                  operation.spec,
                                                  operation.artifact_id}),
        event(3, domain::VideoQuoteObserved{operation.operation_id, quote()}),
        event(4, domain::VideoJobQueued{operation.operation_id, job}),
        event(5, domain::VideoJobStatusObserved{
                     operation.operation_id, job, 1,
                     domain::VideoJobState::completed})};
  };

  SECTION("failed status requires its terminal envelope") {
    auto events = generation_prefix();
    events.back().payload = domain::VideoJobStatusObserved{
        operation.operation_id, job, 1, domain::VideoJobState::failed};
    check_rejected(std::move(events));
  }
  SECTION("cancellation request requires its terminal envelope") {
    check_rejected(
        {event(1, attributes()),
         event(2, domain::VideoGenerationRequested{operation.operation_id,
                                                   operation.spec,
                                                   operation.artifact_id}),
         event(3, domain::RunCancelRequested{std::string{"cancelled"}})});
  }
  SECTION("artifact creation cannot be durable without publication") {
    auto events = generation_prefix();
    events.push_back(event(6, domain::ArtifactCreated{artifact}));
    check_rejected(std::move(events));
  }
  SECTION("publication requires the first cleanup-pending fact") {
    auto events = generation_prefix();
    events.push_back(event(6, domain::ArtifactCreated{artifact}));
    events.push_back(event(7, domain::VideoArtifactPublished{
                                  operation.operation_id, job, artifact}));
    check_rejected(std::move(events));
  }
  SECTION("cleanup completion requires run completion") {
    auto events = generation_prefix();
    events.push_back(event(6, domain::ArtifactCreated{artifact}));
    events.push_back(event(7, domain::VideoArtifactPublished{
                                  operation.operation_id, job, artifact}));
    events.push_back(
        event(8, domain::VideoCleanupPending{operation.operation_id, job, 1}));
    events.push_back(event(
        9, domain::VideoCleanupCompleted{operation.operation_id, job, 1}));
    check_rejected(std::move(events));
  }
  SECTION("transcription observation requires run completion") {
    const auto transcription_id =
        id<domain::VideoOperationId>("transcription-operation");
    check_rejected(
        {event(1, attributes()),
         event(
             2,
             domain::VideoTranscriptionRequested{
                 transcription_id, id<domain::ModelId>("transcription-model")}),
         event(3, domain::VideoTranscriptionObserved{
                      transcription_id, "transcript", std::nullopt})});
  }
}

TEST_CASE("resume rejects durable cancellation before every provider effect") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  const std::vector<std::vector<domain::RunEventPayload>> prefixes{
      {domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id}},
      {domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id},
       domain::VideoQuoteObserved{operation.operation_id, quote()}},
      {domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id},
       domain::VideoQuoteObserved{operation.operation_id, quote()},
       domain::VideoJobQueued{operation.operation_id, job}},
      {domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id},
       domain::VideoQuoteObserved{operation.operation_id, quote()},
       domain::VideoJobQueued{operation.operation_id, job},
       domain::VideoJobStatusObserved{operation.operation_id, job, 1,
                                      domain::VideoJobState::processing}},
      {domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id},
       domain::VideoQuoteObserved{operation.operation_id, quote()},
       domain::VideoJobQueued{operation.operation_id, job},
       domain::VideoJobStatusObserved{operation.operation_id, job, 1,
                                      domain::VideoJobState::completed}}};

  for (std::size_t index{}; index < prefixes.size(); ++index) {
    CAPTURE(index);
    MemorySessionStore sessions;
    const auto session_id = id<domain::SessionId>("video-session");
    REQUIRE(sessions.create_session(
        {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}},
        {}));
    std::vector<domain::RunEvent> events{event(1, attributes())};
    std::uint64_t sequence{2};
    for (const auto& payload : prefixes[index])
      events.push_back(event(sequence++, payload));
    events.push_back(
        event(sequence, domain::RunCancelRequested{std::string{"cancelled"}}));
    REQUIRE(sessions.append_events(session_id, events, {}));
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service;
    runtime::VideoCoordinator coordinator{sessions, artifacts, service};

    const auto resumed = coordinator.generate(
        session(runtime::VideoSessionMode::resume_run), operation);

    REQUIRE_FALSE(resumed);
    CHECK(resumed.error().code ==
          runtime::VideoCoordinatorErrorCode::replay_rejected);
    CHECK(service.recorded_calls().empty());
    CHECK(artifacts.recorded_calls().empty());
  }

  const auto operation_id =
      id<domain::VideoOperationId>("transcription-operation");
  const auto model_id = id<domain::ModelId>("transcription-model");
  MemorySessionStore sessions;
  const auto session_id = id<domain::SessionId>("video-session");
  REQUIRE(sessions.create_session(
      {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}}, {}));
  const std::array events{
      event(1, attributes()),
      event(2, domain::VideoTranscriptionRequested{operation_id, model_id}),
      event(3, domain::RunCancelRequested{std::string{"cancelled"}})};
  REQUIRE(sessions.append_events(session_id, events, {}));
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service;
  runtime::VideoCoordinator coordinator{sessions, artifacts, service};
  auto url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);

  const auto resumed =
      coordinator.transcribe(session(runtime::VideoSessionMode::resume_run),
                             {operation.run_id, attributes(), operation_id,
                              model_id, std::move(*url)});

  REQUIRE_FALSE(resumed);
  CHECK(resumed.error().code ==
        runtime::VideoCoordinatorErrorCode::replay_rejected);
  CHECK(service.recorded_calls().empty());
  CHECK(artifacts.recorded_calls().empty());
}

TEST_CASE("scripted video service enforces and captures response ceilings") {
  const auto job = id<domain::VideoJobId>("job");
  const auto model = id<domain::ModelId>("model");
  const std::vector media{std::byte{1}, std::byte{2}, std::byte{3}};
  testing::ScriptedVideoService exact_media{
      {},
      {},
      {{{job, model},
        backend::VideoRetrieval{backend::VideoMedia{media, "video/mp4"}}}}};
  REQUIRE(exact_media.retrieve(
      job, model, {std::chrono::milliseconds{5}, {}, media.size()}));
  REQUIRE(exact_media.recorded_options().size() == 1);
  CHECK(exact_media.recorded_options().front().maximum_response_bytes ==
        media.size());

  testing::ScriptedVideoService oversized_media{
      {},
      {},
      {{{job, model},
        backend::VideoRetrieval{backend::VideoMedia{media, "video/mp4"}}}}};
  CHECK_FALSE(oversized_media.retrieve(
      job, model, {std::chrono::milliseconds{5}, {}, media.size() - 1}));

  const auto operation = id<domain::VideoOperationId>("transcription");
  testing::ScriptedVideoService exact_text{
      {},
      {},
      {},
      {},
      {{{operation, model},
        backend::VideoTranscription{"abc", std::string{"en"}}}}};
  auto url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);
  REQUIRE(exact_text.transcribe(
      {operation, model, std::move(*url)},
      {std::chrono::milliseconds{5}, {}, std::size_t{5}}));

  testing::ScriptedVideoService oversized_text{
      {},
      {},
      {},
      {},
      {{{operation, model},
        backend::VideoTranscription{"abc", std::string{"en"}}}}};
  url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);
  CHECK_FALSE(oversized_text.transcribe(
      {operation, model, std::move(*url)},
      {std::chrono::milliseconds{5}, {}, std::size_t{4}}));
}

TEST_CASE("scripted video queue provides hard semantic idempotency") {
  const auto operation = id<domain::VideoOperationId>("operation");
  const auto job = id<domain::VideoJobId>("job");
  testing::ScriptedVideoService service{
      {},
      {{testing::VideoQueueCall{operation, spec()},
        backend::VideoQueued{job}}}};
  REQUIRE(service.queue(operation, spec()));
  const auto repeated = service.queue(operation, spec());
  REQUIRE(repeated);
  CHECK(repeated->job_id == job);
  const auto mismatched = service.queue(operation, spec("different"));
  REQUIRE_FALSE(mismatched);
  CHECK(mismatched.error().code == backend::VideoServiceErrorCode::protocol);
}

TEST_CASE("queue recovery reuses one semantic operation after append failure") {
  const auto bytes = minimal_mp4();
  const auto artifact = metadata(bytes);
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  MemorySessionStore sessions;
  sessions.fail_append_call = 3;
  testing::ScriptedArtifactStore artifacts{
      {{{{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
          std::nullopt, std::nullopt},
         bytes},
        artifact}}};
  testing::ScriptedVideoService service{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
      {{{job, operation.spec.model_id},
        backend::VideoRetrieval{
            backend::VideoStatus{domain::VideoJobState::completed}}},
       {{job, operation.spec.model_id},
        backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
      {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}};
  const auto now = [] {
    return domain::EventTimestamp{std::chrono::milliseconds{2}};
  };
  runtime::VideoCoordinator coordinator{sessions, artifacts, service, now};
  const auto interrupted = coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation);
  REQUIRE_FALSE(interrupted);
  CHECK(interrupted.error().code ==
        runtime::VideoCoordinatorErrorCode::storage_failure);

  const auto resumed = coordinator.generate(
      session(runtime::VideoSessionMode::resume_run), operation);
  REQUIRE(resumed);
  CHECK_FALSE(resumed->cleanup_pending);
  std::size_t queue_calls{};
  for (const auto& call : service.recorded_calls())
    queue_calls += std::holds_alternative<testing::VideoQueueCall>(call);
  CHECK(queue_calls == 2);
}

TEST_CASE("interleaved sequence conflicts do not repeat provider effects") {
  const auto bytes = minimal_mp4();
  const auto artifact = metadata(bytes);
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  for (const int conflict_at : {3, 5}) {
    CAPTURE(conflict_at);
    MemorySessionStore sessions;
    sessions.conflict_append_call = conflict_at;
    sessions.conflict_run_id = id<domain::RunId>("concurrent-run");
    testing::ScriptedArtifactStore artifacts{
        {{{{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
            std::nullopt, std::nullopt},
           bytes},
          artifact}}};
    testing::ScriptedVideoService service{
        {{{operation.spec}, backend::VideoQuote{quote()}}},
        {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
        {{{job, operation.spec.model_id},
          backend::VideoRetrieval{
              backend::VideoStatus{domain::VideoJobState::completed}}},
         {{job, operation.spec.model_id},
          backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
        {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}};
    runtime::VideoCoordinator coordinator{
        sessions, artifacts, service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};

    const auto result = coordinator.generate(
        session(runtime::VideoSessionMode::create_session), operation);

    REQUIRE(result);
    CHECK_FALSE(result->cleanup_pending);
    CHECK(artifacts.recorded_calls().size() == 1);
    CHECK(std::ranges::count_if(service.recorded_calls(), [](const auto& call) {
            return std::holds_alternative<testing::VideoQueueCall>(call);
          }) == 1);
    CHECK(std::ranges::count_if(sessions.events(), [](const auto& durable) {
            return std::holds_alternative<domain::VideoJobQueued>(
                durable.payload);
          }) == 1);
    CHECK(std::ranges::count_if(sessions.events(), [](const auto& durable) {
            return std::holds_alternative<domain::VideoArtifactPublished>(
                durable.payload);
          }) == 1);
    const auto opened =
        sessions.open_session(id<domain::SessionId>("video-session"), {});
    REQUIRE(opened);
    CHECK(opened->run_count == 2);
  }
}

TEST_CASE("recovery resumes requested queued and processing video states") {
  const auto bytes = minimal_mp4();
  const auto artifact = metadata(bytes);
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  const auto run_from = [&](std::vector<domain::RunEventPayload> payloads,
                            testing::ScriptedVideoService service) {
    MemorySessionStore sessions;
    REQUIRE(sessions.create_session(
        {id<domain::SessionId>("video-session"),
         domain::EventTimestamp{std::chrono::milliseconds{1}}},
        {}));
    std::vector<domain::RunEvent> events;
    for (std::size_t index{}; index < payloads.size(); ++index)
      events.push_back(event(index + 1, std::move(payloads[index])));
    REQUIRE(sessions.append_events(id<domain::SessionId>("video-session"),
                                   events, {}));
    testing::ScriptedArtifactStore artifacts{
        {{{{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
            std::nullopt, std::nullopt},
           bytes},
          artifact}}};
    runtime::VideoCoordinator coordinator{
        sessions, artifacts, service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
    const auto result = coordinator.generate(
        session(runtime::VideoSessionMode::resume_run), operation);
    REQUIRE(result);
    CHECK_FALSE(result->cleanup_pending);
  };

  run_from(
      {attributes(),
       domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id}},
      testing::ScriptedVideoService{
          {{{operation.spec}, backend::VideoQuote{quote()}}},
          {{{operation.operation_id, operation.spec},
            backend::VideoQueued{job}}},
          {{{job, operation.spec.model_id},
            backend::VideoRetrieval{
                backend::VideoStatus{domain::VideoJobState::completed}}},
           {{job, operation.spec.model_id},
            backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
          {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}});
  run_from(
      {attributes(),
       domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id},
       domain::VideoQuoteObserved{operation.operation_id, quote()}},
      testing::ScriptedVideoService{
          {},
          {{{operation.operation_id, operation.spec},
            backend::VideoQueued{job}}},
          {{{job, operation.spec.model_id},
            backend::VideoRetrieval{
                backend::VideoStatus{domain::VideoJobState::completed}}},
           {{job, operation.spec.model_id},
            backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
          {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}});
  run_from(
      {attributes(),
       domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id},
       domain::VideoQuoteObserved{operation.operation_id, quote()},
       domain::VideoJobQueued{operation.operation_id, job}},
      testing::ScriptedVideoService{
          {},
          {},
          {{{job, operation.spec.model_id},
            backend::VideoRetrieval{
                backend::VideoStatus{domain::VideoJobState::completed}}},
           {{job, operation.spec.model_id},
            backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
          {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}});
  run_from(
      {attributes(),
       domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                        operation.artifact_id},
       domain::VideoQuoteObserved{operation.operation_id, quote()},
       domain::VideoJobQueued{operation.operation_id, job},
       domain::VideoJobStatusObserved{operation.operation_id, job, 1,
                                      domain::VideoJobState::processing}},
      testing::ScriptedVideoService{
          {},
          {},
          {{{job, operation.spec.model_id},
            backend::VideoRetrieval{
                backend::VideoStatus{domain::VideoJobState::completed}}},
           {{job, operation.spec.model_id},
            backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
          {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}});
}

TEST_CASE("published video survives cleanup failure and resumes once") {
  const auto bytes = minimal_mp4();
  const auto artifact = metadata(bytes);
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts{
      {{{{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
          std::nullopt, std::nullopt},
         bytes},
        artifact}}};
  testing::ScriptedVideoService first{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
      {{{job, operation.spec.model_id},
        backend::VideoRetrieval{
            backend::VideoStatus{domain::VideoJobState::processing}}},
       {{job, operation.spec.model_id},
        backend::VideoRetrieval{
            backend::VideoStatus{domain::VideoJobState::completed}}},
       {{job, operation.spec.model_id},
        backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
      {{{job, operation.spec.model_id},
        backend::VideoServiceError{backend::VideoServiceErrorCode::network,
                                   "must not persist", true, 503}}}};
  const auto now = [] {
    return domain::EventTimestamp{std::chrono::milliseconds{2}};
  };
  runtime::VideoCoordinator coordinator{sessions, artifacts, first, now};
  const auto generated = coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation);
  REQUIRE(generated);
  CHECK(generated->artifact == artifact);
  CHECK(generated->cleanup_pending);
  CHECK(artifacts.recorded_calls().size() == 1);
  CHECK(std::holds_alternative<domain::VideoCleanupFailed>(
      sessions.events()[sessions.events().size() - 1].payload));

  testing::ScriptedArtifactStore paused_artifacts;
  testing::ScriptedVideoService paused_service;
  runtime::VideoCoordinator cancelled_resume{sessions, paused_artifacts,
                                             paused_service, now};
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto paused_for_cancellation =
      cancelled_resume.generate(session(runtime::VideoSessionMode::resume_run),
                                operation, cancellation.get_token());
  REQUIRE(paused_for_cancellation);
  CHECK(paused_for_cancellation->cleanup_pending);
  CHECK(paused_service.recorded_calls().empty());

  runtime::VideoCoordinator expired_resume{
      sessions,
      paused_artifacts,
      paused_service,
      [] { return domain::EventTimestamp{std::chrono::hours{1}}; },
      {.total_timeout = std::chrono::milliseconds{10}}};
  const auto paused_for_deadline = expired_resume.generate(
      session(runtime::VideoSessionMode::resume_run), operation);
  REQUIRE(paused_for_deadline);
  CHECK(paused_for_deadline->cleanup_pending);
  CHECK(paused_service.recorded_calls().empty());

  testing::ScriptedArtifactStore no_artifact_calls;
  testing::ScriptedVideoService retry{
      {},
      {},
      {},
      {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}};
  runtime::VideoCoordinator resumed{sessions, no_artifact_calls, retry, now};
  const auto completed = resumed.generate(
      session(runtime::VideoSessionMode::resume_run), operation);
  REQUIRE(completed);
  CHECK_FALSE(completed->cleanup_pending);
  CHECK(no_artifact_calls.recorded_calls().empty());
  REQUIRE(retry.recorded_calls().size() == 1);
  CHECK(std::holds_alternative<testing::VideoCleanupCall>(
      retry.recorded_calls().front()));

  testing::ScriptedVideoService replay_service;
  runtime::VideoCoordinator replayed{sessions, no_artifact_calls,
                                     replay_service, now};
  const auto replay_result = replayed.generate(
      session(runtime::VideoSessionMode::resume_run), operation);
  REQUIRE(replay_result);
  CHECK(replay_service.recorded_calls().empty());
  CHECK(no_artifact_calls.recorded_calls().empty());
}

TEST_CASE("cleanup-pending recovery pauses on cancellation and deadline") {
  const auto bytes = minimal_mp4();
  const auto artifact = metadata(bytes);
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  MemorySessionStore sessions;
  REQUIRE(sessions.create_session(
      {id<domain::SessionId>("video-session"),
       domain::EventTimestamp{std::chrono::milliseconds{1}}},
      {}));
  const std::array events{
      event(1, attributes()),
      event(2, domain::VideoGenerationRequested{operation.operation_id,
                                                operation.spec,
                                                operation.artifact_id}),
      event(3, domain::VideoQuoteObserved{operation.operation_id, quote()}),
      event(4, domain::VideoJobQueued{operation.operation_id, job}),
      event(5,
            domain::VideoJobStatusObserved{operation.operation_id, job, 1,
                                           domain::VideoJobState::completed}),
      event(6, domain::ArtifactCreated{artifact}),
      event(7, domain::VideoArtifactPublished{operation.operation_id, job,
                                              artifact}),
      event(8, domain::VideoCleanupPending{operation.operation_id, job, 1})};
  REQUIRE(sessions.append_events(id<domain::SessionId>("video-session"), events,
                                 {}));
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service;
  runtime::VideoCoordinator cancelled{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto cancelled_result =
      cancelled.generate(session(runtime::VideoSessionMode::resume_run),
                         operation, cancellation.get_token());
  REQUIRE(cancelled_result);
  CHECK(cancelled_result->cleanup_pending);
  CHECK(sessions.events().size() == events.size());

  runtime::VideoCoordinator expired{
      sessions,
      artifacts,
      service,
      [] { return domain::EventTimestamp{std::chrono::hours{1}}; },
      {.total_timeout = std::chrono::milliseconds{10}}};
  const auto expired_result = expired.generate(
      session(runtime::VideoSessionMode::resume_run), operation);
  REQUIRE(expired_result);
  CHECK(expired_result->cleanup_pending);
  CHECK(sessions.events().size() == events.size());
  CHECK(service.recorded_calls().empty());
}

TEST_CASE("video media before completed status fails without publication") {
  const auto bytes = minimal_mp4();
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
      {{{job, operation.spec.model_id},
        backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}}};
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  const auto generated = coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation);
  REQUIRE_FALSE(generated);
  CHECK(generated.error().code ==
        runtime::VideoCoordinatorErrorCode::protocol_failure);
  CHECK(artifacts.recorded_calls().empty());
  CHECK(std::holds_alternative<domain::RunFailed>(
      sessions.events().back().payload));
}

TEST_CASE("invalid provider video status becomes a fixed terminal failure") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  testing::ScriptedVideoService service{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
      {{{job, operation.spec.model_id},
        backend::VideoRetrieval{
            backend::VideoStatus{static_cast<domain::VideoJobState>(
                std::numeric_limits<unsigned char>::max())}}}}};
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts;
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};

  const auto result = coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation);

  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::VideoCoordinatorErrorCode::protocol_failure);
  REQUIRE(std::holds_alternative<domain::RunFailed>(
      sessions.events().back().payload));
  CHECK(std::get<domain::RunFailed>(sessions.events().back().payload)
            .error.message == "video provider status was invalid");
  CHECK(std::ranges::none_of(sessions.events(), [](const auto& durable) {
    return std::holds_alternative<domain::VideoJobStatusObserved>(
        durable.payload);
  }));
  CHECK(artifacts.recorded_calls().empty());
}

TEST_CASE("malformed MP4 and artifact failures fail before publication") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  const auto run_case = [&](std::vector<std::byte> bytes,
                            testing::ScriptedArtifactStore artifacts) {
    MemorySessionStore sessions;
    testing::ScriptedVideoService service{
        {{{operation.spec}, backend::VideoQuote{quote()}}},
        {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
        {{{job, operation.spec.model_id},
          backend::VideoRetrieval{
              backend::VideoStatus{domain::VideoJobState::completed}}},
         {{job, operation.spec.model_id},
          backend::VideoRetrieval{
              backend::VideoMedia{std::move(bytes), "video/mp4"}}}}};
    runtime::VideoCoordinator coordinator{
        sessions, artifacts, service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
    const auto result = coordinator.generate(
        session(runtime::VideoSessionMode::create_session), operation);
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::VideoCoordinatorErrorCode::artifact_failure);
    CHECK(std::holds_alternative<domain::RunFailed>(
        sessions.events().back().payload));
    CHECK(std::ranges::none_of(sessions.events(), [](const auto& durable) {
      return std::holds_alternative<domain::VideoArtifactPublished>(
          durable.payload);
    }));
    return artifacts.recorded_calls().size();
  };

  CHECK(run_case({std::byte{1}}, testing::ScriptedArtifactStore{}) == 0);

  const auto bytes = minimal_mp4();
  {
    MemorySessionStore sessions;
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service{
        {{{operation.spec}, backend::VideoQuote{quote()}}},
        {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
        {{{job, operation.spec.model_id},
          backend::VideoRetrieval{
              backend::VideoStatus{domain::VideoJobState::completed}}},
         {{job, operation.spec.model_id},
          backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}}};
    auto limits = runtime::VideoCoordinatorLimits{};
    limits.mp4.maximum_bytes = bytes.size() - 1;
    runtime::VideoCoordinator coordinator{
        sessions, artifacts, service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; },
        limits};
    const auto oversized = coordinator.generate(
        session(runtime::VideoSessionMode::create_session), operation);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code ==
          runtime::VideoCoordinatorErrorCode::provider_failure);
    CHECK(artifacts.recorded_calls().empty());
  }

  testing::ScriptedArtifactStore rejected_store{
      {{{{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
          std::nullopt, std::nullopt},
         bytes},
        storage::ArtifactStoreError{storage::ArtifactStoreErrorCode::io_failure,
                                    "private path", true}}}};
  CHECK(run_case(bytes, std::move(rejected_store)) == 1);

  auto invalid_metadata = metadata(bytes);
  ++invalid_metadata.byte_size;
  testing::ScriptedArtifactStore invalid_store{
      {{{{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
          std::nullopt, std::nullopt},
         bytes},
        invalid_metadata}}};
  CHECK(run_case(bytes, std::move(invalid_store)) == 1);
}

TEST_CASE("publication append crash recovers without duplicate durable facts") {
  const auto bytes = minimal_mp4();
  const auto artifact = metadata(bytes);
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  MemorySessionStore sessions;
  sessions.fail_append_call = 5;
  const testing::ArtifactStoreExchange publication{
      {{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
        std::nullopt, std::nullopt},
       bytes},
      artifact};
  testing::ScriptedArtifactStore artifacts{{publication, publication}};
  testing::ScriptedVideoService service{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
      {{{job, operation.spec.model_id},
        backend::VideoRetrieval{
            backend::VideoStatus{domain::VideoJobState::completed}}},
       {{job, operation.spec.model_id},
        backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}},
       {{job, operation.spec.model_id},
        backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
      {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}};
  const auto now = [] {
    return domain::EventTimestamp{std::chrono::milliseconds{2}};
  };
  runtime::VideoCoordinator coordinator{sessions, artifacts, service, now};
  const auto interrupted = coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation);
  REQUIRE_FALSE(interrupted);
  CHECK(interrupted.error().code ==
        runtime::VideoCoordinatorErrorCode::storage_failure);
  const auto recovered = coordinator.generate(
      session(runtime::VideoSessionMode::resume_run), operation);
  REQUIRE(recovered);
  CHECK_FALSE(recovered->cleanup_pending);
  CHECK(artifacts.recorded_calls().size() == 2);
  std::size_t publications{};
  for (const auto& durable : sessions.events()) {
    publications +=
        std::holds_alternative<domain::VideoArtifactPublished>(durable.payload);
  }
  CHECK(publications == 1);
  REQUIRE(service.recorded_calls().size() == service.recorded_options().size());
  for (std::size_t index{}; index < service.recorded_calls().size(); ++index) {
    if (std::holds_alternative<testing::VideoRetrieveCall>(
            service.recorded_calls()[index])) {
      CHECK(service.recorded_options()[index].maximum_response_bytes ==
            runtime::VideoCoordinatorLimits{}.mp4.maximum_bytes);
    }
  }
}

TEST_CASE("completed cleanup is idempotent across terminal append failure") {
  const auto bytes = minimal_mp4();
  const auto artifact = metadata(bytes);
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  MemorySessionStore sessions;
  sessions.fail_append_call = 6;
  testing::ScriptedArtifactStore artifacts{
      {{{{operation.artifact_id, "video/mp4", std::nullopt, std::nullopt,
          std::nullopt, std::nullopt},
         bytes},
        artifact}}};
  testing::ScriptedVideoService service{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
      {{{job, operation.spec.model_id},
        backend::VideoRetrieval{
            backend::VideoStatus{domain::VideoJobState::completed}}},
       {{job, operation.spec.model_id},
        backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}},
      {{{job, operation.spec.model_id}, backend::VideoCleanup{true}}}};
  const auto now = [] {
    return domain::EventTimestamp{std::chrono::milliseconds{2}};
  };
  runtime::VideoCoordinator coordinator{sessions, artifacts, service, now};
  const auto interrupted = coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation);
  REQUIRE_FALSE(interrupted);
  CHECK(interrupted.error().code ==
        runtime::VideoCoordinatorErrorCode::storage_failure);
  const auto recovered = coordinator.generate(
      session(runtime::VideoSessionMode::resume_run), operation);
  REQUIRE(recovered);
  CHECK_FALSE(recovered->cleanup_pending);
  std::size_t cleanup_calls{};
  for (const auto& call : service.recorded_calls())
    cleanup_calls += std::holds_alternative<testing::VideoCleanupCall>(call);
  CHECK(cleanup_calls == 2);
}

TEST_CASE("poll exhaustion and provider cancellation are ordinary terminals") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  {
    MemorySessionStore sessions;
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service{
        {{{operation.spec}, backend::VideoQuote{quote()}}},
        {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
        {{{job, operation.spec.model_id},
          backend::VideoRetrieval{
              backend::VideoStatus{domain::VideoJobState::queued}}}}};
    runtime::VideoCoordinator coordinator{
        sessions,
        artifacts,
        service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; },
        {.maximum_polls = 1}};
    const auto exhausted = coordinator.generate(
        session(runtime::VideoSessionMode::create_session), operation);
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error().code ==
          runtime::VideoCoordinatorErrorCode::poll_exhausted);
    CHECK(std::holds_alternative<domain::RunFailed>(
        sessions.events().back().payload));
  }
  {
    MemorySessionStore sessions;
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service{
        {{{operation.spec},
          backend::VideoServiceError{backend::VideoServiceErrorCode::cancelled,
                                     "provider secret must not persist", false,
                                     std::nullopt}}}};
    runtime::VideoCoordinator coordinator{
        sessions, artifacts, service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
    const auto cancelled = coordinator.generate(
        session(runtime::VideoSessionMode::create_session), operation);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code ==
          runtime::VideoCoordinatorErrorCode::cancelled);
    CHECK(std::holds_alternative<domain::RunCancelled>(
        sessions.events().back().payload));
  }
}

TEST_CASE("provider errors at each prepublication stage are fixed terminals") {
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  const backend::VideoServiceError provider_error{
      backend::VideoServiceErrorCode::network,
      "private provider diagnostic must not persist", true, 503};
  const auto require_generation_failure =
      [&](testing::ScriptedVideoService service) {
        MemorySessionStore sessions;
        testing::ScriptedArtifactStore artifacts;
        runtime::VideoCoordinator coordinator{
            sessions, artifacts, service, [] {
              return domain::EventTimestamp{std::chrono::milliseconds{2}};
            }};
        const auto result = coordinator.generate(
            session(runtime::VideoSessionMode::create_session), operation);
        REQUIRE_FALSE(result);
        CHECK(result.error().code ==
              runtime::VideoCoordinatorErrorCode::provider_failure);
        REQUIRE(std::holds_alternative<domain::RunFailed>(
            sessions.events().back().payload));
        CHECK(std::get<domain::RunFailed>(sessions.events().back().payload)
                  .error.message.find("private provider") == std::string::npos);
      };

  require_generation_failure(
      testing::ScriptedVideoService{{{{operation.spec}, provider_error}}});
  require_generation_failure(testing::ScriptedVideoService{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, provider_error}}});
  require_generation_failure(testing::ScriptedVideoService{
      {{{operation.spec}, backend::VideoQuote{quote()}}},
      {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
      {{{job, operation.spec.model_id}, provider_error}}});

  const auto transcription_id = id<domain::VideoOperationId>("transcription");
  const auto model_id = id<domain::ModelId>("transcription-model");
  auto url = backend::TransientVideoUrl::from("https://example.test/private");
  REQUIRE(url);
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {}, {}, {}, {}, {{{transcription_id, model_id}, provider_error}}};
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  const auto result =
      coordinator.transcribe(session(runtime::VideoSessionMode::create_session),
                             {id<domain::RunId>("video-run"), attributes(),
                              transcription_id, model_id, std::move(*url)});
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::VideoCoordinatorErrorCode::provider_failure);
  REQUIRE(std::holds_alternative<domain::RunFailed>(
      sessions.events().back().payload));
  CHECK(std::get<domain::RunFailed>(sessions.events().back().payload)
            .error.message.find("private provider") == std::string::npos);
}

TEST_CASE("caller cancellation closes every paid video boundary") {
  const auto bytes = minimal_mp4();
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  for (const std::size_t cancel_after :
       {std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
    CAPTURE(cancel_after);
    MemorySessionStore sessions;
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service{
        {{{operation.spec}, backend::VideoQuote{quote()}}},
        {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
        {{{job, operation.spec.model_id},
          backend::VideoRetrieval{
              backend::VideoStatus{domain::VideoJobState::completed}}},
         {{job, operation.spec.model_id},
          backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}}};
    std::stop_source cancellation;
    service.request_stop_after_call(cancellation, cancel_after);
    runtime::VideoCoordinator coordinator{
        sessions, artifacts, service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
    const auto result =
        coordinator.generate(session(runtime::VideoSessionMode::create_session),
                             operation, cancellation.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == runtime::VideoCoordinatorErrorCode::cancelled);
    CHECK(std::holds_alternative<domain::RunCancelled>(
        sessions.events().back().payload));
    CHECK(artifacts.recorded_calls().empty());
  }

  const auto transcription_id = id<domain::VideoOperationId>("transcription");
  const auto model = id<domain::ModelId>("model");
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {},
      {},
      {},
      {},
      {{{transcription_id, model},
        backend::VideoTranscription{"text", std::nullopt}}}};
  std::stop_source cancellation;
  service.request_stop_after_call(cancellation, 1);
  auto url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  const auto result =
      coordinator.transcribe(session(runtime::VideoSessionMode::create_session),
                             {id<domain::RunId>("video-run"), attributes(),
                              transcription_id, model, std::move(*url)},
                             cancellation.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::VideoCoordinatorErrorCode::cancelled);
  CHECK(std::holds_alternative<domain::RunCancelled>(
      sessions.events().back().payload));
}

TEST_CASE("total deadline fails before a provider call") {
  const auto operation = generation();
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service;
  std::uint32_t clock_calls{};
  runtime::VideoCoordinator coordinator{
      sessions,
      artifacts,
      service,
      [&] {
        ++clock_calls;
        return domain::EventTimestamp{
            std::chrono::milliseconds{clock_calls <= 2 ? 1 : 1000}};
      },
      {.maximum_polls = 1, .total_timeout = std::chrono::milliseconds{10}}};
  const auto expired = coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation);
  REQUIRE_FALSE(expired);
  CHECK(expired.error().code ==
        runtime::VideoCoordinatorErrorCode::deadline_exceeded);
  CHECK(service.recorded_calls().empty());
  CHECK(std::holds_alternative<domain::RunFailed>(
      sessions.events().back().payload));
}

TEST_CASE(
    "total deadline fails closed for clock regression and integer bounds") {
  const auto operation = generation();
  for (const auto& [started_at, now] :
       {std::pair{std::chrono::milliseconds{10}, std::chrono::milliseconds{9}},
        std::pair{
            std::chrono::milliseconds{
                std::numeric_limits<std::chrono::milliseconds::rep>::min()},
            std::chrono::milliseconds{
                std::numeric_limits<std::chrono::milliseconds::rep>::max()}}}) {
    CAPTURE(started_at.count(), now.count());
    MemorySessionStore sessions;
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service;
    std::uint32_t clock_calls{};
    runtime::VideoCoordinator coordinator{
        sessions,
        artifacts,
        service,
        [&] {
          ++clock_calls;
          return domain::EventTimestamp{clock_calls <= 2 ? started_at : now};
        },
        {.total_timeout = std::chrono::milliseconds{10}}};

    const auto result = coordinator.generate(
        session(runtime::VideoSessionMode::create_session), operation);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::VideoCoordinatorErrorCode::deadline_exceeded);
    CHECK(service.recorded_calls().empty());
    CHECK(std::holds_alternative<domain::RunFailed>(
        sessions.events().back().payload));
  }
}

TEST_CASE("one clock sample authoritatively bounds each provider call") {
  const auto operation = generation();
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {{{operation.spec},
        backend::VideoServiceError{backend::VideoServiceErrorCode::network,
                                   "fixed", true, std::nullopt}}}};
  std::uint32_t clock_calls{};
  runtime::VideoCoordinator coordinator{
      sessions,
      artifacts,
      service,
      [&] {
        ++clock_calls;
        return domain::EventTimestamp{std::chrono::milliseconds{
            clock_calls <= 2 ? 0 : (clock_calls == 3 ? 9 : 10)}};
      },
      {.total_timeout = std::chrono::milliseconds{10}}};
  CHECK_FALSE(coordinator.generate(
      session(runtime::VideoSessionMode::create_session), operation));
  CHECK(clock_calls == 4);
  REQUIRE(service.recorded_options().size() == 1);
  CHECK(service.recorded_options().front().timeout ==
        std::chrono::milliseconds{1});
}

TEST_CASE("deadline closes queue retrieval publication and transcription") {
  const auto bytes = minimal_mp4();
  const auto operation = generation();
  const auto job = id<domain::VideoJobId>("job");
  for (const std::uint32_t expire_at : {5U, 7U, 10U}) {
    CAPTURE(expire_at);
    MemorySessionStore sessions;
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service{
        {{{operation.spec}, backend::VideoQuote{quote()}}},
        {{{operation.operation_id, operation.spec}, backend::VideoQueued{job}}},
        {{{job, operation.spec.model_id},
          backend::VideoRetrieval{
              backend::VideoStatus{domain::VideoJobState::completed}}},
         {{job, operation.spec.model_id},
          backend::VideoRetrieval{backend::VideoMedia{bytes, "video/mp4"}}}}};
    std::uint32_t clock_calls{};
    runtime::VideoCoordinator coordinator{
        sessions,
        artifacts,
        service,
        [&] {
          ++clock_calls;
          return domain::EventTimestamp{
              std::chrono::milliseconds{clock_calls >= expire_at ? 20 : 1}};
        },
        {.total_timeout = std::chrono::milliseconds{10}}};
    const auto result = coordinator.generate(
        session(runtime::VideoSessionMode::create_session), operation);
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::VideoCoordinatorErrorCode::deadline_exceeded);
    CHECK(artifacts.recorded_calls().empty());
  }

  const auto transcription_id = id<domain::VideoOperationId>("transcription");
  const auto model = id<domain::ModelId>("model");
  MemorySessionStore sessions;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {},
      {},
      {},
      {},
      {{{transcription_id, model},
        backend::VideoTranscription{"text", std::nullopt}}}};
  std::uint32_t clock_calls{};
  runtime::VideoCoordinator coordinator{
      sessions,
      artifacts,
      service,
      [&] {
        ++clock_calls;
        return domain::EventTimestamp{
            std::chrono::milliseconds{clock_calls >= 4 ? 20 : 1}};
      },
      {.total_timeout = std::chrono::milliseconds{10}}};
  auto url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);
  const auto result =
      coordinator.transcribe(session(runtime::VideoSessionMode::create_session),
                             {id<domain::RunId>("video-run"), attributes(),
                              transcription_id, model, std::move(*url)});
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::VideoCoordinatorErrorCode::deadline_exceeded);
}

TEST_CASE("transcription recovery requires resupply and never captures URL") {
  const auto operation_id = id<domain::VideoOperationId>("transcription");
  const auto model_id = id<domain::ModelId>("transcription-model");
  auto secret = backend::TransientVideoUrl::from(
      "https://example.test/private?secret=do-not-store");
  REQUIRE(secret);
  MemorySessionStore sessions;
  REQUIRE(sessions.create_session(
      {id<domain::SessionId>("video-session"),
       domain::EventTimestamp{std::chrono::milliseconds{1}}},
      {}));
  const std::array requested_events{
      event(1, attributes()),
      event(2, domain::VideoTranscriptionRequested{operation_id, model_id})};
  REQUIRE(sessions.append_events(id<domain::SessionId>("video-session"),
                                 requested_events, {}));
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {},
      {},
      {},
      {},
      {{{operation_id, model_id},
        backend::VideoTranscription{"transcribed", std::string{"en"}}}}};
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  const auto missing =
      coordinator.transcribe(session(runtime::VideoSessionMode::resume_run),
                             {id<domain::RunId>("video-run"), attributes(),
                              operation_id, model_id, std::nullopt});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code ==
        runtime::VideoCoordinatorErrorCode::invalid_request);
  CHECK(service.recorded_calls().empty());

  const auto transcribed =
      coordinator.transcribe(session(runtime::VideoSessionMode::resume_run),
                             {id<domain::RunId>("video-run"), attributes(),
                              operation_id, model_id, std::move(*secret)});
  REQUIRE(transcribed);
  CHECK(transcribed->text == "transcribed");
  REQUIRE(service.recorded_calls().size() == 1);
  CHECK(std::holds_alternative<testing::VideoTranscriptionCall>(
      service.recorded_calls().front()));
  for (const auto& durable : sessions.events()) {
    if (const auto* observed =
            std::get_if<domain::VideoTranscriptionObserved>(&durable.payload)) {
      CHECK(observed->text.find("do-not-store") == std::string::npos);
      CHECK((!observed->language ||
             observed->language->find("do-not-store") == std::string::npos));
    }
    if (const auto* failed = std::get_if<domain::RunFailed>(&durable.payload)) {
      CHECK(failed->error.message.find("do-not-store") == std::string::npos);
    }
  }

  testing::ScriptedVideoService no_calls;
  runtime::VideoCoordinator replayed{
      sessions, artifacts, no_calls,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  const auto recovered =
      replayed.transcribe(session(runtime::VideoSessionMode::resume_run),
                          {id<domain::RunId>("video-run"), attributes(),
                           operation_id, model_id, std::nullopt});
  REQUIRE(recovered);
  CHECK(no_calls.recorded_calls().empty());
}

TEST_CASE("transcription rejects a transient URL echoed in provider output") {
  const auto operation_id = id<domain::VideoOperationId>("transcription");
  const auto model_id = id<domain::ModelId>("transcription-model");
  const std::string secret{"https://example.test/private?secret=never-persist"};
  for (const bool echo_in_language : {false, true}) {
    CAPTURE(echo_in_language);
    MemorySessionStore sessions;
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service{
        {},
        {},
        {},
        {},
        {{{operation_id, model_id},
          backend::VideoTranscription{
              echo_in_language ? "safe text" : "prefix " + secret,
              echo_in_language ? std::optional<std::string>{"tag-" + secret}
                               : std::optional<std::string>{"en"}}}}};
    auto url = backend::TransientVideoUrl::from(secret);
    REQUIRE(url);
    runtime::VideoCoordinator coordinator{
        sessions, artifacts, service,
        [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
    const auto result = coordinator.transcribe(
        session(runtime::VideoSessionMode::create_session),
        {id<domain::RunId>("video-run"), attributes(), operation_id, model_id,
         std::move(*url)});
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::VideoCoordinatorErrorCode::protocol_failure);
    CHECK(std::holds_alternative<domain::RunFailed>(
        sessions.events().back().payload));
    CHECK(std::get<domain::RunFailed>(sessions.events().back().payload)
              .error.message.find(secret) == std::string::npos);
    CHECK(std::ranges::none_of(sessions.events(), [](const auto& durable) {
      return std::holds_alternative<domain::VideoTranscriptionObserved>(
          durable.payload);
    }));
    REQUIRE(service.recorded_options().size() == 1);
    CHECK(service.recorded_options().front().maximum_response_bytes ==
          domain::maximum_video_transcription_response_bytes);
  }
}

TEST_CASE("video replay projects one interleaved run without side effects") {
  const auto session_id = id<domain::SessionId>("video-session");
  const auto first_run = id<domain::RunId>("first-video-run");
  const auto second_run = id<domain::RunId>("second-video-run");
  const auto first_operation = id<domain::VideoOperationId>("first-operation");
  const auto second_operation =
      id<domain::VideoOperationId>("second-operation");
  const auto model_id = id<domain::ModelId>("transcription-model");
  MemorySessionStore sessions;
  REQUIRE(sessions.create_session(
      {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}}, {}));
  const std::array events{
      event_for(1, first_run, attributes()),
      event_for(2, second_run, attributes()),
      event_for(3, first_run,
                domain::VideoTranscriptionRequested{first_operation, model_id}),
      event_for(
          4, second_run,
          domain::VideoTranscriptionRequested{second_operation, model_id}),
      event_for(5, first_run,
                domain::VideoTranscriptionObserved{
                    first_operation, "first transcript", std::string{"en"}}),
      event_for(6, second_run,
                domain::VideoTranscriptionObserved{
                    second_operation, "second transcript", std::string{"fr"}}),
      event_for(7, first_run, domain::RunCompleted{}),
      event_for(8, second_run, domain::RunCompleted{})};
  REQUIRE(sessions.append_events(session_id, events, {}));
  const auto append_calls = sessions.append_calls;
  const auto replay_calls = sessions.replay_calls;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service;
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};

  const auto replayed = coordinator.replay(session_id, first_run);

  REQUIRE(replayed);
  REQUIRE(replayed->run_id());
  CHECK(*replayed->run_id() == first_run);
  CHECK(replayed->state() == domain::VideoLifecycleState::completed);
  REQUIRE(replayed->transcription());
  CHECK(replayed->transcription()->text == "first transcript");
  CHECK(replayed->last_sequence() == 7);
  CHECK(sessions.append_calls == append_calls);
  CHECK(sessions.replay_calls == replay_calls + 1);
  CHECK(service.recorded_calls().empty());
  CHECK(artifacts.recorded_calls().empty());
  const auto opened = sessions.open_session(session_id, {});
  REQUIRE(opened);
  CHECK(opened->run_count == 2);
}

TEST_CASE("video replay rejects a missing target and invalid full session") {
  const auto session_id = id<domain::SessionId>("video-session");
  const auto target_run = id<domain::RunId>("target-video-run");
  const auto missing_run = id<domain::RunId>("missing-video-run");
  const auto other_run = id<domain::RunId>("other-video-run");
  const auto operation_id = id<domain::VideoOperationId>("operation");
  const auto model_id = id<domain::ModelId>("transcription-model");
  const auto completed_history = [&] {
    return std::vector{
        event_for(1, target_run, attributes()),
        event_for(2, target_run,
                  domain::VideoTranscriptionRequested{operation_id, model_id}),
        event_for(3, target_run,
                  domain::VideoTranscriptionObserved{operation_id, "text",
                                                     std::nullopt}),
        event_for(4, target_run, domain::RunCompleted{})};
  };

  {
    MemorySessionStore sessions;
    REQUIRE(sessions.create_session(
        {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}},
        {}));
    const auto events = completed_history();
    REQUIRE(sessions.append_events(session_id, events, {}));
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service;
    runtime::VideoCoordinator coordinator{sessions, artifacts, service};
    const auto missing = coordinator.replay(session_id, missing_run);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code ==
          runtime::VideoCoordinatorErrorCode::replay_rejected);
    CHECK(service.recorded_calls().empty());
    CHECK(artifacts.recorded_calls().empty());
  }

  {
    MemorySessionStore sessions;
    REQUIRE(sessions.create_session(
        {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}},
        {}));
    auto events = completed_history();
    auto invalid = event_for(5, other_run, attributes());
    invalid.metadata.event_id = events.front().metadata.event_id;
    events.push_back(std::move(invalid));
    REQUIRE(sessions.append_events(session_id, events, {}));
    testing::ScriptedArtifactStore artifacts;
    testing::ScriptedVideoService service;
    runtime::VideoCoordinator coordinator{sessions, artifacts, service};
    const auto replayed = coordinator.replay(session_id, target_run);
    REQUIRE_FALSE(replayed);
    CHECK(replayed.error().code ==
          runtime::VideoCoordinatorErrorCode::replay_rejected);
    CHECK(service.recorded_calls().empty());
    CHECK(artifacts.recorded_calls().empty());
  }
}

TEST_CASE("starting a video run rejects any already-used target") {
  const auto session_id = id<domain::SessionId>("video-session");
  const auto run_id = id<domain::RunId>("used-video-run");
  const auto operation_id = id<domain::VideoOperationId>("new-operation");
  const auto model_id = id<domain::ModelId>("transcription-model");
  MemorySessionStore sessions;
  REQUIRE(sessions.create_session(
      {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}}, {}));
  const std::array events{event_for(
      1, run_id,
      domain::UnknownEvent{"future.video", {"application/json", "{}"}})};
  REQUIRE(sessions.append_events(session_id, events, {}));
  const auto append_calls = sessions.append_calls;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service;
  runtime::VideoCoordinator coordinator{sessions, artifacts, service};
  auto url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);

  const auto result = coordinator.transcribe(
      {session_id, runtime::VideoSessionMode::start_run,
       domain::EventTimestamp{std::chrono::milliseconds{2}}},
      {run_id, attributes(), operation_id, model_id, std::move(*url)});

  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::VideoCoordinatorErrorCode::replay_rejected);
  CHECK(sessions.append_calls == append_calls);
  CHECK(service.recorded_calls().empty());
  CHECK(artifacts.recorded_calls().empty());
}

TEST_CASE("an existing durable session can start a second video run") {
  const auto session_id = id<domain::SessionId>("video-session");
  const auto first_run = id<domain::RunId>("first-video-run");
  const auto second_run = id<domain::RunId>("second-video-run");
  const auto first_operation = id<domain::VideoOperationId>("first-operation");
  const auto second_operation =
      id<domain::VideoOperationId>("second-operation");
  const auto model_id = id<domain::ModelId>("transcription-model");
  MemorySessionStore sessions;
  REQUIRE(sessions.create_session(
      {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}}, {}));
  const std::array first_events{
      event_for(1, first_run, attributes()),
      event_for(2, first_run,
                domain::VideoTranscriptionRequested{first_operation, model_id}),
      event_for(3, first_run,
                domain::VideoTranscriptionObserved{first_operation, "first",
                                                   std::nullopt}),
      event_for(4, first_run, domain::RunCompleted{})};
  REQUIRE(sessions.append_events(session_id, first_events, {}));
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service{
      {},
      {},
      {},
      {},
      {{{second_operation, model_id},
        backend::VideoTranscription{"second", std::string{"en"}}}}};
  runtime::VideoCoordinator coordinator{
      sessions, artifacts, service,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{2}}; }};
  auto url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);

  const auto result = coordinator.transcribe(
      {session_id, runtime::VideoSessionMode::start_run,
       domain::EventTimestamp{std::chrono::milliseconds{2}}},
      {second_run, attributes(), second_operation, model_id, std::move(*url)});

  REQUIRE(result);
  CHECK(result->text == "second");
  const auto opened = sessions.open_session(session_id, {});
  REQUIRE(opened);
  CHECK(opened->run_count == 2);
  REQUIRE(sessions.events().size() == 8);
  CHECK(sessions.events()[4].metadata.run_id == second_run);
  CHECK(
      std::holds_alternative<domain::RunStarted>(sessions.events()[4].payload));
  CHECK(std::holds_alternative<domain::VideoTranscriptionRequested>(
      sessions.events()[5].payload));
}

TEST_CASE("video operation identities cannot be reused across runs") {
  const auto session_id = id<domain::SessionId>("video-session");
  const auto first_run = id<domain::RunId>("first-video-run");
  const auto second_run = id<domain::RunId>("second-video-run");
  const auto operation_id = id<domain::VideoOperationId>("shared-operation");
  const auto model_id = id<domain::ModelId>("transcription-model");
  MemorySessionStore sessions;
  REQUIRE(sessions.create_session(
      {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}}, {}));
  const std::array first_events{
      event_for(1, first_run, attributes()),
      event_for(2, first_run,
                domain::VideoTranscriptionRequested{operation_id, model_id}),
      event_for(3, first_run,
                domain::VideoTranscriptionObserved{operation_id, "first",
                                                   std::nullopt}),
      event_for(4, first_run, domain::RunCompleted{})};
  REQUIRE(sessions.append_events(session_id, first_events, {}));
  const auto append_calls = sessions.append_calls;
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service;
  runtime::VideoCoordinator coordinator{sessions, artifacts, service};
  auto url = backend::TransientVideoUrl::from("https://example.test/video");
  REQUIRE(url);

  const auto result = coordinator.transcribe(
      {session_id, runtime::VideoSessionMode::start_run,
       domain::EventTimestamp{std::chrono::milliseconds{2}}},
      {second_run, attributes(), operation_id, model_id, std::move(*url)});

  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::VideoCoordinatorErrorCode::replay_rejected);
  CHECK(sessions.append_calls == append_calls);
  CHECK(service.recorded_calls().empty());
  CHECK(artifacts.recorded_calls().empty());
}

TEST_CASE("video ownership lookup handles a sizable interleaved session") {
  constexpr std::size_t unrelated_run_count{1024};
  const auto session_id = id<domain::SessionId>("large-video-session");
  const auto model_id = id<domain::ModelId>("transcription-model");
  MemorySessionStore sessions;
  REQUIRE(sessions.create_session(
      {session_id, domain::EventTimestamp{std::chrono::milliseconds{1}}}, {}));
  std::vector<domain::RunEvent> events;
  events.reserve(unrelated_run_count * 2 + 4);
  std::uint64_t sequence{1};
  for (std::size_t index{}; index < unrelated_run_count; ++index) {
    const auto run_id =
        id<domain::RunId>("interleaved-run-" + std::to_string(index));
    const auto operation_id = id<domain::VideoOperationId>(
        "interleaved-operation-" + std::to_string(index));
    events.push_back(event_for(sequence++, run_id, attributes()));
    events.push_back(
        event_for(sequence++, run_id,
                  domain::VideoTranscriptionRequested{operation_id, model_id}));
  }
  const auto target_run = id<domain::RunId>("target-video-run");
  const auto target_operation =
      id<domain::VideoOperationId>("target-video-operation");
  events.push_back(event_for(sequence++, target_run, attributes()));
  events.push_back(event_for(
      sequence++, target_run,
      domain::VideoTranscriptionRequested{target_operation, model_id}));
  events.push_back(
      event_for(sequence++, target_run,
                domain::VideoTranscriptionObserved{
                    target_operation, "target transcript", std::nullopt}));
  events.push_back(event_for(sequence, target_run, domain::RunCompleted{}));
  REQUIRE(sessions.append_events(session_id, events, {}));
  testing::ScriptedArtifactStore artifacts;
  testing::ScriptedVideoService service;
  runtime::VideoCoordinator coordinator{sessions, artifacts, service};

  const auto replayed = coordinator.replay(session_id, target_run);

  REQUIRE(replayed);
  CHECK(replayed->state() == domain::VideoLifecycleState::completed);
  REQUIRE(replayed->transcription());
  CHECK(replayed->transcription()->text == "target transcript");
  CHECK(service.recorded_calls().empty());
  CHECK(artifacts.recorded_calls().empty());
}

} // namespace
