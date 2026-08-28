#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/repository/snapshot_source.hpp>

namespace aiforge::testing {

using RepositorySnapshotOutcome =
    std::variant<domain::RepositorySnapshot,
                 repository::RepositorySnapshotError>;

struct RepositorySnapshotExchange {
  repository::RepositorySnapshotRequest expected_request;
  RepositorySnapshotOutcome outcome;
  auto operator==(const RepositorySnapshotExchange&) const -> bool = default;
};

class ScriptedRepositorySnapshotSource final
    : public repository::RepositorySnapshotSource {
 public:
  explicit ScriptedRepositorySnapshotSource(
      std::vector<RepositorySnapshotExchange> exchanges = {});

  [[nodiscard]] auto observe(repository::RepositorySnapshotRequest request,
                             std::stop_token stop_token = {})
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> override;

  [[nodiscard]] auto recorded_requests() const noexcept
      -> const std::vector<repository::RepositorySnapshotRequest>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<RepositorySnapshotExchange> m_exchanges;
  std::vector<repository::RepositorySnapshotRequest> m_recorded_requests;
  std::size_t m_next_exchange{};
};

} // namespace aiforge::testing
