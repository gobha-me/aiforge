#pragma once

#include <cstddef>

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessOneShotCommand final : public cli::OneShotCommand {
 public:
  explicit ProcessOneShotCommand(std::size_t maximum_input_bytes = 1024U *
                                                                   1024U)
      : m_maximum_input_bytes(maximum_input_bytes) {}

  [[nodiscard]] auto execute(cli::OneShotCommand::Request request,
                             cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;

 private:
  std::size_t m_maximum_input_bytes;
};

} // namespace aiforge::adapters
