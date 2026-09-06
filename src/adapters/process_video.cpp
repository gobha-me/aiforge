#include <aiforge/adapters/process_video.hpp>

#include <ostream>
#include <string>
#include <utility>

#include <aiforge/presentation/video.hpp>
#include <aiforge/runtime/video_artifacts.hpp>

#include "secure_artifact_export.hpp"

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(cli::CommandFailureKind kind, std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

[[nodiscard]] auto replay_presentation(
    storage::SessionStore& sessions, domain::SessionId session_id,
    const std::optional<domain::ArtifactId>& artifact_id,
    const std::stop_token stop_token)
    -> std::expected<presentation::VideoPresentation, cli::CommandFailure> {
  auto events = sessions.replay_events(session_id, stop_token);
  if (!events) {
    if (events.error().code == storage::SessionStoreErrorCode::cancelled) {
      return failure(cli::CommandFailureKind::cancelled,
                     "video replay cancelled");
    }
    return failure(
        events.error().code == storage::SessionStoreErrorCode::not_found
            ? cli::CommandFailureKind::usage
            : cli::CommandFailureKind::runtime,
        events.error().code == storage::SessionStoreErrorCode::not_found
            ? "video session was not found"
            : "video session could not be replayed");
  }
  auto selected = presentation::select_video_presentation(std::move(session_id),
                                                          *events, artifact_id);
  if (!selected) {
    return failure(
        selected.error().code ==
                presentation::VideoPresentationErrorCode::invalid_history
            ? cli::CommandFailureKind::runtime
            : cli::CommandFailureKind::usage,
        selected.error().message);
  }
  return std::move(*selected);
}

[[nodiscard]] auto load_failure(const runtime::VideoArtifactError& error)
    -> cli::CommandFailure {
  if (error.code == runtime::VideoArtifactErrorCode::cancelled) {
    return {cli::CommandFailureKind::cancelled, "video export cancelled"};
  }
  return {cli::CommandFailureKind::runtime,
          "video artifact could not be validated for export"};
}

[[nodiscard]] auto write_presentation(
    const presentation::VideoPresentation& value, std::ostream& output)
    -> std::expected<void, cli::CommandFailure> {
  output << presentation::render_video_presentation_text(value);
  if (!output) {
    return failure(cli::CommandFailureKind::runtime,
                   "video presentation output failed");
  }
  return {};
}

} // namespace

ProcessVideoCommand::ProcessVideoCommand(
    SessionStoreFactory session_store_factory,
    ArtifactStoreFactory artifact_store_factory)
    : m_session_store_factory{std::move(session_store_factory)},
      m_artifact_store_factory{std::move(artifact_store_factory)} {
}

auto ProcessVideoCommand::show(ShowRequest request,
                               cli::CommandEnvironment& environment,
                               std::ostream& output, std::ostream&)
    -> std::expected<void, cli::CommandFailure> {
  try {
    if (!m_session_store_factory) {
      return failure(cli::CommandFailureKind::runtime,
                     "video session storage is unavailable");
    }
    auto sessions = m_session_store_factory();
    if (!sessions) return std::unexpected(std::move(sessions.error()));
    auto selected =
        replay_presentation(**sessions, std::move(request.session_id),
                            request.artifact_id, environment.stop_token);
    if (!selected) return std::unexpected(std::move(selected.error()));
    return write_presentation(*selected, output);
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "video show command failed internally");
  }
}

auto ProcessVideoCommand::export_artifact(ExportRequest request,
                                          cli::CommandEnvironment& environment,
                                          std::ostream& output, std::ostream&)
    -> std::expected<void, cli::CommandFailure> {
  try {
    if (!m_session_store_factory || !m_artifact_store_factory) {
      return failure(cli::CommandFailureKind::runtime,
                     "video storage is unavailable");
    }
    auto sessions = m_session_store_factory();
    if (!sessions) return std::unexpected(std::move(sessions.error()));
    auto selected =
        replay_presentation(**sessions, std::move(request.session_id),
                            request.artifact_id, environment.stop_token);
    if (!selected) return std::unexpected(std::move(selected.error()));
    auto artifacts = m_artifact_store_factory();
    if (!artifacts) return std::unexpected(std::move(artifacts.error()));
    auto artifact = runtime::load_mp4_artifact(**artifacts, selected->artifact,
                                               {}, environment.stop_token);
    if (!artifact) return std::unexpected(load_failure(artifact.error()));
    auto exported = detail::secure_export_bytes(
        artifact->content, request.output_path, environment.stop_token);
    if (!exported) return std::unexpected(std::move(exported.error()));
    return write_presentation(*selected, output);
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "video export command failed internally");
  }
}

} // namespace aiforge::adapters
