#pragma once

#include <aiforge/adapters/git_repository_snapshot_source.hpp>
#include <aiforge/repository/exact_source_edit.hpp>

namespace aiforge::adapters {

enum class GitExactSourceReadPolicy {
  any_regular_file,
  tracked_regular_files,
};

class GitExactSourceEditor final : public repository::ExactSourceEditor {
 public:
  explicit GitExactSourceEditor(
      GitRepositorySnapshotSource& snapshot_source,
      const GitExactSourceReadPolicy read_policy =
          GitExactSourceReadPolicy::any_regular_file) noexcept
      : m_snapshot_source(snapshot_source), m_read_policy(read_policy) {}

  [[nodiscard]] auto guarantees_tracked_regular_files() const noexcept
      -> bool override {
    return m_read_policy == GitExactSourceReadPolicy::tracked_regular_files;
  }
  [[nodiscard]] auto guarantees_read_only_execution() const noexcept
      -> bool override;
  [[nodiscard]] auto is_coupled_to(
      const repository::RepositorySnapshotSource& source) const noexcept
      -> bool override {
    return &source == static_cast<const repository::RepositorySnapshotSource*>(
                          &m_snapshot_source);
  }

  [[nodiscard]] auto read(repository::ExactSourceReadRequest request,
                          std::stop_token stop_token = {})
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override;

  [[nodiscard]] auto apply(repository::ExactSourceEditRequest request,
                           std::stop_token stop_token = {})
      -> std::expected<repository::ExactSourceEditReceipt,
                       repository::ExactSourceEditError> override;

 private:
  GitRepositorySnapshotSource& m_snapshot_source;
  GitExactSourceReadPolicy m_read_policy;
};

} // namespace aiforge::adapters
