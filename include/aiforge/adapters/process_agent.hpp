#pragma once

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {
class ProcessAgentCommand final : public cli::AgentCommand {
 public:
  [[nodiscard]] auto execute(Request options,
                             cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& diagnostics)
      -> std::expected<void, cli::CommandFailure> override;
};
} // namespace aiforge::adapters
