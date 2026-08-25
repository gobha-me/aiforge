#pragma once

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessModelsCommand final : public cli::ModelsCommand {
 public:
  [[nodiscard]] auto execute(cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
};

}  // namespace aiforge::adapters
