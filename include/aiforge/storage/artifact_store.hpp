#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::storage {

enum class ArtifactStoreErrorCode {
  invalid_request,
  unavailable,
  permission_denied,
  resource_exhausted,
  io_failure,
  cancelled,
  internal_failure,
};

struct ArtifactStoreError {
  ArtifactStoreErrorCode code{ArtifactStoreErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const ArtifactStoreError&) const -> bool = default;
};

struct ArtifactWrite {
  domain::ArtifactId artifact_id;
  std::string media_type;
  std::optional<domain::InvocationId> producing_invocation_id;
  std::optional<domain::InferenceId> producing_inference_id{};
  std::optional<std::uint32_t> width{};
  std::optional<std::uint32_t> height{};
  auto operator==(const ArtifactWrite&) const -> bool = default;
};

struct ArtifactRead {
  domain::ArtifactMetadata metadata;
  std::vector<std::byte> content;
  auto operator==(const ArtifactRead&) const -> bool = default;
};

class ArtifactStore {
 public:
  virtual ~ArtifactStore() = default;

  [[nodiscard]] virtual auto put(ArtifactWrite write,
                                 std::span<const std::byte> content,
                                 std::stop_token stop_token = {})
      -> std::expected<domain::ArtifactMetadata, ArtifactStoreError> = 0;
  [[nodiscard]] virtual auto get(const domain::ArtifactMetadata& metadata,
                                 std::size_t maximum_bytes,
                                 std::stop_token stop_token = {})
      -> std::expected<ArtifactRead, ArtifactStoreError> {
    static_cast<void>(metadata);
    static_cast<void>(maximum_bytes);
    static_cast<void>(stop_token);
    return std::unexpected(
        ArtifactStoreError{ArtifactStoreErrorCode::unavailable,
                           "artifact store does not support reads", false});
  }
};

} // namespace aiforge::storage
