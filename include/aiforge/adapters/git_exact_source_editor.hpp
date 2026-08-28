#pragma once

#include <aiforge/adapters/git_repository_snapshot_source.hpp>
#include <aiforge/repository/exact_source_edit.hpp>

namespace aiforge::adapters {

class GitExactSourceEditor final : public repository::ExactSourceEditor {
 public:
  explicit GitExactSourceEditor(
      GitRepositorySnapshotSource& snapshot_source) noexcept
      : m_snapshot_source(snapshot_source) {}

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
};

} // namespace aiforge::adapters
