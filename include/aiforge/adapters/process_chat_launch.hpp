#pragma once

#include <aiforge/cli/command_registry.hpp>
#include <aiforge/surfaces/agent.hpp>

namespace aiforge::adapters {

struct ProcessAgentExecution {
  const surfaces::AgentRequest& request;
  surfaces::AgentRecordSink& sink;
  std::optional<std::string> repository;
  bool terminal_attempted{};
};

// Owns the production adapters for the duration of either surface. Agent
// execution exits before constructing an interactive application or picker.
[[nodiscard]] auto execute_process_chat(
    cli::InteractiveCommand::Request request,
    cli::CommandEnvironment& environment, std::ostream& output,
    std::ostream& diagnostics, ProcessAgentExecution* agent = nullptr)
    -> std::expected<void, cli::CommandFailure>;

} // namespace aiforge::adapters
