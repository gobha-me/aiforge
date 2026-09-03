#pragma once

#include <memory>
#include <string>

#include <aiforge/repository/snapshot_source.hpp>

namespace aiforge::adapters {

class GitProjectInstructionSource;
class GitExactSourceEditor;

enum class GitCommandPolicy {
  standard,
  isolated_read_only,
};

class GitRepositorySnapshotSource final
    : public repository::RepositorySnapshotSource {
 public:
  [[nodiscard]] static auto open(
      std::string git_executable,
      GitCommandPolicy command_policy = GitCommandPolicy::standard)
      -> std::expected<GitRepositorySnapshotSource,
                       repository::RepositorySnapshotError>;

  GitRepositorySnapshotSource(GitRepositorySnapshotSource&&) noexcept;
  auto operator=(GitRepositorySnapshotSource&&) noexcept
      -> GitRepositorySnapshotSource&;
  ~GitRepositorySnapshotSource() override;

  GitRepositorySnapshotSource(const GitRepositorySnapshotSource&) = delete;
  auto operator=(const GitRepositorySnapshotSource&)
      -> GitRepositorySnapshotSource& = delete;

  [[nodiscard]] auto command_policy() const noexcept -> GitCommandPolicy;
  [[nodiscard]] auto guarantees_read_only_observation() const noexcept
      -> bool override;

  [[nodiscard]] auto observe(repository::RepositorySnapshotRequest request,
                             std::stop_token stop_token = {})
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> override;

 private:
  friend class GitProjectInstructionSource;
  friend class GitExactSourceEditor;
  struct Impl;
  explicit GitRepositorySnapshotSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::adapters
