#pragma once

#include <optional>
#include <string>
#include <utility>

#include <aiforge/runtime/application_launch_context.hpp>

namespace aiforge::testing {

[[nodiscard]] inline auto available_application_launch_context(
    const runtime::RestrictionLevel restriction,
    const runtime::ApprovalMode approval = runtime::ApprovalMode::prompt,
    std::optional<std::string> matcher_policy_identity = std::nullopt)
    -> runtime::ApplicationLaunchContext {
  runtime::ApplicationLaunchContextConfiguration configuration;
  configuration.selected_restriction = restriction;
  configuration.achieved_restriction = restriction;
  configuration.unavailable_reason.reset();
  configuration.restriction_policy_identity = "test.process-policy.v1";
  configuration.approval_mode = approval;
  configuration.matcher_policy_identity = std::move(matcher_policy_identity);
  return runtime::make_application_launch_context(std::move(configuration))
      .value();
}

} // namespace aiforge::testing
