#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace aiforge::runtime {

enum class RestrictionLevel {
  high,
  medium,
  low,
  none,
};

enum class ApprovalMode {
  prompt,
  automatic,
  allow_all,
};

enum class RestrictionUnavailableReason {
  unsupported_platform,
  unsupported_architecture,
  unsupported_kernel,
  missing_delegation,
  missing_controller,
  permission_denied,
  privilege_changed,
  mechanism_absent,
  unsupported_combination,
  setup_race,
  enforcement_failed,
  cleanup_failed,
  internal_error,
};

enum class ProcessNetworkContract {
  unrestricted_new_sockets,
  deny_new_sockets,
};

struct LaunchMechanismContract {
  std::string identity{"aiforge.linux-restriction-levels"};
  std::string version{"0018"};
  auto operator==(const LaunchMechanismContract&) const -> bool = default;
};

struct ApplicationLaunchContextConfiguration {
  RestrictionLevel selected_restriction{RestrictionLevel::high};
  std::optional<RestrictionLevel> achieved_restriction{};
  std::optional<RestrictionUnavailableReason> unavailable_reason{
      RestrictionUnavailableReason::mechanism_absent};
  LaunchMechanismContract mechanism{};
  std::optional<std::string> restriction_policy_identity{};
  ApprovalMode approval_mode{ApprovalMode::prompt};
  std::optional<std::string> matcher_policy_identity{};
  auto operator==(const ApplicationLaunchContextConfiguration&) const
      -> bool = default;
};

enum class ApplicationLaunchContextErrorCode {
  invalid_level,
  invalid_mode,
  invalid_state,
  invalid_identity,
};

struct ApplicationLaunchContextError {
  ApplicationLaunchContextErrorCode code{
      ApplicationLaunchContextErrorCode::invalid_state};
  std::string message;
  auto operator==(const ApplicationLaunchContextError&) const -> bool = default;
};

// The context is created once at application launch and exposes no mutation.
// Durable provenance may describe it, but cannot recreate runtime authority.
class ApplicationLaunchContext final {
 public:
  auto operator==(const ApplicationLaunchContext& other) const -> bool {
    return m_configuration == other.m_configuration;
  }

  [[nodiscard]] auto selected_restriction() const noexcept -> RestrictionLevel {
    return m_configuration.selected_restriction;
  }
  [[nodiscard]] auto achieved_restriction() const noexcept
      -> const std::optional<RestrictionLevel>& {
    return m_configuration.achieved_restriction;
  }
  [[nodiscard]] auto unavailable_reason() const noexcept
      -> const std::optional<RestrictionUnavailableReason>& {
    return m_configuration.unavailable_reason;
  }
  [[nodiscard]] auto mechanism() const noexcept
      -> const LaunchMechanismContract& {
    return m_configuration.mechanism;
  }
  [[nodiscard]] auto approval_mode() const noexcept -> ApprovalMode {
    return m_configuration.approval_mode;
  }
  [[nodiscard]] auto restriction_policy_identity() const noexcept
      -> const std::optional<std::string>& {
    return m_configuration.restriction_policy_identity;
  }
  [[nodiscard]] auto matcher_policy_identity() const noexcept
      -> const std::optional<std::string>& {
    return m_configuration.matcher_policy_identity;
  }
  [[nodiscard]] auto process_restriction_available() const noexcept -> bool {
    return m_configuration.achieved_restriction.has_value();
  }
  [[nodiscard]] auto process_network_contract() const noexcept
      -> std::optional<ProcessNetworkContract> {
    if (!m_configuration.achieved_restriction) return std::nullopt;
    switch (*m_configuration.achieved_restriction) {
      case RestrictionLevel::none:
      case RestrictionLevel::low:
        return ProcessNetworkContract::unrestricted_new_sockets;
      case RestrictionLevel::medium:
      case RestrictionLevel::high:
        return ProcessNetworkContract::deny_new_sockets;
    }
    return std::nullopt;
  }

 private:
  explicit ApplicationLaunchContext(
      ApplicationLaunchContextConfiguration configuration)
      : m_configuration(std::move(configuration)) {}

  ApplicationLaunchContextConfiguration m_configuration;

  friend auto make_application_launch_context(
      ApplicationLaunchContextConfiguration configuration)
      -> std::expected<ApplicationLaunchContext, ApplicationLaunchContextError>;
};

[[nodiscard]] auto make_application_launch_context(
    ApplicationLaunchContextConfiguration configuration = {})
    -> std::expected<ApplicationLaunchContext, ApplicationLaunchContextError>;

} // namespace aiforge::runtime
