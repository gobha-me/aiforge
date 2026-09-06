#include <aiforge/runtime/video_coordinator.hpp>

#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <aiforge/domain/event_log.hpp>

namespace aiforge::runtime {
namespace {

struct State {
  domain::SessionId session_id;
  domain::RunId target_run_id;
  std::optional<domain::VideoOperationId> operation_id;
  domain::SessionEventLog event_log;
  domain::RunProjection run;
  domain::VideoProjection video;
  bool target_history{};
};

[[nodiscard]] auto coordinator_error(const VideoCoordinatorErrorCode code,
                                     std::string message,
                                     const bool retryable = false)
    -> std::unexpected<VideoCoordinatorError> {
  return std::unexpected(
      VideoCoordinatorError{code, std::move(message), retryable});
}

[[nodiscard]] auto valid_limits(const VideoCoordinatorLimits& limits) -> bool {
  return limits.maximum_polls != 0 &&
         limits.total_timeout > std::chrono::milliseconds::zero() &&
         limits.mp4.maximum_bytes >= 8 && limits.mp4.maximum_boxes != 0 &&
         limits.mp4.maximum_nesting_depth != 0 &&
         limits.mp4.maximum_tracks != 0 &&
         limits.mp4.maximum_compatible_brands != 0;
}

[[nodiscard]] auto payload_operation_id(const domain::RunEventPayload& payload)
    -> const domain::VideoOperationId* {
  if (const auto* value =
          std::get_if<domain::VideoGenerationRequested>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value = std::get_if<domain::VideoQuoteObserved>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value = std::get_if<domain::VideoJobQueued>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value =
          std::get_if<domain::VideoJobStatusObserved>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value =
          std::get_if<domain::VideoArtifactPublished>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value = std::get_if<domain::VideoCleanupPending>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value =
          std::get_if<domain::VideoCleanupCompleted>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value = std::get_if<domain::VideoCleanupFailed>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value =
          std::get_if<domain::VideoTranscriptionRequested>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value =
          std::get_if<domain::VideoTranscriptionObserved>(&payload)) {
    return &value->operation_id;
  }
  return nullptr;
}

[[nodiscard]] auto requested_operation_id(
    const domain::RunEventPayload& payload) -> const domain::VideoOperationId* {
  if (const auto* value =
          std::get_if<domain::VideoGenerationRequested>(&payload)) {
    return &value->operation_id;
  }
  if (const auto* value =
          std::get_if<domain::VideoTranscriptionRequested>(&payload)) {
    return &value->operation_id;
  }
  return nullptr;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Replay checks.
[[nodiscard]] auto load_state(
    storage::SessionStore& store, const domain::SessionId& session_id,
    const domain::RunId& target_run_id,
    std::optional<domain::VideoOperationId> operation_id,
    const std::stop_token stop_token)
    -> std::expected<State, VideoCoordinatorError> {
  auto opened = store.open_session(session_id, stop_token);
  if (!opened) {
    return coordinator_error(VideoCoordinatorErrorCode::storage_failure,
                             "video session could not be opened",
                             opened.error().retryable);
  }
  auto events = store.replay_events(session_id, stop_token);
  if (!events) {
    return coordinator_error(VideoCoordinatorErrorCode::storage_failure,
                             "video session could not be replayed",
                             events.error().retryable);
  }
  State state{session_id,
              target_run_id,
              std::move(operation_id),
              domain::SessionEventLog{session_id},
              {},
              {},
              false};
  std::map<domain::VideoOperationId, domain::RunId> operation_owners;
  for (const auto& event : *events) {
    if (!state.event_log.append(event)) {
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "durable video history is invalid");
    }
    if (const auto* requested = requested_operation_id(event.payload)) {
      const auto owner = operation_owners.find(*requested);
      if (owner != operation_owners.end() &&
          owner->second != event.metadata.run_id) {
        return coordinator_error(
            VideoCoordinatorErrorCode::replay_rejected,
            "video operation identity belongs to multiple runs");
      }
      if (owner == operation_owners.end()) {
        operation_owners.emplace(*requested, event.metadata.run_id);
      }
    }
    if (event.metadata.run_id != target_run_id) {
      const auto* observed_operation = payload_operation_id(event.payload);
      if (state.operation_id && observed_operation != nullptr &&
          *observed_operation == *state.operation_id) {
        return coordinator_error(
            VideoCoordinatorErrorCode::replay_rejected,
            "video operation identity belongs to another run");
      }
      continue;
    }
    state.target_history = true;
    if (!state.run.apply(event)) {
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "durable video run history is invalid");
    }
  }
  if (state.event_log.last_sequence() != opened->last_sequence) {
    return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                             "durable video sequence is inconsistent");
  }
  if (state.target_history) {
    auto video =
        domain::VideoProjection::rebuild(state.event_log, target_run_id);
    if (!video) {
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "durable video run history is invalid");
    }
    state.video = std::move(*video);
  }
  return state;
}

