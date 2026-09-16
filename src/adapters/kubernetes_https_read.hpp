#pragma once
#include "static_kubernetes_config.hpp"
#include <aiforge/runtime/ops_observation_source.hpp>
#include <chrono>
#include <functional>

namespace aiforge::adapters {
struct KubernetesLogReadTestHooks {
  std::function<void()> before_credential_exclusion{};
};

// Closed adapter-private read. The watcher spans TLS, capture, pure projection
// and exclusion; no raw response escapes this operation-shaped boundary.
[[nodiscard]] auto read_kubernetes_observation(
    const StaticKubernetesConfig& configuration,
    const domain::OpsObservationRequest& request,
    std::chrono::steady_clock::time_point deadline,
    domain::EventTimestamp started_at, std::stop_token stop) noexcept
    -> std::expected<domain::OpsObservation,
                     runtime::OpsObservationSourceError>;
// A log observation uses one deadline for exact Pod identity, one bounded log
// response, and the repeated identity proof. No log bytes are returned unless
// both Pod UID and selected container runtime identity remain exact.
[[nodiscard]] auto read_kubernetes_log_observation(
    const StaticKubernetesConfig& configuration,
    const domain::OpsObservationRequest& request,
    std::chrono::steady_clock::time_point deadline,
    domain::EventTimestamp started_at, std::stop_token stop,
    const KubernetesLogReadTestHooks* test_hooks = nullptr) noexcept
    -> std::expected<domain::OpsObservation,
                     runtime::OpsObservationSourceError>;
} // namespace aiforge::adapters
