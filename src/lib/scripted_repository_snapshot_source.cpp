#include <aiforge/testing/scripted_repository_snapshot_source.hpp>

#include <utility>

namespace aiforge::testing {

ScriptedRepositorySnapshotSource::ScriptedRepositorySnapshotSource(
    std::vector<RepositorySnapshotExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {}

auto ScriptedRepositorySnapshotSource::observe(
    repository::RepositorySnapshotRequest request,
    const std::stop_token stop_token)
    -> std::expected<domain::RepositorySnapshot,
                     repository::RepositorySnapshotError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::cancelled,
          "repository observation cancelled", false});
    }
    m_recorded_requests.push_back(request);
    if (m_next_exchange >= m_exchanges.size()) {
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::internal_failure,
          "scripted repository source is exhausted", false});
    }
    const auto& exchange = m_exchanges[m_next_exchange];
    if (exchange.expected_request != request) {
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::internal_failure,
          "repository observation did not match the script", false});
    }
    ++m_next_exchange;
    if (const auto* error =
            std::get_if<repository::RepositorySnapshotError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<domain::RepositorySnapshot>(exchange.outcome);
  } catch (...) {
    return std::unexpected(repository::RepositorySnapshotError{
        repository::RepositorySnapshotErrorCode::internal_failure,
        "scripted repository source failed internally", false});
  }
}

auto ScriptedRepositorySnapshotSource::recorded_requests() const noexcept
    -> const std::vector<repository::RepositorySnapshotRequest>& {
  return m_recorded_requests;
}

auto ScriptedRepositorySnapshotSource::remaining_exchanges() const noexcept
    -> std::size_t {
  return m_exchanges.size() - m_next_exchange;
}

}  // namespace aiforge::testing
