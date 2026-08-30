#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/ids.hpp>

namespace aiforge::backend {

struct SpeechSynthesisRequest {
  domain::ModelId model_id;
  domain::VoiceId voice_id;
  std::string text;
  std::optional<std::string> language;
  auto operator==(const SpeechSynthesisRequest&) const -> bool = default;
};

struct SynthesizedAudio {
  std::vector<std::byte> encoded;
  std::string media_type;
  auto operator==(const SynthesizedAudio&) const -> bool = default;
};

struct AudioTranscriptionRequest {
  domain::ModelId model_id;
  std::vector<std::byte> encoded;
  std::string media_type;
  std::optional<std::string> language;
  auto operator==(const AudioTranscriptionRequest&) const -> bool = default;
};

struct AudioTranscription {
  std::string text;
  auto operator==(const AudioTranscription&) const -> bool = default;
};

enum class AudioServiceErrorCode {
  invalid_request,
  authentication,
  payment_required,
  rate_limited,
  network,
  protocol,
  cancelled,
  unavailable,
  internal_failure,
};

struct AudioServiceError {
  AudioServiceErrorCode code{AudioServiceErrorCode::internal_failure};
  std::string redacted_message;
  bool retryable{};
  std::optional<int> status_code;
  auto operator==(const AudioServiceError&) const -> bool = default;
};

class AudioService {
 public:
  virtual ~AudioService() = default;

  [[nodiscard]] virtual auto synthesize(SpeechSynthesisRequest request,
                                        std::stop_token stop_token = {})
      -> std::expected<SynthesizedAudio, AudioServiceError> = 0;
  [[nodiscard]] virtual auto transcribe(AudioTranscriptionRequest request,
                                        std::stop_token stop_token = {})
      -> std::expected<AudioTranscription, AudioServiceError> = 0;
};

} // namespace aiforge::backend