[[nodiscard]] auto make_event(const State& state, const domain::RunId& run_id,
                              domain::RunEventPayload payload,
                              const VideoTimestampSource& timestamp)
    -> std::expected<domain::RunEvent, VideoCoordinatorError> {
  if (state.event_log.last_sequence() ==
      std::numeric_limits<std::uint64_t>::max()) {
    return coordinator_error(VideoCoordinatorErrorCode::storage_failure,
                             "video event sequence overflowed");
  }
  const auto sequence = state.event_log.last_sequence() + 1;
  auto event_id = domain::EventId::from("event-" + std::to_string(sequence));
  if (!event_id) {
    return coordinator_error(VideoCoordinatorErrorCode::internal_failure,
                             "video event identity could not be created");
  }
  return domain::RunEvent{{*event_id, run_id, sequence, 1, timestamp(),
                           std::nullopt, std::nullopt, std::nullopt},
                          std::move(payload)};
}

[[nodiscard]] auto append(storage::SessionStore& store, State& state,
                          const domain::RunId& run_id,
                          std::vector<domain::RunEventPayload> payloads,
                          const VideoTimestampSource& timestamp,
                          const std::stop_token stop_token)
    -> std::expected<void, VideoCoordinatorError> {
  constexpr std::size_t maximum_conflict_reloads{8};
  for (std::size_t attempt{}; attempt <= maximum_conflict_reloads; ++attempt) {
    auto log = state.event_log;
    auto run = state.run;
    auto video = state.video;
    std::vector<domain::RunEvent> events;
    events.reserve(payloads.size());
    for (const auto& payload : payloads) {
      State candidate{state.session_id,
                      state.target_run_id,
                      state.operation_id,
                      log,
                      run,
                      video,
                      state.target_history};
      auto event = make_event(candidate, run_id, payload, timestamp);
      if (!event || !log.append(*event) || !run.apply(*event) ||
          !video.apply(*event)) {
        return coordinator_error(VideoCoordinatorErrorCode::protocol_failure,
                                 "video state transition was rejected");
      }
      events.push_back(std::move(*event));
    }
    auto stored = store.append_events(state.session_id, events, stop_token);
    if (stored) {
      state.event_log = std::move(log);
      state.run = std::move(run);
      state.video = std::move(video);
      state.target_history = true;
      return {};
    }
    if (stored.error().code != storage::SessionStoreErrorCode::conflict ||
        attempt == maximum_conflict_reloads) {
      return coordinator_error(VideoCoordinatorErrorCode::storage_failure,
                               "video events could not be persisted",
                               stored.error().retryable);
    }
    const auto target_sequence = state.video.last_sequence();
    auto reloaded = load_state(store, state.session_id, state.target_run_id,
                               state.operation_id, stop_token);
    if (!reloaded) return std::unexpected(std::move(reloaded.error()));
    if (reloaded->video.last_sequence() != target_sequence) {
      return coordinator_error(VideoCoordinatorErrorCode::storage_failure,
                               "video run changed during conflict recovery",
                               true);
    }
    state = std::move(*reloaded);
  }
  return coordinator_error(VideoCoordinatorErrorCode::internal_failure,
                           "video conflict recovery failed internally");
}

[[nodiscard]] auto create_with_initial_events(
    storage::SessionStore& store, const storage::SessionCreate& session,
    State& state, const domain::RunId& run_id,
    std::vector<domain::RunEventPayload> payloads,
    const VideoTimestampSource& timestamp, const std::stop_token stop_token)
    -> std::expected<void, VideoCoordinatorError> {
  auto log = state.event_log;
  auto run = state.run;
  auto video = state.video;
  std::vector<domain::RunEvent> events;
  events.reserve(payloads.size());
  for (const auto& payload : payloads) {
    State candidate{state.session_id,
                    state.target_run_id,
                    state.operation_id,
                    log,
                    run,
                    video,
                    state.target_history};
    auto event = make_event(candidate, run_id, payload, timestamp);
    if (!event || !log.append(*event) || !run.apply(*event) ||
        !video.apply(*event)) {
      return coordinator_error(VideoCoordinatorErrorCode::protocol_failure,
                               "initial video state transition was rejected");
    }
    events.push_back(std::move(*event));
  }
  auto stored = store.create_session_with_events(session, events, stop_token);
  if (!stored) {
    return coordinator_error(
        VideoCoordinatorErrorCode::storage_failure,
        "video session and initial events could not be persisted",
        stored.error().retryable);
  }
  state.event_log = std::move(log);
  state.run = std::move(run);
  state.video = std::move(video);
  state.target_history = true;
  return {};
}

