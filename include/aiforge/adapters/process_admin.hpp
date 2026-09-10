#pragma once
#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {
class ProcessAdminCommand final : public cli::AdminCommand {
 public:
  [[nodiscard]] auto execute(Request request,
                             cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& diagnostics)
      -> std::expected<void, cli::CommandFailure> override;
};
} // namespace aiforge::adapters
