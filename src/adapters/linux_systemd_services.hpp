#pragma once

#include "linux_systemd_bus.hpp"

namespace aiforge::adapters {
[[nodiscard]] auto verify_linux_systemd_service_identity(
    LinuxSystemdConnection& connection,
    const domain::LinuxServiceIdentity& selected, LinuxSystemdBudget& budget)
    -> std::expected<void, runtime::OpsObservationSourceError>;

// Adapter-private collection for the two closed service operations. The source
// validates its immutable target before calling; no response-selected paths,
// transport addresses or raw DBus values cross the neutral source boundary.
[[nodiscard]] auto validate_linux_systemd_service_request(
    const domain::OpsObservationRequest& request)
    -> std::expected<void, runtime::OpsObservationSourceError>;
[[nodiscard]] auto observe_linux_systemd_services(
    const domain::OpsObservationRequest& request, LinuxSystemdBus& bus,
    LinuxSystemdBudget& budget, domain::EventTimestamp started_at)
    -> std::expected<domain::OpsObservation,
                     runtime::OpsObservationSourceError>;
} // namespace aiforge::adapters
