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

  [[nodiscard]] auto guarantees_read_only_discovery() const noexcept
      -> bool override {
    return m_snapshot_source.guarantees_read_only_observation();
  }
  [[nodiscard]] auto is_coupled_to(
      const repository::RepositorySnapshotSource& source) const noexcept
      -> bool override {
    return &source == &m_snapshot_source;
  }
  [[nodiscard]] auto discover_pinned(
      repository::ProjectInstructionRequest request, int root_descriptor,
      std::stop_token stop_token = {})
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError>;

  [[nodiscard]] auto discover(repository::ProjectInstructionRequest request,
                              std::stop_token stop_token = {})
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override;

 private:
  [[nodiscard]] auto discover_impl(
      repository::ProjectInstructionRequest request, std::stop_token stop_token,
      int root_descriptor)
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError>;
  [[nodiscard]] auto instruction_document(
      const repository::ProjectInstructionRequest& request,
      const std::string& subtree, std::uint32_t specificity,
      std::uint64_t discovery_order, std::string content,
      std::stop_token stop_token, int root_descriptor,
      std::chrono::steady_clock::time_point deadline)
      -> std::expected<domain::ProjectInstructionDocument,
                       repository::ProjectInstructionError>;
  [[nodiscard]] auto instruction_documents(
      const repository::ProjectInstructionRequest& request,
      std::stop_token stop_token, int root_descriptor,
      std::chrono::steady_clock::time_point deadline)
      -> std::expected<std::vector<domain::ProjectInstructionDocument>,
                       repository::ProjectInstructionError>;
  GitRepositorySnapshotSource& m_snapshot_source;
};

} // namespace aiforge::adapters
