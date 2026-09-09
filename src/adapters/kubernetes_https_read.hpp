#pragma once
#include "static_kubernetes_config.hpp"
#include <aiforge/runtime/ops_observation_source.hpp>
#include <chrono>

namespace aiforge::adapters {
// Closed adapter-private read. The watcher spans TLS, capture, pure projection
// and exclusion; no raw response escapes this operation-shaped boundary.
[[nodiscard]] auto read_kubernetes_observation(
    const StaticKubernetesConfig& configuration,
    const domain::OpsObservationRequest& request,
    std::chrono::steady_clock::time_point deadline,
    domain::EventTimestamp started_at, std::stop_token stop) noexcept
    -> std::expected<domain::OpsObservation,
                     runtime::OpsObservationSourceError>;
} // namespace aiforge::adapters
