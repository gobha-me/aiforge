#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/backend/video.hpp>
#include <aiforge/domain/run_projection.hpp>
#include <aiforge/domain/video_projection.hpp>
#include <aiforge/runtime/video_artifacts.hpp>
#include <aiforge/storage/artifact_store.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::runtime {

enum class VideoSessionMode { create_session, start_run, resume_run };

struct VideoSessionOpen {
  domain::SessionId session_id;
  VideoSessionMode mode{VideoSessionMode::create_session};
  domain::EventTimestamp created_at;
  auto operator==(const VideoSessionOpen&) const -> bool = default;
};

struct VideoGenerationOperation {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::VideoOperationId operation_id;
  domain::VideoGenerationSpec spec;
  domain::ArtifactId artifact_id;
  auto operator==(const VideoGenerationOperation&) const -> bool = default;
};

struct VideoTranscriptionOperation {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::VideoOperationId operation_id;
  domain::ModelId model_id;
  std::optional<backend::TransientVideoUrl> url;
};

struct VideoCoordinatorLimits {
  std::uint32_t maximum_polls{120};
  std::chrono::milliseconds total_timeout{std::chrono::minutes{10}};
  video::Mp4Limits mp4{};
  auto operator==(const VideoCoordinatorLimits&) const -> bool = default;
};

struct VideoGenerationResult {
  domain::MonetaryAmount quote;
  domain::ArtifactMetadata artifact;
  bool cleanup_pending{};
  auto operator==(const VideoGenerationResult&) const -> bool = default;
};

struct VideoTranscriptionResult {
  std::string text;
  std::optional<std::string> language;
  auto operator==(const VideoTranscriptionResult&) const -> bool = default;
};

enum class VideoCoordinatorErrorCode {
  invalid_request,
  storage_failure,
  replay_rejected,
  provider_failure,
  protocol_failure,
  artifact_failure,
  deadline_exceeded,
  poll_exhausted,
  cancelled,
  internal_failure,
};

struct VideoCoordinatorError {
  VideoCoordinatorErrorCode code{VideoCoordinatorErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const VideoCoordinatorError&) const -> bool = default;
};

using VideoTimestampSource = std::function<domain::EventTimestamp()>;

class VideoCoordinator final {
 public:
  VideoCoordinator(storage::SessionStore& sessions,
                   storage::ArtifactStore& artifacts,
                   backend::VideoService& service,
                   VideoTimestampSource timestamp_source = {},
                   VideoCoordinatorLimits limits = {});

  [[nodiscard]] auto generate(VideoSessionOpen session,
                              VideoGenerationOperation operation,
                              std::stop_token stop_token = {})
      -> std::expected<VideoGenerationResult, VideoCoordinatorError>;
  [[nodiscard]] auto transcribe(VideoSessionOpen session,
                                VideoTranscriptionOperation operation,
                                std::stop_token stop_token = {})
      -> std::expected<VideoTranscriptionResult, VideoCoordinatorError>;

  // Pure replay with respect to provider and artifact boundaries.
  [[nodiscard]] auto replay(const domain::SessionId& session_id,
                            const domain::RunId& run_id,
                            std::stop_token stop_token = {})
      -> std::expected<domain::VideoProjection, VideoCoordinatorError>;

 private:
  storage::SessionStore& m_sessions;
  storage::ArtifactStore& m_artifacts;
  backend::VideoService& m_service;
  VideoTimestampSource m_timestamp_source;
  VideoCoordinatorLimits m_limits;
};

} // namespace aiforge::runtime
