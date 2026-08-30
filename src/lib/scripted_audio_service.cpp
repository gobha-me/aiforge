#include <aiforge/testing/scripted_audio_service.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto scripted_error(std::string message)
    -> std::unexpected<backend::AudioServiceError> {
  return std::unexpected(backend::AudioServiceError{
      backend::AudioServiceErrorCode::internal_failure, std::move(message),
      false, std::nullopt});
}

} // namespace

ScriptedAudioService::ScriptedAudioService(
    std::vector<SpeechSynthesisExchange> synthesis,
    std::vector<AudioTranscriptionExchange> transcription)
    : m_synthesis(std::move(synthesis)),
      m_transcription(std::move(transcription)) {
}

auto ScriptedAudioService::synthesize(backend::SpeechSynthesisRequest request,
                                      const std::stop_token stop_token)
    -> std::expected<backend::SynthesizedAudio, backend::AudioServiceError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::AudioServiceError{
          backend::AudioServiceErrorCode::cancelled,
          "scripted speech synthesis cancelled", false, std::nullopt});
    }
    m_recorded_synthesis.push_back(request);
    if (m_next_synthesis >= m_synthesis.size())
      return scripted_error("scripted speech synthesis is exhausted");
    const auto& exchange = m_synthesis[m_next_synthesis++];
    if (exchange.expected_request != request)
      return scripted_error("speech synthesis request did not match script");
    if (const auto* error =
            std::get_if<backend::AudioServiceError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<backend::SynthesizedAudio>(exchange.outcome);
  } catch (...) {
    return scripted_error("scripted speech synthesis failed internally");
  }
}

auto ScriptedAudioService::transcribe(
    backend::AudioTranscriptionRequest request,
    const std::stop_token stop_token)
    -> std::expected<backend::AudioTranscription, backend::AudioServiceError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::AudioServiceError{
          backend::AudioServiceErrorCode::cancelled,
          "scripted audio transcription cancelled", false, std::nullopt});
    }
    m_recorded_transcription.push_back(request);
    if (m_next_transcription >= m_transcription.size())
      return scripted_error("scripted audio transcription is exhausted");
    const auto& exchange = m_transcription[m_next_transcription++];
    if (exchange.expected_request != request)
      return scripted_error("audio transcription request did not match script");
    if (const auto* error =
            std::get_if<backend::AudioServiceError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<backend::AudioTranscription>(exchange.outcome);
  } catch (...) {
    return scripted_error("scripted audio transcription failed internally");
  }
}

auto ScriptedAudioService::recorded_synthesis() const noexcept
    -> const std::vector<backend::SpeechSynthesisRequest>& {
  return m_recorded_synthesis;
}

auto ScriptedAudioService::recorded_transcription() const noexcept
    -> const std::vector<backend::AudioTranscriptionRequest>& {
  return m_recorded_transcription;
}

} // namespace aiforge::testing
