#pragma once

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessInteractiveCommand final : public cli::InteractiveCommand {
 public:
  [[nodiscard]] auto execute(Request request,
                             cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
};

} // namespace aiforge::adapters
