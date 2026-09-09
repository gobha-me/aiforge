#pragma once

#include <aiforge/runtime/ops_observation_source.hpp>
#include <expected>
#include <stop_token>
#include <string_view>

namespace aiforge::adapters {

// Pure adapter-private projection of one successful structured response.
// Historical structure validation grants no collection/publication authority.
// The owning source separately checks known credentials and current authority.
[[nodiscard]] auto project_kubernetes_observation(
    const domain::OpsObservationRequest& request,
    domain::EventTimestamp started_at, domain::EventTimestamp completed_at,
    std::string_view response_json, std::stop_token stop = {}) noexcept
    -> std::expected<domain::OpsObservation,
                     runtime::OpsObservationSourceError>;

} // namespace aiforge::adapters
