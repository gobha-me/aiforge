#pragma once

#include <array>
#include <expected>
#include <memory>

#include <aiforge/runtime/ops_observation_source.hpp>

namespace aiforge::adapters {
struct LinuxOpsObservationSourceAccess;

// Fixed local Linux health, read-only systemd service observations and
// explicitly permitted exact-invocation journal window prefixes. No
// cgroup-budget, physical-host or Kubernetes observation support.
class LinuxOpsObservationSource final : public runtime::OpsObservationSource {
 public:
  // Implemented operations, not a successful environment/permission probe.
  static constexpr std::array supported_operations{
      domain::OpsObservationOperation::linux_health,
      domain::OpsObservationOperation::linux_services,
      domain::OpsObservationOperation::linux_service_health,
      domain::OpsObservationOperation::linux_service_logs};
  [[nodiscard]] static auto create(domain::OpsTargetId target,
                                   domain::OpsConfigurationRevision revision)
      -> std::expected<std::shared_ptr<LinuxOpsObservationSource>,
                       runtime::OpsObservationSourceError>;
  ~LinuxOpsObservationSource() override;
  [[nodiscard]] auto guarantees_bound_read_only_observations() const noexcept
      -> bool override;
  [[nodiscard]] auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override;
  [[nodiscard]] auto observe(const domain::OpsObservationRequest& request,
                             std::stop_token stop = {})
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override;

 private:
  friend struct LinuxOpsObservationSourceAccess;
  struct Impl;
  explicit LinuxOpsObservationSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

// Creation pins real procfs and the current thread's PID/mount namespaces.
// Nested PID-namespace proof permits container scope; absence of that proof is
// unknown, never host. Memory is kernel MemAvailable, not container headroom.
// Uptime requires stable, matching current/child time namespaces and a proven
// zero boot-time offset. Otherwise it is omitted as partial evidence.
// Stop, deadline and byte bounds are checked between finite primitive reads;
// they cannot interrupt a stalled kernel syscall. Service reads separately
// prove the fixed system bus and selected manager inside the observing worker;
// unavailable namespace/peer proof fails without affecting procfs health reads.
// Ordinary private OS authentication/NSS reads and blocked cleanup retain the
// same logical deadline versus physical source-slot limitation. Journal fields
// retain the same limitation; native mapping/decompression memory is not hard
// bounded, visibility remains partial, and heuristic text exclusions do not
// guarantee arbitrary log text is free of credentials.
} // namespace aiforge::adapters
