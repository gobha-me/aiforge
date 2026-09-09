#pragma once

// Adapter-private deterministic probe seam. Production callers cannot choose a
// path or substitute a probe through the installed public factory.
#include <chrono>
#include <optional>
#include <string>

#include <aiforge/adapters/linux_ops_observation_source.hpp>

namespace aiforge::adapters {
enum class LinuxOpsInputFile { uptime, memory, time_offsets };
struct LinuxOpsTimeIdentity {
  std::uint64_t current;
  std::uint64_t children;
  auto operator==(const LinuxOpsTimeIdentity&) const -> bool = default;
};
struct LinuxOpsReadBudget {
  std::uint64_t remaining;
  std::chrono::steady_clock::time_point deadline;
  std::stop_token stop;
  [[nodiscard]] auto check() const
      -> std::expected<void, runtime::OpsObservationSourceError>;
  [[nodiscard]] auto consume(std::size_t bytes)
      -> std::expected<void, runtime::OpsObservationSourceError>;
};
class LinuxOpsProbe {
 public:
  virtual ~LinuxOpsProbe() = default;
  [[nodiscard]] virtual auto identity(LinuxOpsReadBudget& budget)
      -> std::expected<domain::LinuxOpsIdentity,
                       runtime::OpsObservationSourceError> = 0;
  [[nodiscard]] virtual auto read(LinuxOpsInputFile file,
                                  LinuxOpsReadBudget& budget)
      -> std::expected<std::optional<std::string>,
                       runtime::OpsObservationSourceError> = 0;
  [[nodiscard]] virtual auto time_identity(LinuxOpsReadBudget& budget)
      -> std::expected<std::optional<LinuxOpsTimeIdentity>,
                       runtime::OpsObservationSourceError> = 0;
};
struct LinuxOpsObservationSourceAccess {
  [[nodiscard]] static auto create(domain::OpsTargetId target,
                                   domain::OpsConfigurationRevision revision,
                                   std::shared_ptr<LinuxOpsProbe> probe)
      -> std::expected<std::shared_ptr<LinuxOpsObservationSource>,
                       runtime::OpsObservationSourceError>;
};
} // namespace aiforge::adapters