template <typename Requested>
[[nodiscard]] auto open_operation(storage::SessionStore& store,
                                  VideoSessionOpen session,
                                  const domain::RunId& run_id,
                                  const domain::RunStarted& attributes,
                                  Requested requested,
                                  const VideoTimestampSource& timestamp,
                                  const std::stop_token stop_token)
    -> std::expected<State, VideoCoordinatorError> {
  const auto operation_id = requested.operation_id;
  if (session.mode == VideoSessionMode::create_session) {
    State state{session.session_id,
                run_id,
                operation_id,
                domain::SessionEventLog{session.session_id},
                {},
                {},
                false};
    auto persisted = create_with_initial_events(
        store, {session.session_id, session.created_at}, state, run_id,
        {domain::RunStarted{attributes},
         domain::RunEventPayload{std::move(requested)}},
        timestamp, stop_token);
    if (!persisted) return std::unexpected(std::move(persisted.error()));
    return state;
  }
  auto state =
      load_state(store, session.session_id, run_id, operation_id, stop_token);
  if (!state) return std::unexpected(std::move(state.error()));
  if (session.mode == VideoSessionMode::start_run) {
    if (state->target_history) {
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "video run already has durable history");
    }
    auto persisted = append(store, *state, run_id,
                            {domain::RunStarted{attributes},
                             domain::RunEventPayload{std::move(requested)}},
                            timestamp, stop_token);
    if (!persisted) return std::unexpected(std::move(persisted.error()));
    return state;
  }
  if (session.mode != VideoSessionMode::resume_run || !state->target_history ||
      !state->run.run_id() || !state->video.run_id()) {
    return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                             "video run cannot be resumed");
  }
  return state;
}

[[nodiscard]] auto call_options(const domain::VideoProjection& projection,
                                const VideoCoordinatorLimits& limits,
                                const domain::EventTimestamp now,
                                const std::stop_token stop_token,
                                const std::size_t maximum_response_bytes)
    -> std::expected<backend::VideoServiceCallOptions, VideoCoordinatorError> {
  if (!projection.started_at()) {
    return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                             "durable video start timestamp is missing");
  }
  const auto started_count =
      projection.started_at()->time_since_epoch().count();
  const auto now_count = now.time_since_epoch().count();
  if (now_count < started_count) {
    return coordinator_error(VideoCoordinatorErrorCode::deadline_exceeded,
                             "video operation deadline clock regressed");
  }
  std::uint64_t elapsed_count{};
  if (now_count > started_count) {
    if (started_count < 0 && now_count >= 0) {
      elapsed_count = static_cast<std::uint64_t>(-(started_count + 1)) + 1U +
                      static_cast<std::uint64_t>(now_count);
    } else {
      elapsed_count = static_cast<std::uint64_t>(now_count - started_count);
    }
  }
  const auto timeout_count =
      static_cast<std::uint64_t>(limits.total_timeout.count());
  if (elapsed_count >= timeout_count) {
    return coordinator_error(VideoCoordinatorErrorCode::deadline_exceeded,
                             "video operation deadline expired");
  }
  const auto remaining =
      std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
          timeout_count - elapsed_count)};
  if (remaining <= std::chrono::milliseconds::zero() ||
      maximum_response_bytes == 0) {
    return coordinator_error(VideoCoordinatorErrorCode::invalid_request,
                             "video provider call options are invalid");
  }
  return backend::VideoServiceCallOptions{remaining, stop_token,
                                          maximum_response_bytes};
}

inline constexpr std::size_t maximum_video_control_response_bytes{
    std::size_t{64} * std::size_t{1024}};

[[nodiscard]] auto neutral_provider_error(
    const backend::VideoServiceError& error) -> domain::DomainError {
  const auto cancelled =
      error.code == backend::VideoServiceErrorCode::cancelled;
  return {cancelled ? domain::ErrorCode::cancelled : domain::ErrorCode::backend,
          cancelled ? "video provider operation was cancelled"
                    : "video provider operation failed",
          error.retryable};
}

[[nodiscard]] auto result_from(const domain::VideoProjection& projection,
                               const bool cleanup_pending)
    -> std::expected<VideoGenerationResult, VideoCoordinatorError> {
  if (!projection.quote() || !projection.artifact()) {
    return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                             "durable video result is incomplete");
  }
  return VideoGenerationResult{*projection.quote(), *projection.artifact(),
                               cleanup_pending};
}

[[nodiscard]] auto cancel_before_publication(
    storage::SessionStore& store, State& state, const domain::RunId& run_id,
    const VideoTimestampSource& timestamp, const std::stop_token)
    -> std::expected<void, VideoCoordinatorError> {
  return append(store, state, run_id,
                {domain::RunCancelRequested{std::string{"cancelled"}},
                 domain::RunCancelled{std::string{"cancelled"}}},
                timestamp, {});
}

[[nodiscard]] auto fail_before_publication(
    storage::SessionStore& store, State& state, const domain::RunId& run_id,
    domain::DomainError error, const VideoTimestampSource& timestamp,
    const std::stop_token) -> std::expected<void, VideoCoordinatorError> {
  return append(store, state, run_id, {domain::RunFailed{std::move(error)}},
                timestamp, {});
}

} // namespace

