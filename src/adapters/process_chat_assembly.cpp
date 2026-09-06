#include <aiforge/adapters/process_chat_assembly.hpp>

#include <cstdint>
#include <filesystem>
#include <utility>
#include <variant>

#include <aiforge/adapters/process_tool.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(std::string message)
    -> std::unexpected<ProcessChatAssemblyError> {
  return std::unexpected(ProcessChatAssemblyError{std::move(message)});
}

[[nodiscard]] auto unavailable_context(
    const runtime::RestrictionLevel restriction,
    const runtime::ApprovalMode approval,
    std::optional<std::string> matcher_policy_identity,
    const runtime::RestrictionUnavailableReason reason)
    -> std::expected<runtime::ApplicationLaunchContext,
                     ProcessChatAssemblyError> {
  runtime::ApplicationLaunchContextConfiguration configuration;
  configuration.selected_restriction = restriction;
  configuration.achieved_restriction.reset();
  configuration.unavailable_reason = reason;
  configuration.restriction_policy_identity.reset();
  configuration.approval_mode = approval;
  configuration.matcher_policy_identity = std::move(matcher_policy_identity);
  auto context =
      runtime::make_application_launch_context(std::move(configuration));
  if (!context) {
    return failure("process Chat launch context is invalid");
  }
  return std::move(*context);
}

[[nodiscard]] auto declare_unavailable(
    runtime::ToolRegistry& registry, runtime::ToolUnavailableReason reason,
    std::optional<runtime::ToolRestrictionUnavailability> restriction =
        std::nullopt) -> std::expected<void, ProcessChatAssemblyError> {
  auto declared = registry.declare_unavailable_tool(
      "run_process", {reason, std::move(restriction)},
      runtime::ToolCategory::process);
  if (!declared) return failure(declared.error().message);
  return {};
}

[[nodiscard]] auto unavailable_result(
    runtime::ToolRegistry& registry, runtime::ToolUnavailableReason reason,
    runtime::ApplicationLaunchContext context,
    std::optional<runtime::ToolRestrictionUnavailability> restriction =
        std::nullopt)
    -> std::expected<ProcessChatAssemblyResult, ProcessChatAssemblyError> {
  auto declared = declare_unavailable(registry, reason, std::move(restriction));
  if (!declared) return std::unexpected(std::move(declared.error()));
  return ProcessChatAssemblyResult{std::move(context), false};
}

[[nodiscard]] auto process_configuration(
    const config::ProcessConfigSettings& settings,
    const ProcessEnvironmentLookup& environment_lookup)
    -> std::optional<ProcessToolConfiguration> {
  ProcessToolConfiguration result;
  result.executable_allowlist.reserve(settings.executable_allowlist.size());
  for (const auto& path : settings.executable_allowlist) {
    result.executable_allowlist.emplace_back(path);
  }
  result.readable_roots.reserve(settings.readable_roots.size());
  for (const auto& path : settings.readable_roots) {
    result.readable_roots.emplace_back(path);
  }
  result.writable_roots.reserve(settings.writable_roots.size());
  for (const auto& path : settings.writable_roots) {
    result.writable_roots.emplace_back(path);
  }
  result.environment_allowlist.reserve(
      settings.inherited_environment_names.size());
  for (const auto& name : settings.inherited_environment_names) {
    auto value = environment_lookup ? environment_lookup(name) : std::nullopt;
    if (!value) return std::nullopt;
    result.environment_allowlist.push_back({name, std::move(*value)});
  }
  result.limits = {
      settings.limits.executables,
      settings.limits.arguments,
      settings.limits.argument_bytes,
      settings.limits.roots,
      settings.limits.environment_variables,
      settings.limits.timeout,
      settings.limits.output_bytes,
      settings.limits.inline_output_bytes,
      settings.limits.progress_chunk_bytes,
      settings.limits.progress_events,
      settings.limits.termination_grace,
  };
  return result;
}

[[nodiscard]] auto launch_bounds(const config::ProcessConfigLimits& limits)
    -> runtime::ProcessLaunchBounds {
  return {
      .maximum_arguments = limits.arguments,
      .maximum_argument_bytes = limits.argument_bytes,
      .maximum_roots = 2U * limits.roots,
      .maximum_environment_variables = limits.environment_variables,
      .maximum_path_bytes = 4096,
      .maximum_identity_bytes = 128,
      .maximum_wall_time = limits.timeout,
      .maximum_output_bytes = static_cast<std::uint64_t>(limits.output_bytes),
      .maximum_progress_chunk_bytes =
          static_cast<std::uint64_t>(limits.progress_chunk_bytes),
      .maximum_termination_grace = limits.termination_grace,
  };
}

} // namespace

auto configured_process_automatic_approval_rules(
    const std::optional<config::ProcessConfigSettings>& settings)
    -> std::vector<runtime::AutomaticApprovalRule> {
  std::vector<runtime::AutomaticApprovalRule> result;
  if (!settings || !settings->allowlist_automatic_approval_maximum_matches) {
    return result;
  }
  result.reserve(settings->executable_allowlist.size());
  for (const auto& executable : settings->executable_allowlist) {
    runtime::AutomaticApprovalRuleConstraints constraints;
    constraints.allowed_restrictions = {runtime::RestrictionLevel::none,
                                        runtime::RestrictionLevel::low};
    constraints.maximum_matches =
        *settings->allowlist_automatic_approval_maximum_matches;
    result.emplace_back(runtime::ProcessExecutableApprovalRule{
        executable, std::move(constraints)});
  }
  return result;
}

