#include <aiforge/domain/video_projection.hpp>

#include <aiforge/domain/event_log.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- UTF-8 checks.
[[nodiscard]] auto valid_utf8(const std::string_view value) -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0) return false;
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto valid_text_controls(const std::string_view value) -> bool {
  return std::ranges::none_of(value, [](const unsigned char character) {
    return (character < 0x20U && character != '\n' && character != '\t') ||
           character == 0x7FU;
  });
}

[[nodiscard]] auto valid_language_controls(const std::string_view value)
    -> bool {
  return std::ranges::none_of(value, [](const unsigned char character) {
    return character <= 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto projection_failure(const VideoProjectionErrorCode code,
                                      std::string message)
    -> std::expected<void, VideoProjectionError> {
  return std::unexpected(VideoProjectionError{code, std::move(message)});
}

[[nodiscard]] auto valid_artifact(const ArtifactMetadata& artifact) -> bool {
  return artifact.media_type == "video/mp4" && artifact.byte_size != 0 &&
         artifact.digest.starts_with("sha256:") &&
         artifact.digest.size() == 71 &&
         std::ranges::all_of(artifact.digest.substr(7),
                             [](const unsigned char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'a' && character <= 'f');
                             }) &&
         !artifact.producing_invocation_id &&
         !artifact.producing_inference_id && !artifact.width &&
         !artifact.height;
}

} // namespace

auto validate_video_job_state(const VideoJobState state) noexcept -> bool {
  switch (state) {
    case VideoJobState::queued:
    case VideoJobState::processing:
    case VideoJobState::completed:
    case VideoJobState::failed: return true;
  }
  return false;
}

auto validate_video_generation_spec(const VideoGenerationSpec& spec) -> bool {
  return !spec.prompt.empty() &&
         spec.prompt.size() <= maximum_video_prompt_bytes &&
         valid_utf8(spec.prompt) && valid_text_controls(spec.prompt) &&
         spec.duration > std::chrono::seconds::zero() &&
         spec.duration <= maximum_video_duration;
}

auto validate_video_transcription(const VideoTranscriptionObserved& observation)
    -> bool {
  return !observation.text.empty() &&
         observation.text.size() <= maximum_video_transcription_bytes &&
         valid_utf8(observation.text) &&
         valid_text_controls(observation.text) &&
         (!observation.language ||
          (!observation.language->empty() &&
           observation.language->size() <= maximum_video_language_bytes &&
           valid_utf8(*observation.language) &&
           valid_language_controls(*observation.language)));
}

auto VideoProjection::apply(const RunEvent& event)
    -> std::expected<void, VideoProjectionError> {
  try {
    auto candidate = *this;
    if (auto applied = candidate.apply_in_place(event); !applied) {
      return applied;
    }
    *this = std::move(candidate);
    return {};
  } catch (...) {
    return projection_failure(VideoProjectionErrorCode::internal_failure,
                              "video projection failed internally");
  }
}

auto VideoProjection::rebuild(const SessionEventLog& event_log,
                              const RunId& run_id)
    -> std::expected<VideoProjection, VideoProjectionError> {
  try {
    VideoProjection projection;
    projection.m_session_id = event_log.session_id();
    std::set<EventId> event_ids;
    std::uint64_t session_sequence{};

    for (const auto& event : event_log.events()) {
      if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
        return std::unexpected(
            VideoProjectionError{VideoProjectionErrorCode::invalid_envelope,
                                 "video session event envelope is invalid"});
      }
      if (!event_ids.insert(event.metadata.event_id).second) {
        return std::unexpected(VideoProjectionError{
            VideoProjectionErrorCode::duplicate_event,
            "video session contains a duplicate event ID"});
      }
      if (event.metadata.sequence <= session_sequence) {
        return std::unexpected(VideoProjectionError{
            VideoProjectionErrorCode::non_monotonic_sequence,
            "video session event sequence did not increase"});
      }
      session_sequence = event.metadata.sequence;

      if (event.metadata.run_id == run_id) {
        if (auto applied = projection.apply_in_place(event); !applied) {
          return std::unexpected(std::move(applied.error()));
        }
      }
    }

    if (!projection.m_run_id) {
      return std::unexpected(
          VideoProjectionError{VideoProjectionErrorCode::missing_run,
                               "video target run is absent from the session"});
    }
    const auto incomplete_atomic_tail =
        (projection.m_cancel_requested &&
         projection.m_state != VideoLifecycleState::cancelled) ||
        projection.m_staged_artifact ||
        projection.m_state == VideoLifecycleState::job_failed ||
        projection.m_state == VideoLifecycleState::published ||
        projection.m_state == VideoLifecycleState::cleanup_completed ||
        projection.m_state == VideoLifecycleState::transcription_observed;
    if (incomplete_atomic_tail) {
      return std::unexpected(VideoProjectionError{
          VideoProjectionErrorCode::invalid_transition,
          "video history ends inside an atomic lifecycle transition"});
    }
    return projection;
  } catch (...) {
    return std::unexpected(
        VideoProjectionError{VideoProjectionErrorCode::internal_failure,
                             "video projection rebuild failed internally"});
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Typed reducer.
auto VideoProjection::apply_in_place(const RunEvent& event)
    -> std::expected<void, VideoProjectionError> {
  if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
    return projection_failure(VideoProjectionErrorCode::invalid_envelope,
                              "video event envelope is invalid");
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return projection_failure(VideoProjectionErrorCode::non_monotonic_sequence,
                              "video event sequence did not increase");
  }
  if (m_run_id && *m_run_id != event.metadata.run_id) {
    return projection_failure(VideoProjectionErrorCode::wrong_run,
                              "video event belongs to another run");
  }
  if (std::holds_alternative<UnknownEvent>(event.payload)) {
    m_last_sequence = event.metadata.sequence;
    return {};
  }
  if (m_cancel_requested &&
      !std::holds_alternative<RunCancelled>(event.payload)) {
    return projection_failure(VideoProjectionErrorCode::invalid_transition,
                              "known event followed cancellation request");
  }
  if (m_state == VideoLifecycleState::job_failed &&
      !std::holds_alternative<RunFailed>(event.payload)) {
    return projection_failure(VideoProjectionErrorCode::invalid_transition,
                              "known event followed failed video status");
  }
  const auto terminal = m_state == VideoLifecycleState::completed ||
                        m_state == VideoLifecycleState::failed ||
                        m_state == VideoLifecycleState::cancelled;
  if (terminal) {
    return projection_failure(VideoProjectionErrorCode::invalid_transition,
                              "known event followed terminal video run");
  }

  auto result = std::expected<void, VideoProjectionError>{};
  if (const auto* started = std::get_if<RunStarted>(&event.payload)) {
    static_cast<void>(started);
    if (m_run_id || m_state != VideoLifecycleState::not_started) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video run may start only once");
    } else {
      m_run_id = event.metadata.run_id;
      m_started_at = event.metadata.timestamp;
      m_attributes = *started;
    }
  } else if (!m_run_id) {
    result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                "video run must start first");
  } else if (const auto* requested =
                 std::get_if<VideoGenerationRequested>(&event.payload)) {
    if (m_workflow != VideoWorkflow::none ||
        m_state != VideoLifecycleState::not_started ||
        !validate_video_generation_spec(requested->spec)) {
      result = projection_failure(VideoProjectionErrorCode::invalid_value,
                                  "video generation request is invalid");
    } else {
      m_workflow = VideoWorkflow::generation;
      m_generation_request = *requested;
      m_state = VideoLifecycleState::requested;
    }
  } else if (const auto* requested =
                 std::get_if<VideoTranscriptionRequested>(&event.payload)) {
    if (m_workflow != VideoWorkflow::none ||
        m_state != VideoLifecycleState::not_started) {
      result =
          projection_failure(VideoProjectionErrorCode::invalid_transition,
                             "video transcription request is out of order");
    } else {
      m_workflow = VideoWorkflow::transcription;
      m_transcription_request = *requested;
      m_state = VideoLifecycleState::requested;
    }
  } else if (const auto* observed =
                 std::get_if<VideoQuoteObserved>(&event.payload)) {
    if (!m_generation_request ||
        observed->operation_id != m_generation_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "video quote operation does not match");
    } else if (m_state != VideoLifecycleState::requested || m_quote) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video quote is repeated or out of order");
    } else {
      m_quote = observed->quote;
      m_state = VideoLifecycleState::quoted;
    }
  } else if (const auto* queued = std::get_if<VideoJobQueued>(&event.payload)) {
    if (!m_generation_request ||
        queued->operation_id != m_generation_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "queued video operation does not match");
    } else if (m_state != VideoLifecycleState::quoted || m_job_id) {
      result =
          projection_failure(VideoProjectionErrorCode::invalid_transition,
                             "queued video job is repeated or out of order");
    } else {
      m_job_id = queued->job_id;
      m_state = VideoLifecycleState::queued;
    }
  } else if (const auto* status =
                 std::get_if<VideoJobStatusObserved>(&event.payload)) {
    if (!m_generation_request ||
        status->operation_id != m_generation_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "video status operation does not match");
    } else if (!m_job_id || status->job_id != *m_job_id) {
      result = projection_failure(VideoProjectionErrorCode::mismatched_job,
                                  "video status job does not match");
    } else if (!validate_video_job_state(status->state)) {
      result = projection_failure(VideoProjectionErrorCode::invalid_value,
                                  "video status state is invalid");
    } else if (m_last_poll_number ==
                   std::numeric_limits<std::uint32_t>::max() ||
               status->poll_number != m_last_poll_number + 1) {
      result = projection_failure(VideoProjectionErrorCode::invalid_value,
                                  "video poll number is not consecutive");
    } else if (m_state != VideoLifecycleState::queued &&
               m_state != VideoLifecycleState::processing) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video status is out of order");
    } else if (m_state == VideoLifecycleState::processing &&
               status->state == VideoJobState::queued) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video status regressed");
    } else {
      m_last_poll_number = status->poll_number;
      switch (status->state) {
        case VideoJobState::queued:
          m_state = VideoLifecycleState::queued;
          break;
        case VideoJobState::processing:
          m_state = VideoLifecycleState::processing;
          break;
        case VideoJobState::completed:
          m_state = VideoLifecycleState::media_ready;
          break;
        case VideoJobState::failed:
          m_state = VideoLifecycleState::job_failed;
          break;
      }
    }
  } else if (const auto* created =
                 std::get_if<ArtifactCreated>(&event.payload)) {
    if (m_state != VideoLifecycleState::media_ready || m_staged_artifact ||
        !m_generation_request ||
        created->artifact.artifact_id != m_generation_request->artifact_id ||
        !valid_artifact(created->artifact)) {
      result = projection_failure(VideoProjectionErrorCode::invalid_value,
                                  "video artifact creation is invalid");
    } else {
      m_staged_artifact = created->artifact;
    }
  } else if (const auto* published =
                 std::get_if<VideoArtifactPublished>(&event.payload)) {
    if (!m_generation_request ||
        published->operation_id != m_generation_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "video publication operation does not match");
    } else if (!m_job_id || published->job_id != *m_job_id) {
      result = projection_failure(VideoProjectionErrorCode::mismatched_job,
                                  "video publication job does not match");
    } else if (m_state != VideoLifecycleState::media_ready ||
               !m_staged_artifact ||
               *m_staged_artifact != published->artifact) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video publication is out of order");
    } else {
      m_artifact = published->artifact;
      m_staged_artifact.reset();
      m_state = VideoLifecycleState::published;
    }
  } else if (const auto* pending =
                 std::get_if<VideoCleanupPending>(&event.payload)) {
    if (!m_generation_request ||
        pending->operation_id != m_generation_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "video cleanup operation does not match");
    } else if (!m_job_id || pending->job_id != *m_job_id) {
      result = projection_failure(VideoProjectionErrorCode::mismatched_job,
                                  "video cleanup job does not match");
    } else if ((m_state != VideoLifecycleState::published &&
                m_state != VideoLifecycleState::cleanup_failed) ||
               m_cleanup_attempt == std::numeric_limits<std::uint32_t>::max() ||
               pending->attempt != m_cleanup_attempt + 1) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video cleanup attempt is out of order");
    } else {
      m_cleanup_attempt = pending->attempt;
      m_state = VideoLifecycleState::cleanup_pending;
    }
  } else if (const auto* completed =
                 std::get_if<VideoCleanupCompleted>(&event.payload)) {
    if (!m_generation_request ||
        completed->operation_id != m_generation_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "video cleanup operation does not match");
    } else if (!m_job_id || completed->job_id != *m_job_id) {
      result = projection_failure(VideoProjectionErrorCode::mismatched_job,
                                  "video cleanup job does not match");
    } else if (m_state != VideoLifecycleState::cleanup_pending ||
               completed->attempt != m_cleanup_attempt) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video cleanup completion is out of order");
    } else {
      m_state = VideoLifecycleState::cleanup_completed;
    }
  } else if (const auto* failed =
                 std::get_if<VideoCleanupFailed>(&event.payload)) {
    if (!m_generation_request ||
        failed->operation_id != m_generation_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "video cleanup operation does not match");
    } else if (!m_job_id || failed->job_id != *m_job_id) {
      result = projection_failure(VideoProjectionErrorCode::mismatched_job,
                                  "video cleanup job does not match");
    } else if (m_state != VideoLifecycleState::cleanup_pending ||
               failed->attempt != m_cleanup_attempt ||
               !failed->error.retryable) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video cleanup failure is invalid");
    } else {
      m_state = VideoLifecycleState::cleanup_failed;
    }
  } else if (const auto* observed =
                 std::get_if<VideoTranscriptionObserved>(&event.payload)) {
    if (!m_transcription_request ||
        observed->operation_id != m_transcription_request->operation_id) {
      result =
          projection_failure(VideoProjectionErrorCode::mismatched_operation,
                             "video transcription operation does not match");
    } else if (m_state != VideoLifecycleState::requested ||
               !validate_video_transcription(*observed)) {
      result = projection_failure(VideoProjectionErrorCode::invalid_value,
                                  "video transcription is invalid");
    } else {
      m_transcription = *observed;
      m_state = VideoLifecycleState::transcription_observed;
    }
  } else if (std::holds_alternative<RunCancelRequested>(event.payload)) {
    if (m_workflow == VideoWorkflow::none || m_cancel_requested ||
        m_state == VideoLifecycleState::published ||
        m_state == VideoLifecycleState::cleanup_pending ||
        m_state == VideoLifecycleState::cleanup_failed ||
        m_state == VideoLifecycleState::cleanup_completed ||
        m_state == VideoLifecycleState::transcription_observed) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video cancellation is out of order");
    } else {
      m_cancel_requested = true;
    }
  } else if (std::holds_alternative<RunCancelled>(event.payload)) {
    if (!m_cancel_requested) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video cancellation was not requested");
    } else {
      m_state = VideoLifecycleState::cancelled;
    }
  } else if (std::holds_alternative<RunFailed>(event.payload)) {
    if (m_workflow == VideoWorkflow::none ||
        m_state == VideoLifecycleState::published ||
        m_state == VideoLifecycleState::cleanup_pending ||
        m_state == VideoLifecycleState::cleanup_failed ||
        m_state == VideoLifecycleState::cleanup_completed ||
        m_state == VideoLifecycleState::transcription_observed) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "published video run cannot fail");
    } else {
      m_state = VideoLifecycleState::failed;
    }
  } else if (std::holds_alternative<RunCompleted>(event.payload)) {
    if (m_state != VideoLifecycleState::cleanup_completed &&
        m_state != VideoLifecycleState::transcription_observed) {
      result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                  "video run completed before durable success");
    } else {
      m_state = VideoLifecycleState::completed;
    }
  } else {
    result = projection_failure(VideoProjectionErrorCode::invalid_transition,
                                "event is not valid in a video run");
  }

  if (result) m_last_sequence = event.metadata.sequence;
  return result;
}

