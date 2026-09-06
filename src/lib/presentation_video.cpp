#include <aiforge/presentation/video.hpp>

#include <aiforge/domain/event_log.hpp>

#include <format>
#include <utility>

namespace aiforge::presentation {
namespace {

[[nodiscard]] auto failure(VideoPresentationErrorCode code, std::string message)
    -> std::unexpected<VideoPresentationError> {
  return std::unexpected(VideoPresentationError{code, std::move(message)});
}

[[nodiscard]] auto cleanup_is_pending(
    const domain::VideoLifecycleState state) noexcept -> bool {
  return state == domain::VideoLifecycleState::published ||
         state == domain::VideoLifecycleState::cleanup_pending ||
         state == domain::VideoLifecycleState::cleanup_failed;
}

} // namespace

auto make_video_presentation(domain::SessionId session_id,
                             const domain::VideoProjection& projection,
                             const domain::ArtifactMetadata& metadata)
    -> std::expected<VideoPresentation, VideoPresentationError> {
  if (!projection.session_id() || *projection.session_id() != session_id ||
      !projection.run_id() || !projection.generation_request() ||
      !projection.artifact() || *projection.artifact() != metadata ||
      projection.generation_request()->artifact_id != metadata.artifact_id ||
      metadata.media_type != "video/mp4") {
    return failure(VideoPresentationErrorCode::invalid_history,
                   "durable video publication is inconsistent");
  }
  return VideoPresentation{
      std::move(session_id),
      *projection.run_id(),
      projection.generation_request()->operation_id,
      projection.state(),
      metadata,
      cleanup_is_pending(projection.state()),
  };
}

auto select_video_presentation(
    domain::SessionId session_id,
    const std::span<const domain::RunEvent> events,
    const std::optional<domain::ArtifactId>& artifact_id)
    -> std::expected<VideoPresentation, VideoPresentationError> {
  domain::SessionEventLog event_log{session_id};
  for (const auto& event : events) {
    if (!event_log.append(event)) {
      return failure(VideoPresentationErrorCode::invalid_history,
                     "durable video history is invalid");
    }
  }

  std::optional<domain::RunId> candidate_run;
  for (const auto& event : event_log.events()) {
    const auto* published =
        std::get_if<domain::VideoArtifactPublished>(&event.payload);
    if (published == nullptr ||
        (artifact_id && published->artifact.artifact_id != *artifact_id)) {
      continue;
    }
    if (!candidate_run) {
      candidate_run = event.metadata.run_id;
    } else if (*candidate_run != event.metadata.run_id) {
      return failure(VideoPresentationErrorCode::ambiguous,
                     "video artifact selection is ambiguous");
    }
  }

  if (!candidate_run) {
    return failure(VideoPresentationErrorCode::not_found,
                   artifact_id ? "video artifact is not present in that session"
                               : "session has no published video artifact");
  }

  auto projection = domain::VideoProjection::rebuild(event_log, *candidate_run);
  if (!projection || !projection->artifact() ||
      (artifact_id && projection->artifact()->artifact_id != *artifact_id)) {
    return failure(VideoPresentationErrorCode::invalid_history,
                   "durable video history is invalid");
  }
  return make_video_presentation(std::move(session_id), *projection,
                                 *projection->artifact());
}

auto video_lifecycle_state_name(
    const domain::VideoLifecycleState state) noexcept -> std::string_view {
  using enum domain::VideoLifecycleState;
  switch (state) {
    case not_started: return "not_started";
    case requested: return "requested";
    case quoted: return "quoted";
    case queued: return "queued";
    case processing: return "processing";
    case media_ready: return "media_ready";
    case job_failed: return "job_failed";
    case published: return "published";
    case cleanup_pending: return "cleanup_pending";
    case cleanup_failed: return "cleanup_failed";
    case cleanup_completed: return "cleanup_completed";
    case transcription_observed: return "transcription_observed";
    case completed: return "completed";
    case failed: return "failed";
    case cancelled: return "cancelled";
  }
  return "unknown";
}

auto render_video_presentation_text(const VideoPresentation& presentation)
    -> std::string {
  return std::format(
      "session={} run={} operation={} state={} artifact={} media=video/mp4 "
      "bytes={} digest={} cleanup_pending={}\n",
      presentation.session_id.value(), presentation.run_id.value(),
      presentation.operation_id.value(),
      video_lifecycle_state_name(presentation.lifecycle_state),
      presentation.artifact.artifact_id.value(),
      presentation.artifact.byte_size, presentation.artifact.digest,
      presentation.cleanup_pending ? "true" : "false");
}

} // namespace aiforge::presentation
