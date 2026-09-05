#include <aiforge/runtime/application_launch_context.hpp>

#include <algorithm>
#include <ranges>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto failure(const ApplicationLaunchContextErrorCode code,
                           std::string message)
    -> std::unexpected<ApplicationLaunchContextError> {
  return std::unexpected(
      ApplicationLaunchContextError{code, std::move(message)});
}

[[nodiscard]] auto valid_level(const RestrictionLevel level) noexcept -> bool {
  switch (level) {
    case RestrictionLevel::high:
    case RestrictionLevel::medium:
    case RestrictionLevel::low:
    case RestrictionLevel::none: return true;
  }
  return false;
}

[[nodiscard]] auto valid_mode(const ApprovalMode mode) noexcept -> bool {
  switch (mode) {
    case ApprovalMode::prompt:
    case ApprovalMode::automatic:
    case ApprovalMode::allow_all: return true;
  }
  return false;
}

[[nodiscard]] auto valid_reason(
    const RestrictionUnavailableReason reason) noexcept -> bool {
  switch (reason) {
    case RestrictionUnavailableReason::unsupported_platform:
    case RestrictionUnavailableReason::unsupported_architecture:
    case RestrictionUnavailableReason::unsupported_kernel:
    case RestrictionUnavailableReason::missing_delegation:
    case RestrictionUnavailableReason::missing_controller:
    case RestrictionUnavailableReason::permission_denied:
    case RestrictionUnavailableReason::privilege_changed:
    case RestrictionUnavailableReason::mechanism_absent:
    case RestrictionUnavailableReason::unsupported_combination:
    case RestrictionUnavailableReason::setup_race:
    case RestrictionUnavailableReason::enforcement_failed:
    case RestrictionUnavailableReason::cleanup_failed:
    case RestrictionUnavailableReason::internal_error: return true;
  }
  return false;
}

[[nodiscard]] auto valid_identity(const std::string_view value) -> bool {
  return !value.empty() && value.size() <= 128U &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '.' ||
                  character == '_' || character == '-' || character == ':';
         });
}

} // namespace

auto make_application_launch_context(
    ApplicationLaunchContextConfiguration configuration)
    -> std::expected<ApplicationLaunchContext, ApplicationLaunchContextError> {
  try {
    if (!valid_level(configuration.selected_restriction) ||
        (configuration.achieved_restriction &&
         !valid_level(*configuration.achieved_restriction))) {
      return failure(ApplicationLaunchContextErrorCode::invalid_level,
                     "application restriction level is invalid");
    }
    if (!valid_mode(configuration.approval_mode)) {
      return failure(ApplicationLaunchContextErrorCode::invalid_mode,
                     "application approval mode is invalid");
    }
    if (configuration.unavailable_reason &&
        !valid_reason(*configuration.unavailable_reason)) {
      return failure(ApplicationLaunchContextErrorCode::invalid_state,
                     "application restriction reason is invalid");
    }
    if (configuration.achieved_restriction.has_value() ==
            configuration.unavailable_reason.has_value() ||
        (configuration.achieved_restriction &&
         *configuration.achieved_restriction !=
             configuration.selected_restriction)) {
      return failure(ApplicationLaunchContextErrorCode::invalid_state,
                     "application restriction state is inconsistent");
    }
    if (!valid_identity(configuration.mechanism.identity) ||
        !valid_identity(configuration.mechanism.version) ||
        (configuration.restriction_policy_identity &&
         !valid_identity(*configuration.restriction_policy_identity)) ||
        (configuration.matcher_policy_identity &&
         !valid_identity(*configuration.matcher_policy_identity))) {
      return failure(ApplicationLaunchContextErrorCode::invalid_identity,
                     "application launch identity is invalid");
    }
    const bool automatic =
        configuration.approval_mode == ApprovalMode::automatic;
    if (automatic != configuration.matcher_policy_identity.has_value() ||
        configuration.achieved_restriction.has_value() !=
            configuration.restriction_policy_identity.has_value()) {
      return failure(ApplicationLaunchContextErrorCode::invalid_state,
                     "application policy identities conflict with launch "
                     "state");
    }
    return ApplicationLaunchContext{std::move(configuration)};
  } catch (...) {
    return failure(ApplicationLaunchContextErrorCode::invalid_state,
                   "application launch context failed internally");
  }
}

} // namespace aiforge::runtime