auto VideoProjection::session_id() const noexcept
    -> const std::optional<SessionId>& {
  return m_session_id;
}
auto VideoProjection::run_id() const noexcept -> const std::optional<RunId>& {
  return m_run_id;
}
auto VideoProjection::started_at() const noexcept
    -> const std::optional<EventTimestamp>& {
  return m_started_at;
}
auto VideoProjection::attributes() const noexcept
    -> const std::optional<RunStarted>& {
  return m_attributes;
}
auto VideoProjection::workflow() const noexcept -> VideoWorkflow {
  return m_workflow;
}
auto VideoProjection::state() const noexcept -> VideoLifecycleState {
  return m_state;
}
auto VideoProjection::generation_request() const noexcept
    -> const std::optional<VideoGenerationRequested>& {
  return m_generation_request;
}
auto VideoProjection::transcription_request() const noexcept
    -> const std::optional<VideoTranscriptionRequested>& {
  return m_transcription_request;
}
auto VideoProjection::quote() const noexcept
    -> const std::optional<MonetaryAmount>& {
  return m_quote;
}
auto VideoProjection::job_id() const noexcept
    -> const std::optional<VideoJobId>& {
  return m_job_id;
}
auto VideoProjection::artifact() const noexcept
    -> const std::optional<ArtifactMetadata>& {
  return m_artifact;
}
auto VideoProjection::transcription() const noexcept
    -> const std::optional<VideoTranscriptionObserved>& {
  return m_transcription;
}
auto VideoProjection::last_poll_number() const noexcept -> std::uint32_t {
  return m_last_poll_number;
}
auto VideoProjection::cleanup_attempt() const noexcept -> std::uint32_t {
  return m_cleanup_attempt;
}
auto VideoProjection::last_sequence() const noexcept -> std::uint64_t {
  return m_last_sequence;
}

} // namespace aiforge::domain
