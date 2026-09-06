#include <aiforge/adapters/process_video.hpp>

#include <memory>
#include <string>
#include <utility>

#include <aiforge/adapters/filesystem_artifact_store.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/video/mp4.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(cli::CommandFailureKind kind, std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

[[nodiscard]] auto production_sessions()
    -> std::expected<std::shared_ptr<storage::SessionStore>,
                     cli::CommandFailure> {
  auto path = process_session_store_path();
  if (!path) {
    return failure(cli::CommandFailureKind::runtime,
                   "session storage path could not be resolved");
  }
  auto sessions = SqliteSessionStore::open_existing_read_only(*path);
  if (!sessions) {
    return failure(cli::CommandFailureKind::runtime,
                   "session storage could not be opened");
  }
  return std::shared_ptr<storage::SessionStore>{std::move(*sessions)};
}

[[nodiscard]] auto production_artifacts()
    -> std::expected<std::shared_ptr<storage::ArtifactStore>,
                     cli::CommandFailure> {
  auto path = process_session_store_path();
  if (!path) {
    return failure(cli::CommandFailureKind::runtime,
                   "artifact storage path could not be resolved");
  }
  auto artifacts = FilesystemArtifactStore::open(
      path->parent_path() / "artifacts",
      {.maximum_artifact_bytes = video::Mp4Limits{}.maximum_bytes},
      FilesystemArtifactStoreOpenMode::existing);
  if (!artifacts) {
    return failure(cli::CommandFailureKind::runtime,
                   "artifact storage could not be opened");
  }
  return std::shared_ptr<storage::ArtifactStore>{std::move(*artifacts)};
}

} // namespace

ProcessVideoCommand::ProcessVideoCommand()
    : ProcessVideoCommand{production_sessions, production_artifacts} {
}

} // namespace aiforge::adapters
