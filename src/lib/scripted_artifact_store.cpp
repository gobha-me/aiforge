#include <aiforge/testing/scripted_artifact_store.hpp>

#include <utility>

namespace aiforge::testing {

ScriptedArtifactStore::ScriptedArtifactStore(
    std::vector<ArtifactStoreExchange> exchanges,
    std::vector<ArtifactStoreReadExchange> reads)
    : m_exchanges(std::move(exchanges)), m_reads(std::move(reads)) {
}

auto ScriptedArtifactStore::get(const domain::ArtifactMetadata& metadata,
                                const std::size_t maximum_bytes,
                                const std::stop_token stop_token)
    -> std::expected<storage::ArtifactRead, storage::ArtifactStoreError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::cancelled, "artifact read cancelled",
          false});
    }
    if (m_next_read >= m_reads.size()) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::unavailable,
          "scripted artifact store has no read remaining", false});
    }
    const auto& exchange = m_reads[m_next_read];
    if (exchange.expected_metadata != metadata ||
        exchange.expected_maximum_bytes != maximum_bytes) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::internal_failure,
          "artifact read did not match the script", false});
    }
    ++m_next_read;
    if (const auto* error =
            std::get_if<storage::ArtifactStoreError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<storage::ArtifactRead>(exchange.outcome);
  } catch (...) {
    return std::unexpected(storage::ArtifactStoreError{
        storage::ArtifactStoreErrorCode::internal_failure,
        "scripted artifact read failed internally", false});
  }
}

auto ScriptedArtifactStore::put(storage::ArtifactWrite write,
                                const std::span<const std::byte> content,
                                const std::stop_token stop_token)
    -> std::expected<domain::ArtifactMetadata, storage::ArtifactStoreError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::cancelled,
          "artifact write cancelled", false});
    }

    ArtifactStoreCall call{
        std::move(write),
        std::vector<std::byte>{content.begin(), content.end()}};
    m_recorded_calls.push_back(call);
    if (m_next_exchange >= m_exchanges.size()) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::unavailable,
          "scripted artifact store has no exchange remaining", false});
    }

    const auto& exchange = m_exchanges[m_next_exchange];
    if (exchange.expected_call != call) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::internal_failure,
          "artifact write did not match the script", false});
    }
    ++m_next_exchange;
    if (const auto* error =
            std::get_if<storage::ArtifactStoreError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<domain::ArtifactMetadata>(exchange.outcome);
  } catch (...) {
    return std::unexpected(storage::ArtifactStoreError{
        storage::ArtifactStoreErrorCode::internal_failure,
        "scripted artifact store failed internally", false});
  }
}

auto ScriptedArtifactStore::recorded_calls() const noexcept
    -> const std::vector<ArtifactStoreCall>& {
  return m_recorded_calls;
}

auto ScriptedArtifactStore::remaining_exchanges() const noexcept
    -> std::size_t {
  return m_exchanges.size() - m_next_exchange;
}

} // namespace aiforge::testing
