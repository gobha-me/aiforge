#pragma once

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessImageCommand final : public cli::ImageCommand {
 public:
  [[nodiscard]] auto generate(GenerateRequest request,
                              cli::CommandEnvironment& environment,
                              std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
  [[nodiscard]] auto show(ShowRequest request,
                          cli::CommandEnvironment& environment,
                          std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
};

} // namespace aiforge::adapters
