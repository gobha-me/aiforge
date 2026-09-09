#pragma once

#include <aiforge/domain/ops_observation.hpp>
#include <expected>
#include <string>
#include <string_view>

namespace aiforge::adapters {

inline constexpr std::size_t maximum_ops_observation_json_bytes =
    std::size_t{512} * 1024U;
inline constexpr std::size_t maximum_ops_request_json_bytes =
    std::size_t{16} * 1024U;
enum class OpsObservationJsonError {
  invalid_record,
  invalid_document,
  resource_exhausted,
  internal_failure
};

// Versioned strict JSON for neutral historical observations. Neither operation
// performs IO, handles credentials, constructs authority, or grants access.
[[nodiscard]] auto encode_ops_observation(const domain::OpsObservation& value)
    -> std::expected<std::string, OpsObservationJsonError>;
[[nodiscard]] auto decode_ops_observation(std::string_view document)
    -> std::expected<domain::OpsObservation, OpsObservationJsonError>;

// Independent version-1 historical request document, bounded to 16 KiB.
[[nodiscard]] auto encode_recorded_ops_request(
    const domain::OpsObservationRequest& value)
    -> std::expected<std::string, OpsObservationJsonError>;
[[nodiscard]] auto decode_recorded_ops_request(std::string_view document)
    -> std::expected<domain::OpsObservationRequest, OpsObservationJsonError>;

} // namespace aiforge::adapters
