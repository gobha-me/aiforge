#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <aiforge/domain/video_projection.hpp>

namespace aiforge::presentation {

struct VideoPresentation {
  domain::SessionId session_id;
  domain::RunId run_id;
  domain::VideoOperationId operation_id;
  domain::VideoLifecycleState lifecycle_state{
      domain::VideoLifecycleState::not_started};
  domain::ArtifactMetadata artifact;
  bool cleanup_pending{};
  auto operator==(const VideoPresentation&) const -> bool = default;
};

enum class VideoPresentationErrorCode {
  invalid_history,
  not_found,
  ambiguous,
};

struct VideoPresentationError {
  VideoPresentationErrorCode code{VideoPresentationErrorCode::invalid_history};
  std::string message;
  auto operator==(const VideoPresentationError&) const -> bool = default;
};

[[nodiscard]] auto make_video_presentation(
    domain::SessionId session_id, const domain::VideoProjection& projection,
    const domain::ArtifactMetadata& metadata)
    -> std::expected<VideoPresentation, VideoPresentationError>;

[[nodiscard]] auto select_video_presentation(
    domain::SessionId session_id, std::span<const domain::RunEvent> events,
    const std::optional<domain::ArtifactId>& artifact_id = std::nullopt)
    -> std::expected<VideoPresentation, VideoPresentationError>;

[[nodiscard]] auto video_lifecycle_state_name(
    domain::VideoLifecycleState state) noexcept -> std::string_view;

[[nodiscard]] auto render_video_presentation_text(
    const VideoPresentation& presentation) -> std::string;

} // namespace aiforge::presentation
