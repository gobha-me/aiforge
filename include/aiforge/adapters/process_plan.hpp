#pragma once

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessPlanCommand final : public cli::PlanCommand {
public:
  [[nodiscard]] auto execute(Request request,
                             cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
};

} // namespace aiforge::adapters
