#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <aiforge/backend/audio.hpp>
#include <aiforge/backend/backend.hpp>
#include <aiforge/storage/artifact_store.hpp>

namespace aiforge::adapters {

struct AudioBackendLimits {
  std::size_t maximum_text_bytes{1024U * 1024U};
  std::size_t maximum_audio_bytes{32U * 1024U * 1024U};
  auto operator==(const AudioBackendLimits&) const -> bool = default;
};

class SpeechBackend final : public backend::Backend {
 public:
  SpeechBackend(backend::AudioService& service,
                storage::ArtifactStore& artifact_store,
                domain::VoiceId voice_id,
                std::optional<std::string> language = {},
                AudioBackendLimits limits = {});

  [[nodiscard]] auto start(backend::BackendRequest request,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override;

 private:
  backend::AudioService& m_service;
  storage::ArtifactStore& m_artifact_store;
  domain::VoiceId m_voice_id;
  std::optional<std::string> m_language;
  AudioBackendLimits m_limits;
};

class TranscriptionBackend final : public backend::Backend {
 public:
  TranscriptionBackend(backend::AudioService& service,
                       storage::ArtifactStore& artifact_store,
                       domain::ArtifactMetadata input_artifact,
                       std::optional<std::string> language = {},
                       AudioBackendLimits limits = {});

  [[nodiscard]] auto start(backend::BackendRequest request,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override;

 private:
  backend::AudioService& m_service;
  storage::ArtifactStore& m_artifact_store;
  domain::ArtifactMetadata m_input_artifact;
  std::optional<std::string> m_language;
  AudioBackendLimits m_limits;
};

} // namespace aiforge::adapters
