#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/adapters/linux_process_launcher.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/runtime/automatic_approval_matcher.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/storage/artifact_store.hpp>

namespace aiforge::adapters {

using ProcessEnvironmentLookup =
    std::function<std::optional<std::string>(std::string_view)>;
using ProcessLauncherEstablisher =
    std::function<std::expected<LinuxProcessLauncherEstablishment,
                                LinuxProcessLauncherEstablishmentError>(
        LinuxProcessLauncherConfiguration)>;

struct ProcessChatAssemblyRequest {
  bool durable_session{};
  std::optional<config::ProcessConfigSettings> settings;
  runtime::RestrictionLevel restriction{runtime::RestrictionLevel::high};
  runtime::ApprovalMode approval{runtime::ApprovalMode::prompt};
  std::optional<std::string> matcher_policy_identity;
  storage::ArtifactStore* artifact_store{};
  ProcessEnvironmentLookup environment_lookup;
  ProcessLauncherEstablisher establish_launcher;
};

struct ProcessChatAssemblyResult {
  runtime::ApplicationLaunchContext launch_context;
  bool process_registered{};
};

struct ProcessChatAssemblyError {
  std::string message;
  auto operator==(const ProcessChatAssemblyError&) const -> bool = default;
};

// Returns the bounded per-executable implicit rules authorized by process
// configuration. Absence is deny-all and never treats allow-list membership as
// approval.
[[nodiscard]] auto configured_process_automatic_approval_rules(
    const std::optional<config::ProcessConfigSettings>& settings)
    -> std::vector<runtime::AutomaticApprovalRule>;

// Performs the production Chat process-tool decision once. Unavailable states
// add metadata only; a callable declaration is registered only for a durable
// session with an achieved none launcher and complete explicit authority.
[[nodiscard]] auto assemble_process_chat_tool(
    runtime::ToolRegistry& registry, ProcessChatAssemblyRequest request)
    -> std::expected<ProcessChatAssemblyResult, ProcessChatAssemblyError>;

} // namespace aiforge::adapters
