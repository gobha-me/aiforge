#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

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
  auto operator==(const ArtifactWrite&) const -> bool = default;
};

class ArtifactStore {
 public:
  virtual ~ArtifactStore() = default;

  [[nodiscard]] virtual auto put(ArtifactWrite write,
                                 std::span<const std::byte> content,
                                 std::stop_token stop_token = {})
      -> std::expected<domain::ArtifactMetadata, ArtifactStoreError> = 0;
};

} // namespace aiforge::storage
