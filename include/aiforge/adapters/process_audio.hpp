#pragma once

#include <expected>
#include <functional>
#include <memory>

#include <aiforge/audio/playback.hpp>
#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters {

class ProcessAudioCommand final : public cli::AudioCommand {
 public:
  using PlaybackFactory =
      std::function<std::expected<std::shared_ptr<audio::PlaybackPort>,
                                  cli::CommandFailure>()>;

  explicit ProcessAudioCommand(PlaybackFactory playback_factory = {});

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
  [[nodiscard]] auto play(PlayRequest request,
                          cli::CommandEnvironment& environment,
                          std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;

 private:
  PlaybackFactory m_playback_factory;
};

} // namespace aiforge::adapters