auto assemble_process_chat_tool(runtime::ToolRegistry& registry,
                                ProcessChatAssemblyRequest request)
    -> std::expected<ProcessChatAssemblyResult, ProcessChatAssemblyError> {
  try {
    auto shell = registry.declare_unavailable_tool(
        "run_shell",
        {runtime::ToolUnavailableReason::shell_unimplemented, std::nullopt},
        runtime::ToolCategory::process);
    if (!shell) return failure(shell.error().message);

    const auto make_unavailable = [&](const auto reason) {
      return unavailable_context(request.restriction, request.approval,
                                 request.matcher_policy_identity, reason);
    };
    if (!request.establish_launcher) {
      return failure("process launcher establishment is unavailable");
    }
    const config::ProcessConfigLimits bounds =
        request.settings ? request.settings->limits
                         : config::ProcessConfigLimits{};
    auto established = request.establish_launcher(
        {.restriction = request.restriction,
         .approval_mode = request.approval,
         .matcher_policy_identity = request.matcher_policy_identity,
         .bounds = launch_bounds(bounds)});

    std::optional<runtime::BoundProcessLauncher> launcher;
    std::optional<runtime::ApplicationLaunchContext> launch_context;
    std::optional<LinuxProcessLauncherEstablishmentErrorCode>
        establishment_error;
    if (!established) {
      establishment_error = established.error().code;
      const auto reason =
          established.error().code ==
                  LinuxProcessLauncherEstablishmentErrorCode::
                      unsupported_platform
              ? runtime::RestrictionUnavailableReason::unsupported_platform
              : runtime::RestrictionUnavailableReason::internal_error;
      auto context = make_unavailable(reason);
      if (!context) return std::unexpected(std::move(context.error()));
      launch_context.emplace(std::move(*context));
    } else if (auto* unavailable = std::get_if<LinuxProcessLauncherUnavailable>(
                   &*established)) {
      launch_context.emplace(std::move(unavailable->context));
    } else if (auto* bound =
                   std::get_if<runtime::BoundProcessLauncher>(&*established)) {
      launch_context.emplace(bound->context());
      launcher.emplace(std::move(*bound));
    }

    const bool context_matches =
        launch_context &&
        launch_context->selected_restriction() == request.restriction &&
        launch_context->approval_mode() == request.approval &&
        launch_context->matcher_policy_identity() ==
            request.matcher_policy_identity;
    if (!context_matches) {
      launcher.reset();
      auto context = make_unavailable(
          runtime::RestrictionUnavailableReason::internal_error);
      if (!context) return std::unexpected(std::move(context.error()));
      launch_context.emplace(std::move(*context));
      establishment_error =
          LinuxProcessLauncherEstablishmentErrorCode::internal_failure;
    }

    const auto unavailable = [&](const runtime::ToolUnavailableReason reason) {
      return unavailable_result(registry, reason, *launch_context);
    };
    if (!request.settings) {
      return unavailable(runtime::ToolUnavailableReason::not_configured);
    }
    if (!request.durable_session) {
      return unavailable(
          runtime::ToolUnavailableReason::durable_session_required);
    }
    if ((request.restriction == runtime::RestrictionLevel::none ||
         request.restriction == runtime::RestrictionLevel::low) &&
        !request.settings->unrestricted_network) {
      return unavailable(
          runtime::ToolUnavailableReason::unrestricted_network_required);
    }

    if (request.restriction != runtime::RestrictionLevel::none) {
      const auto reason = launch_context->unavailable_reason().value_or(
          runtime::RestrictionUnavailableReason::unsupported_combination);
      return unavailable_result(
          registry, runtime::ToolUnavailableReason::restriction_unavailable,
          *launch_context,
          runtime::ToolRestrictionUnavailability{request.restriction, reason});
    }
    if (!launcher) {
      const auto reason =
          establishment_error == LinuxProcessLauncherEstablishmentErrorCode::
                                     unsupported_platform
              ? runtime::ToolUnavailableReason::unsupported_platform
              : runtime::ToolUnavailableReason::
                    runtime_dependency_or_path_unavailable;
      return unavailable(reason);
    }
    if (request.artifact_store == nullptr) {
      return unavailable(runtime::ToolUnavailableReason::
                             runtime_dependency_or_path_unavailable);
    }
    auto tool_configuration =
        process_configuration(*request.settings, request.environment_lookup);
    if (!tool_configuration) {
      return unavailable(runtime::ToolUnavailableReason::
                             runtime_dependency_or_path_unavailable);
    }
    auto registered = register_process_tool(registry, *request.artifact_store,
                                            std::move(*tool_configuration),
                                            std::move(*launcher));
    if (!registered) {
      return unavailable(runtime::ToolUnavailableReason::
                             runtime_dependency_or_path_unavailable);
    }
    return ProcessChatAssemblyResult{std::move(*launch_context), true};
  } catch (...) {
    return failure("process Chat tool assembly failed internally");
  }
}

} // namespace aiforge::adapters
