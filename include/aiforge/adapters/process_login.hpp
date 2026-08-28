#pragma once

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessLoginCommand final : public cli::LoginCommand {
 public:
  [[nodiscard]] auto execute(cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
};

} // namespace aiforge::adapters
