#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {

class SessionEventLog;

enum class VideoWorkflow { none, generation, transcription };

enum class VideoLifecycleState {
  not_started,
  requested,
  quoted,
  queued,
  processing,
  media_ready,
  job_failed,
  published,
  cleanup_pending,
  cleanup_failed,
  cleanup_completed,
  transcription_observed,
  completed,
  failed,
  cancelled,
};

enum class VideoProjectionErrorCode {
  invalid_envelope,
  duplicate_event,
  wrong_run,
  missing_run,
  non_monotonic_sequence,
  invalid_transition,
  mismatched_operation,
  mismatched_job,
  invalid_value,
  internal_failure,
};

struct VideoProjectionError {
  VideoProjectionErrorCode code{VideoProjectionErrorCode::invalid_transition};
  std::string message;
  auto operator==(const VideoProjectionError&) const -> bool = default;
};

class VideoProjection final {
 public:
  [[nodiscard]] auto apply(const RunEvent& event)
      -> std::expected<void, VideoProjectionError>;
  [[nodiscard]] static auto rebuild(const SessionEventLog& event_log,
                                    const RunId& run_id)
      -> std::expected<VideoProjection, VideoProjectionError>;

  [[nodiscard]] auto session_id() const noexcept
      -> const std::optional<SessionId>&;
  [[nodiscard]] auto run_id() const noexcept -> const std::optional<RunId>&;
  [[nodiscard]] auto started_at() const noexcept
      -> const std::optional<EventTimestamp>&;
  [[nodiscard]] auto attributes() const noexcept
      -> const std::optional<RunStarted>&;
  [[nodiscard]] auto workflow() const noexcept -> VideoWorkflow;
  [[nodiscard]] auto state() const noexcept -> VideoLifecycleState;
  [[nodiscard]] auto generation_request() const noexcept
      -> const std::optional<VideoGenerationRequested>&;
  [[nodiscard]] auto transcription_request() const noexcept
      -> const std::optional<VideoTranscriptionRequested>&;
  [[nodiscard]] auto quote() const noexcept
      -> const std::optional<MonetaryAmount>&;
  [[nodiscard]] auto job_id() const noexcept
      -> const std::optional<VideoJobId>&;
  [[nodiscard]] auto artifact() const noexcept
      -> const std::optional<ArtifactMetadata>&;
  [[nodiscard]] auto transcription() const noexcept
      -> const std::optional<VideoTranscriptionObserved>&;
  [[nodiscard]] auto last_poll_number() const noexcept -> std::uint32_t;
  [[nodiscard]] auto cleanup_attempt() const noexcept -> std::uint32_t;
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t;

 private:
  [[nodiscard]] auto apply_in_place(const RunEvent& event)
      -> std::expected<void, VideoProjectionError>;

  std::optional<SessionId> m_session_id;
  std::optional<RunId> m_run_id;
  std::optional<EventTimestamp> m_started_at;
  std::optional<RunStarted> m_attributes;
  VideoWorkflow m_workflow{VideoWorkflow::none};
  VideoLifecycleState m_state{VideoLifecycleState::not_started};
  std::optional<VideoGenerationRequested> m_generation_request;
  std::optional<VideoTranscriptionRequested> m_transcription_request;
  std::optional<MonetaryAmount> m_quote;
  std::optional<VideoJobId> m_job_id;
  std::optional<ArtifactMetadata> m_staged_artifact;
  std::optional<ArtifactMetadata> m_artifact;
  std::optional<VideoTranscriptionObserved> m_transcription;
  std::uint32_t m_last_poll_number{};
  std::uint32_t m_cleanup_attempt{};
  bool m_cancel_requested{};
  std::uint64_t m_last_sequence{};
};

[[nodiscard]] auto validate_video_transcription(
    const VideoTranscriptionObserved& observation) -> bool;

} // namespace aiforge::domain
