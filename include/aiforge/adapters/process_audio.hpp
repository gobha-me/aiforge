#pragma once

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessAudioCommand final : public cli::AudioCommand {
 public:
  [[nodiscard]] auto synthesize(SynthesizeRequest request,
                                cli::CommandEnvironment& environment,
                                std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
  [[nodiscard]] auto transcribe(TranscribeRequest request,
                                cli::CommandEnvironment& environment,
                                std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
  [[nodiscard]] auto export_artifact(ExportRequest request,
                                     cli::CommandEnvironment& environment,
                                     std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
};

} // namespace aiforge::adapters
