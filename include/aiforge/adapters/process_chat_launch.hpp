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

// Runs after production dependencies and the requested session are ready,
// before constructing any terminal application. Borrows the live session only
// for this callback; no single-request Agent lifecycle is involved.
struct ProcessHeadlessExecution {
  std::function<std::expected<void, cli::CommandFailure>(
      surfaces::ChatSession&)>
      run;
};

// Owns the production adapters for the duration of either surface. Agent
// execution exits before constructing an interactive application or picker.
[[nodiscard]] auto execute_process_chat(
    cli::InteractiveCommand::Request request,
    cli::CommandEnvironment& environment, std::ostream& output,
    std::ostream& diagnostics, ProcessAgentExecution* agent = nullptr,
    ProcessHeadlessExecution* headless = nullptr)
    -> std::expected<void, cli::CommandFailure>;

} // namespace aiforge::adapters
