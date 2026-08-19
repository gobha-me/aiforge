#pragma once

#include <memory>
#include <string>

#include <aiforge/repository/snapshot_source.hpp>

namespace aiforge::adapters {

class GitProjectInstructionSource;

class GitRepositorySnapshotSource final
    : public repository::RepositorySnapshotSource {
 public:
  [[nodiscard]] static auto open(std::string git_executable)
      -> std::expected<GitRepositorySnapshotSource,
                       repository::RepositorySnapshotError>;

  GitRepositorySnapshotSource(GitRepositorySnapshotSource&&) noexcept;
  auto operator=(GitRepositorySnapshotSource&&) noexcept
      -> GitRepositorySnapshotSource&;
  ~GitRepositorySnapshotSource() override;

  GitRepositorySnapshotSource(const GitRepositorySnapshotSource&) = delete;
  auto operator=(const GitRepositorySnapshotSource&)
      -> GitRepositorySnapshotSource& = delete;

  [[nodiscard]] auto observe(
      repository::RepositorySnapshotRequest request,
      std::stop_token stop_token = {})
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> override;

 private:
  friend class GitProjectInstructionSource;
  struct Impl;
  explicit GitRepositorySnapshotSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

}  // namespace aiforge::adapters
