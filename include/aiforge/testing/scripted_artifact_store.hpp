#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/storage/artifact_store.hpp>

namespace aiforge::testing {

struct ArtifactStoreCall {
  storage::ArtifactWrite write;
  std::vector<std::byte> content;
  auto operator==(const ArtifactStoreCall&) const -> bool = default;
};

using ArtifactStoreOutcome =
    std::variant<domain::ArtifactMetadata, storage::ArtifactStoreError>;

struct ArtifactStoreExchange {
  ArtifactStoreCall expected_call;
  ArtifactStoreOutcome outcome;
  auto operator==(const ArtifactStoreExchange&) const -> bool = default;
};

class ScriptedArtifactStore final : public storage::ArtifactStore {
 public:
  explicit ScriptedArtifactStore(
      std::vector<ArtifactStoreExchange> exchanges = {});

  [[nodiscard]] auto put(storage::ArtifactWrite write,
                         std::span<const std::byte> content,
                         std::stop_token stop_token = {})
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override;

  [[nodiscard]] auto recorded_calls() const noexcept
      -> const std::vector<ArtifactStoreCall>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ArtifactStoreExchange> m_exchanges;
  std::vector<ArtifactStoreCall> m_recorded_calls;
  std::size_t m_next_exchange{};
};

}  // namespace aiforge::testing