VideoCoordinator::VideoCoordinator(storage::SessionStore& sessions,
                                   storage::ArtifactStore& artifacts,
                                   backend::VideoService& service,
                                   VideoTimestampSource timestamp_source,
                                   const VideoCoordinatorLimits limits)
    : m_sessions(sessions), m_artifacts(artifacts), m_service(service),
      m_timestamp_source(std::move(timestamp_source)), m_limits(limits) {
  if (!m_timestamp_source) {
    m_timestamp_source = [] {
      return std::chrono::floor<std::chrono::milliseconds>(
          std::chrono::system_clock::now());
    };
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Video effects.
auto VideoCoordinator::generate(VideoSessionOpen session,
                                VideoGenerationOperation operation,
                                const std::stop_token stop_token)
    -> std::expected<VideoGenerationResult, VideoCoordinatorError> {
  try {
    if (!valid_limits(m_limits) ||
        !domain::validate_video_generation_spec(operation.spec)) {
      return coordinator_error(VideoCoordinatorErrorCode::invalid_request,
                               "video generation request is invalid");
    }
    if (stop_token.stop_requested() &&
        session.mode != VideoSessionMode::resume_run) {
      return coordinator_error(VideoCoordinatorErrorCode::cancelled,
                               "video generation was cancelled");
    }
    const auto open_token =
        stop_token.stop_requested() ? std::stop_token{} : stop_token;
    auto state = open_operation(
        m_sessions, std::move(session), operation.run_id, operation.attributes,
        domain::VideoGenerationRequested{operation.operation_id, operation.spec,
                                         operation.artifact_id},
        m_timestamp_source, open_token);
    if (!state) return std::unexpected(std::move(state.error()));
    if (!state->video.generation_request() ||
        *state->video.generation_request() !=
            domain::VideoGenerationRequested{operation.operation_id,
                                             operation.spec,
                                             operation.artifact_id} ||
        state->video.run_id() != operation.run_id) {
      return coordinator_error(
          VideoCoordinatorErrorCode::replay_rejected,
          "video generation recovery input does not match");
    }
    if (!state->video.attributes() ||
        *state->video.attributes() != operation.attributes) {
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "video run attributes do not match");
    }

    for (;;) {
      const auto lifecycle = state->video.state();
      const auto published = state->video.artifact().has_value();
      if (lifecycle == domain::VideoLifecycleState::completed)
        return result_from(state->video, false);
      if (lifecycle == domain::VideoLifecycleState::failed ||
          lifecycle == domain::VideoLifecycleState::cancelled ||
          lifecycle == domain::VideoLifecycleState::job_failed ||
          lifecycle == domain::VideoLifecycleState::cleanup_completed) {
        return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                                 "video generation run is terminal or invalid");
      }
      auto options =
          call_options(state->video, m_limits, m_timestamp_source(), stop_token,
                       maximum_video_control_response_bytes);
      if (!options && options.error().code !=
                          VideoCoordinatorErrorCode::deadline_exceeded) {
        return std::unexpected(std::move(options.error()));
      }
      if ((stop_token.stop_requested() || !options) && !published) {
        if (stop_token.stop_requested()) {
          if (auto cancelled = cancel_before_publication(
                  m_sessions, *state, operation.run_id, m_timestamp_source,
                  stop_token);
              !cancelled) {
            return std::unexpected(std::move(cancelled.error()));
          }
        } else if (auto failed = fail_before_publication(
                       m_sessions, *state, operation.run_id,
                       {domain::ErrorCode::unavailable,
                        "video generation deadline expired", false},
                       m_timestamp_source, stop_token);
                   !failed) {
          return std::unexpected(std::move(failed.error()));
        }
        return coordinator_error(
            stop_token.stop_requested()
                ? VideoCoordinatorErrorCode::cancelled
                : VideoCoordinatorErrorCode::deadline_exceeded,
            stop_token.stop_requested() ? "video generation was cancelled"
                                        : "video generation deadline expired");
      }
      if (lifecycle == domain::VideoLifecycleState::requested) {
        auto quoted = m_service.quote(operation.spec, *options);
        if (!quoted) {
          auto provider = neutral_provider_error(quoted.error());
          if (provider.code == domain::ErrorCode::cancelled) {
            if (auto cancelled = cancel_before_publication(
                    m_sessions, *state, operation.run_id, m_timestamp_source,
                    stop_token);
                !cancelled)
              return std::unexpected(std::move(cancelled.error()));
            return coordinator_error(VideoCoordinatorErrorCode::cancelled,
                                     "video generation was cancelled");
          }
          if (auto failed = fail_before_publication(
                  m_sessions, *state, operation.run_id, provider,
                  m_timestamp_source, stop_token);
              !failed)
            return std::unexpected(std::move(failed.error()));
          return coordinator_error(VideoCoordinatorErrorCode::provider_failure,
                                   "video quote failed", provider.retryable);
        }
        if (auto persisted =
                append(m_sessions, *state, operation.run_id,
                       {domain::VideoQuoteObserved{operation.operation_id,
                                                   quoted->amount}},
                       m_timestamp_source, {});
            !persisted)
          return std::unexpected(std::move(persisted.error()));
        continue;
      }
      if (lifecycle == domain::VideoLifecycleState::quoted) {
        auto queued =
            m_service.queue(operation.operation_id, operation.spec, *options);
        if (!queued) {
          const auto provider = neutral_provider_error(queued.error());
          if (provider.code == domain::ErrorCode::cancelled) {
            if (auto cancelled = cancel_before_publication(
                    m_sessions, *state, operation.run_id, m_timestamp_source,
                    stop_token);
                !cancelled)
              return std::unexpected(std::move(cancelled.error()));
            return coordinator_error(VideoCoordinatorErrorCode::cancelled,
                                     "video generation was cancelled");
          }
          if (auto failed = fail_before_publication(
                  m_sessions, *state, operation.run_id, provider,
                  m_timestamp_source, stop_token);
              !failed)
            return std::unexpected(std::move(failed.error()));
          return coordinator_error(VideoCoordinatorErrorCode::provider_failure,
                                   "video queue failed", provider.retryable);
        }
        if (auto persisted =
                append(m_sessions, *state, operation.run_id,
                       {domain::VideoJobQueued{operation.operation_id,
                                               queued->job_id}},
                       m_timestamp_source, {});
            !persisted)
          return std::unexpected(std::move(persisted.error()));
        continue;
      }
      if (lifecycle == domain::VideoLifecycleState::queued ||
          lifecycle == domain::VideoLifecycleState::processing ||
          lifecycle == domain::VideoLifecycleState::media_ready) {
        if (lifecycle != domain::VideoLifecycleState::media_ready &&
            state->video.last_poll_number() >= m_limits.maximum_polls) {
          if (auto failed = fail_before_publication(
                  m_sessions, *state, operation.run_id,
                  {domain::ErrorCode::unavailable,
                   "video polling limit was exhausted", false},
                  m_timestamp_source, stop_token);
              !failed)
            return std::unexpected(std::move(failed.error()));
          return coordinator_error(VideoCoordinatorErrorCode::poll_exhausted,
                                   "video polling limit was exhausted");
        }
        auto retrieved = m_service.retrieve(
            *state->video.job_id(), operation.spec.model_id,
            {options->timeout, stop_token, m_limits.mp4.maximum_bytes});
        if (!retrieved) {
          const auto provider = neutral_provider_error(retrieved.error());
          if (provider.code == domain::ErrorCode::cancelled) {
            if (auto cancelled = cancel_before_publication(
                    m_sessions, *state, operation.run_id, m_timestamp_source,
                    stop_token);
                !cancelled)
              return std::unexpected(std::move(cancelled.error()));
            return coordinator_error(VideoCoordinatorErrorCode::cancelled,
                                     "video generation was cancelled");
          }
          if (auto failed = fail_before_publication(
                  m_sessions, *state, operation.run_id, provider,
                  m_timestamp_source, stop_token);
              !failed)
            return std::unexpected(std::move(failed.error()));
          return coordinator_error(VideoCoordinatorErrorCode::provider_failure,
                                   "video retrieval failed",
                                   provider.retryable);
        }
        if (const auto* status =
                std::get_if<backend::VideoStatus>(&*retrieved)) {
          if (!domain::validate_video_job_state(status->state)) {
            if (auto failed = fail_before_publication(
                    m_sessions, *state, operation.run_id,
                    {domain::ErrorCode::invalid_event,
                     "video provider status was invalid", false},
                    m_timestamp_source, stop_token);
                !failed)
              return std::unexpected(std::move(failed.error()));
            return coordinator_error(
                VideoCoordinatorErrorCode::protocol_failure,
                "video provider status was invalid");
          }
          if (lifecycle == domain::VideoLifecycleState::media_ready) {
            if (auto failed = fail_before_publication(
                    m_sessions, *state, operation.run_id,
                    {domain::ErrorCode::invalid_event,
                     "video status followed completion", false},
                    m_timestamp_source, stop_token);
                !failed)
              return std::unexpected(std::move(failed.error()));
            return coordinator_error(
                VideoCoordinatorErrorCode::protocol_failure,
                "video status followed completion");
          }
          if (lifecycle == domain::VideoLifecycleState::processing &&
              status->state == domain::VideoJobState::queued) {
            if (auto failed = fail_before_publication(
                    m_sessions, *state, operation.run_id,
                    {domain::ErrorCode::invalid_event, "video status regressed",
                     false},
                    m_timestamp_source, stop_token);
                !failed)
              return std::unexpected(std::move(failed.error()));
            return coordinator_error(
                VideoCoordinatorErrorCode::protocol_failure,
                "video status regressed");
          }
          const auto poll = state->video.last_poll_number() + 1;
          std::vector<domain::RunEventPayload> payloads{
              domain::VideoJobStatusObserved{operation.operation_id,
                                             *state->video.job_id(), poll,
                                             status->state}};
          if (status->state == domain::VideoJobState::failed) {
            payloads.push_back(
                domain::RunFailed{{domain::ErrorCode::backend,
                                   "video generation job failed", false}});
          }
          if (auto persisted =
                  append(m_sessions, *state, operation.run_id,
                         std::move(payloads), m_timestamp_source, {});
              !persisted)
            return std::unexpected(std::move(persisted.error()));
          if (status->state == domain::VideoJobState::failed) {
            return coordinator_error(
                VideoCoordinatorErrorCode::provider_failure,
                "video generation job failed");
          }
          continue;
        }
        if (lifecycle != domain::VideoLifecycleState::media_ready) {
          if (auto failed = fail_before_publication(
                  m_sessions, *state, operation.run_id,
                  {domain::ErrorCode::invalid_event,
                   "video media arrived before completion", false},
                  m_timestamp_source, stop_token);
              !failed)
            return std::unexpected(std::move(failed.error()));
          return coordinator_error(VideoCoordinatorErrorCode::protocol_failure,
                                   "video media arrived before completion");
        }
        auto media = std::get<backend::VideoMedia>(std::move(*retrieved));
        if (media.media_type != "video/mp4") {
          if (auto failed = fail_before_publication(
                  m_sessions, *state, operation.run_id,
                  {domain::ErrorCode::invalid_event,
                   "video media type was invalid", false},
                  m_timestamp_source, stop_token);
              !failed)
            return std::unexpected(std::move(failed.error()));
          return coordinator_error(VideoCoordinatorErrorCode::protocol_failure,
                                   "video media type was invalid");
        }
        auto publication_options =
            call_options(state->video, m_limits, m_timestamp_source(),
                         stop_token, m_limits.mp4.maximum_bytes);
        if (stop_token.stop_requested() || !publication_options) {
          if (stop_token.stop_requested()) {
            if (auto cancelled = cancel_before_publication(
                    m_sessions, *state, operation.run_id, m_timestamp_source,
                    stop_token);
                !cancelled)
              return std::unexpected(std::move(cancelled.error()));
          } else if (auto failed = fail_before_publication(
                         m_sessions, *state, operation.run_id,
                         {domain::ErrorCode::unavailable,
                          "video generation deadline expired", false},
                         m_timestamp_source, stop_token);
                     !failed) {
            return std::unexpected(std::move(failed.error()));
          }
          return coordinator_error(
              stop_token.stop_requested()
                  ? VideoCoordinatorErrorCode::cancelled
                  : VideoCoordinatorErrorCode::deadline_exceeded,
              stop_token.stop_requested()
                  ? "video generation was cancelled"
                  : "video generation deadline expired");
        }
        auto artifact = publish_mp4_artifact(
            m_artifacts, {operation.artifact_id, std::nullopt, std::nullopt},
            std::move(media.encoded), m_limits.mp4, stop_token);
        if (!artifact) {
          const auto cancelled =
              artifact.error().code == VideoArtifactErrorCode::cancelled;
          if (cancelled) {
            if (auto recorded = cancel_before_publication(
                    m_sessions, *state, operation.run_id, m_timestamp_source,
                    stop_token);
                !recorded)
              return std::unexpected(std::move(recorded.error()));
          } else if (auto failed = fail_before_publication(
                         m_sessions, *state, operation.run_id,
                         {domain::ErrorCode::invalid_event,
                          "video artifact publication failed",
                          artifact.error().retryable},
                         m_timestamp_source, stop_token);
                     !failed) {
            return std::unexpected(std::move(failed.error()));
          }
          return coordinator_error(
              cancelled ? VideoCoordinatorErrorCode::cancelled
                        : VideoCoordinatorErrorCode::artifact_failure,
              cancelled ? "video generation was cancelled"
                        : "video artifact publication failed",
              artifact.error().retryable);
        }
        if (auto persisted = append(
                m_sessions, *state, operation.run_id,
                {domain::ArtifactCreated{*artifact},
                 domain::VideoArtifactPublished{
                     operation.operation_id, *state->video.job_id(), *artifact},
                 domain::VideoCleanupPending{operation.operation_id,
                                             *state->video.job_id(), 1}},
                m_timestamp_source, {});
            !persisted)
          return std::unexpected(std::move(persisted.error()));
        continue;
      }
      if (lifecycle == domain::VideoLifecycleState::published ||
          lifecycle == domain::VideoLifecycleState::cleanup_failed) {
        if (stop_token.stop_requested() || !options)
          return result_from(state->video, true);
        if (state->video.cleanup_attempt() ==
            std::numeric_limits<std::uint32_t>::max()) {
          return result_from(state->video, true);
        }
        if (auto persisted =
                append(m_sessions, *state, operation.run_id,
                       {domain::VideoCleanupPending{
                           operation.operation_id, *state->video.job_id(),
                           state->video.cleanup_attempt() + 1}},
                       m_timestamp_source, {});
            !persisted)
          return std::unexpected(std::move(persisted.error()));
        continue;
      }
      if (lifecycle == domain::VideoLifecycleState::cleanup_pending) {
        if (stop_token.stop_requested() || !options) {
          return result_from(state->video, true);
        }
        auto cleaned = m_service.cleanup(*state->video.job_id(),
                                         operation.spec.model_id, *options);
        if (!cleaned || !cleaned->completed) {
          const auto retryable = !cleaned || !cleaned->completed;
          if (auto persisted =
                  append(m_sessions, *state, operation.run_id,
                         {domain::VideoCleanupFailed{
                             operation.operation_id,
                             *state->video.job_id(),
                             state->video.cleanup_attempt(),
                             {domain::ErrorCode::unavailable,
                              "video cleanup remains pending", retryable}}},
                         m_timestamp_source, {});
              !persisted)
            return std::unexpected(std::move(persisted.error()));
          return result_from(state->video, true);
        }
        if (auto persisted =
                append(m_sessions, *state, operation.run_id,
                       {domain::VideoCleanupCompleted{
                            operation.operation_id, *state->video.job_id(),
                            state->video.cleanup_attempt()},
                        domain::RunCompleted{}},
                       m_timestamp_source, {});
            !persisted)
          return std::unexpected(std::move(persisted.error()));
        return result_from(state->video, false);
      }
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "video generation run is terminal or invalid");
    }
  } catch (...) {
    return coordinator_error(VideoCoordinatorErrorCode::internal_failure,
                             "video generation failed internally");
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Video effects.
auto VideoCoordinator::transcribe(VideoSessionOpen session,
                                  VideoTranscriptionOperation operation,
                                  const std::stop_token stop_token)
    -> std::expected<VideoTranscriptionResult, VideoCoordinatorError> {
  try {
    if (!valid_limits(m_limits)) {
      return coordinator_error(VideoCoordinatorErrorCode::invalid_request,
                               "video transcription request is invalid");
    }
    if (session.mode != VideoSessionMode::resume_run && !operation.url) {
      return coordinator_error(VideoCoordinatorErrorCode::invalid_request,
                               "video transcription requires a transient URL");
    }
    if (stop_token.stop_requested() &&
        session.mode != VideoSessionMode::resume_run) {
      return coordinator_error(VideoCoordinatorErrorCode::cancelled,
                               "video transcription was cancelled");
    }
    const auto open_token =
        stop_token.stop_requested() ? std::stop_token{} : stop_token;
    auto state = open_operation(m_sessions, std::move(session),
                                operation.run_id, operation.attributes,
                                domain::VideoTranscriptionRequested{
                                    operation.operation_id, operation.model_id},
                                m_timestamp_source, open_token);
    if (!state) return std::unexpected(std::move(state.error()));
    if (!state->video.transcription_request() ||
        *state->video.transcription_request() !=
            domain::VideoTranscriptionRequested{operation.operation_id,
                                                operation.model_id} ||
        state->video.run_id() != operation.run_id) {
      return coordinator_error(
          VideoCoordinatorErrorCode::replay_rejected,
          "video transcription recovery input does not match");
    }
    if (!state->video.attributes() ||
        *state->video.attributes() != operation.attributes) {
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "video run attributes do not match");
    }
    if (state->video.state() == domain::VideoLifecycleState::completed) {
      if (!state->video.transcription()) {
        return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                                 "durable video transcription is incomplete");
      }
      return VideoTranscriptionResult{state->video.transcription()->text,
                                      state->video.transcription()->language};
    }
    if (state->video.state() != domain::VideoLifecycleState::requested) {
      return coordinator_error(
          VideoCoordinatorErrorCode::replay_rejected,
          "video transcription run is terminal or invalid");
    }
    if (!operation.url) {
      return coordinator_error(
          VideoCoordinatorErrorCode::invalid_request,
          "video transcription recovery requires a transient URL");
    }
    auto options =
        call_options(state->video, m_limits, m_timestamp_source(), stop_token,
                     domain::maximum_video_transcription_response_bytes);
    if (!options &&
        options.error().code != VideoCoordinatorErrorCode::deadline_exceeded) {
      return std::unexpected(std::move(options.error()));
    }
    if (stop_token.stop_requested() || !options) {
      if (stop_token.stop_requested()) {
        if (auto cancelled =
                cancel_before_publication(m_sessions, *state, operation.run_id,
                                          m_timestamp_source, stop_token);
            !cancelled)
          return std::unexpected(std::move(cancelled.error()));
      } else if (auto failed = fail_before_publication(
                     m_sessions, *state, operation.run_id,
                     {domain::ErrorCode::unavailable,
                      "video transcription deadline expired", false},
                     m_timestamp_source, stop_token);
                 !failed) {
        return std::unexpected(std::move(failed.error()));
      }
      return coordinator_error(
          stop_token.stop_requested()
              ? VideoCoordinatorErrorCode::cancelled
              : VideoCoordinatorErrorCode::deadline_exceeded,
          stop_token.stop_requested() ? "video transcription was cancelled"
                                      : "video transcription deadline expired");
    }
    const auto transient_url = std::string{operation.url->value()};
    auto transcribed = m_service.transcribe(
        {operation.operation_id, operation.model_id, std::move(*operation.url)},
        *options);
    if (!transcribed) {
      const auto provider = neutral_provider_error(transcribed.error());
      if (provider.code == domain::ErrorCode::cancelled) {
        if (auto cancelled =
                cancel_before_publication(m_sessions, *state, operation.run_id,
                                          m_timestamp_source, stop_token);
            !cancelled)
          return std::unexpected(std::move(cancelled.error()));
        return coordinator_error(VideoCoordinatorErrorCode::cancelled,
                                 "video transcription was cancelled");
      }
      if (auto failed =
              fail_before_publication(m_sessions, *state, operation.run_id,
                                      provider, m_timestamp_source, stop_token);
          !failed)
        return std::unexpected(std::move(failed.error()));
      return coordinator_error(VideoCoordinatorErrorCode::provider_failure,
                               "video transcription failed",
                               provider.retryable);
    }
    auto completion_options =
        call_options(state->video, m_limits, m_timestamp_source(), stop_token,
                     domain::maximum_video_transcription_response_bytes);
    if (stop_token.stop_requested() || !completion_options) {
      if (stop_token.stop_requested()) {
        if (auto cancelled =
                cancel_before_publication(m_sessions, *state, operation.run_id,
                                          m_timestamp_source, stop_token);
            !cancelled)
          return std::unexpected(std::move(cancelled.error()));
      } else if (auto failed = fail_before_publication(
                     m_sessions, *state, operation.run_id,
                     {domain::ErrorCode::unavailable,
                      "video transcription deadline expired", false},
                     m_timestamp_source, stop_token);
                 !failed) {
        return std::unexpected(std::move(failed.error()));
      }
      return coordinator_error(
          stop_token.stop_requested()
              ? VideoCoordinatorErrorCode::cancelled
              : VideoCoordinatorErrorCode::deadline_exceeded,
          stop_token.stop_requested() ? "video transcription was cancelled"
                                      : "video transcription deadline expired");
    }
    if (transcribed->text.contains(transient_url) ||
        (transcribed->language &&
         transcribed->language->contains(transient_url))) {
      if (auto failed = fail_before_publication(
              m_sessions, *state, operation.run_id,
              {domain::ErrorCode::invalid_event,
               "video transcription output was invalid", false},
              m_timestamp_source, stop_token);
          !failed)
        return std::unexpected(std::move(failed.error()));
      return coordinator_error(VideoCoordinatorErrorCode::protocol_failure,
                               "video transcription output was invalid");
    }
    domain::VideoTranscriptionObserved observed{
        operation.operation_id, std::move(transcribed->text),
        std::move(transcribed->language)};
    if (!domain::validate_video_transcription(observed)) {
      if (auto failed = fail_before_publication(
              m_sessions, *state, operation.run_id,
              {domain::ErrorCode::invalid_event,
               "video transcription output was invalid", false},
              m_timestamp_source, stop_token);
          !failed)
        return std::unexpected(std::move(failed.error()));
      return coordinator_error(VideoCoordinatorErrorCode::protocol_failure,
                               "video transcription output was invalid");
    }
    auto result = VideoTranscriptionResult{observed.text, observed.language};
    if (auto persisted = append(m_sessions, *state, operation.run_id,
                                {std::move(observed), domain::RunCompleted{}},
                                m_timestamp_source, {});
        !persisted)
      return std::unexpected(std::move(persisted.error()));
    return result;
  } catch (...) {
    return coordinator_error(VideoCoordinatorErrorCode::internal_failure,
                             "video transcription failed internally");
  }
}

auto VideoCoordinator::replay(const domain::SessionId& session_id,
                              const domain::RunId& run_id,
                              const std::stop_token stop_token)
    -> std::expected<domain::VideoProjection, VideoCoordinatorError> {
  try {
    auto state =
        load_state(m_sessions, session_id, run_id, std::nullopt, stop_token);
    if (!state) return std::unexpected(std::move(state.error()));
    if (!state->target_history || !state->video.run_id() ||
        state->video.workflow() == domain::VideoWorkflow::none) {
      return coordinator_error(VideoCoordinatorErrorCode::replay_rejected,
                               "video run does not exist");
    }
    return std::move(state->video);
  } catch (...) {
    return coordinator_error(VideoCoordinatorErrorCode::internal_failure,
                             "video replay failed internally");
  }
}

} // namespace aiforge::runtime
