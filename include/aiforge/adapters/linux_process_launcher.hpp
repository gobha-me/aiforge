#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <aiforge/runtime/process_launcher.hpp>

namespace aiforge::adapters {

enum class LinuxProcessUnavailableConjunct {
  same_uid_broker_execution_confinement,
};

[[nodiscard]] auto linux_process_unavailable_conjunct_name(
    LinuxProcessUnavailableConjunct conjunct) noexcept -> std::string_view;

struct LinuxProcessLauncherConfiguration {
  runtime::RestrictionLevel restriction{runtime::RestrictionLevel::high};
  runtime::ApprovalMode approval_mode{runtime::ApprovalMode::prompt};
  std::optional<std::string> matcher_policy_identity;
  runtime::ProcessLaunchBounds bounds{};
  auto operator==(const LinuxProcessLauncherConfiguration&) const
      -> bool = default;
};

struct LinuxProcessLauncherUnavailable {
  runtime::ApplicationLaunchContext context;
  LinuxProcessUnavailableConjunct conjunct{
      LinuxProcessUnavailableConjunct::same_uid_broker_execution_confinement};
  auto operator==(const LinuxProcessLauncherUnavailable&) const
      -> bool = default;
};

using LinuxProcessLauncherEstablishment =
    std::variant<runtime::BoundProcessLauncher,
                 LinuxProcessLauncherUnavailable>;

enum class LinuxProcessLauncherEstablishmentErrorCode {
  invalid_configuration,
  unsupported_platform,
  binding_failed,
  internal_failure,
};

struct LinuxProcessLauncherEstablishmentError {
  LinuxProcessLauncherEstablishmentErrorCode code{
      LinuxProcessLauncherEstablishmentErrorCode::internal_failure};
  std::string message;
  auto operator==(const LinuxProcessLauncherEstablishmentError&) const
      -> bool = default;
};

// Establishment is evaluated once per application launch. Restricted levels
// return a successful immutable unavailable outcome before any Linux setup or
// payload mutation. Only `none` can carry an invocable launcher.
[[nodiscard]] auto establish_linux_process_launcher(
    LinuxProcessLauncherConfiguration configuration = {}) noexcept
    -> std::expected<LinuxProcessLauncherEstablishment,
                     LinuxProcessLauncherEstablishmentError>;

} // namespace aiforge::adapters
