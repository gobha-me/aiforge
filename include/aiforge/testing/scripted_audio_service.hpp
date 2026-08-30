#pragma once

#include <variant>
#include <vector>

#include <aiforge/backend/audio.hpp>

namespace aiforge::testing {

using SpeechSynthesisOutcome =
    std::variant<backend::SynthesizedAudio, backend::AudioServiceError>;
using AudioTranscriptionOutcome =
    std::variant<backend::AudioTranscription, backend::AudioServiceError>;

struct SpeechSynthesisExchange {
  backend::SpeechSynthesisRequest expected_request;
  SpeechSynthesisOutcome outcome;
};

struct AudioTranscriptionExchange {
  backend::AudioTranscriptionRequest expected_request;
  AudioTranscriptionOutcome outcome;
};

class ScriptedAudioService final : public backend::AudioService {
 public:
  explicit ScriptedAudioService(
      std::vector<SpeechSynthesisExchange> synthesis = {},
      std::vector<AudioTranscriptionExchange> transcription = {});

  [[nodiscard]] auto synthesize(backend::SpeechSynthesisRequest request,
                                std::stop_token stop_token = {})
      -> std::expected<backend::SynthesizedAudio,
                       backend::AudioServiceError> override;
  [[nodiscard]] auto transcribe(backend::AudioTranscriptionRequest request,
                                std::stop_token stop_token = {})
      -> std::expected<backend::AudioTranscription,
                       backend::AudioServiceError> override;

  [[nodiscard]] auto recorded_synthesis() const noexcept
      -> const std::vector<backend::SpeechSynthesisRequest>&;
  [[nodiscard]] auto recorded_transcription() const noexcept
      -> const std::vector<backend::AudioTranscriptionRequest>&;

 private:
  std::vector<SpeechSynthesisExchange> m_synthesis;
  std::vector<AudioTranscriptionExchange> m_transcription;
  std::vector<backend::SpeechSynthesisRequest> m_recorded_synthesis;
  std::vector<backend::AudioTranscriptionRequest> m_recorded_transcription;
  std::size_t m_next_synthesis{};
  std::size_t m_next_transcription{};
};

} // namespace aiforge::testing
