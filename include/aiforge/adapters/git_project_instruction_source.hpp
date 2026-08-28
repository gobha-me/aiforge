#pragma once

#include <aiforge/adapters/git_repository_snapshot_source.hpp>
#include <aiforge/repository/project_instruction_source.hpp>

namespace aiforge::adapters {

class GitProjectInstructionSource final
    : public repository::ProjectInstructionSource {
 public:
  explicit GitProjectInstructionSource(
      GitRepositorySnapshotSource& snapshot_source) noexcept
      : m_snapshot_source(snapshot_source) {}

  [[nodiscard]] auto discover(repository::ProjectInstructionRequest request,
                              std::stop_token stop_token = {})
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override;

 private:
  GitRepositorySnapshotSource& m_snapshot_source;
};

} // namespace aiforge::adapters
