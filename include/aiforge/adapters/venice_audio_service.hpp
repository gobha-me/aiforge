#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <aiforge/backend/audio.hpp>
#include <aiforge/credentials/credential.hpp>

namespace aiforge::adapters {

struct VeniceAudioServiceOptions {
  std::string base_url{"https://api.venice.ai/api/v1"};
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> read_timeout;
  std::optional<std::chrono::milliseconds> write_timeout;
};

class VeniceAudioService final : public backend::AudioService {
 public:
  explicit VeniceAudioService(credentials::Secret credential,
                              VeniceAudioServiceOptions options = {});
  ~VeniceAudioService() override;

  VeniceAudioService(const VeniceAudioService&) = delete;
  auto operator=(const VeniceAudioService&) -> VeniceAudioService& = delete;
  VeniceAudioService(VeniceAudioService&&) noexcept;
  auto operator=(VeniceAudioService&&) noexcept -> VeniceAudioService&;

  [[nodiscard]] auto synthesize(backend::SpeechSynthesisRequest request,
                                std::stop_token stop_token = {})
      -> std::expected<backend::SynthesizedAudio,
                       backend::AudioServiceError> override;
  [[nodiscard]] auto transcribe(backend::AudioTranscriptionRequest request,
                                std::stop_token stop_token = {})
      -> std::expected<backend::AudioTranscription,
                       backend::AudioServiceError> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::adapters
