#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/backend/backend.hpp>
#include <aiforge/domain/events.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::surfaces {

enum class AudioErrorCode {
  invalid_input,
  context_failed,
  run_failed,
  cancelled,
  internal_failure,
};

struct AudioError {
  AudioErrorCode code{AudioErrorCode::internal_failure};
  std::string message;
  auto operator==(const AudioError&) const -> bool = default;
};

struct SpeechRequest {
  std::string text;
  domain::ModelId model_id;
  std::optional<domain::RunProvenance> provenance;
};

struct SpeechResult {
  domain::SessionId session_id;
  domain::RunId run_id;
  domain::ArtifactMetadata artifact;
};

class SpeechSurface final {
 public:
  SpeechSurface(backend::Backend& backend,
                storage::SessionStore& session_store);

  [[nodiscard]] auto synthesize(SpeechRequest request,
                                std::stop_token stop_token = {})
      -> std::expected<SpeechResult, AudioError>;

 private:
  backend::Backend& m_backend;
  storage::SessionStore& m_session_store;
};

struct TranscriptionRequest {
  domain::ArtifactMetadata input_artifact;
  domain::ModelId model_id;
  std::optional<domain::RunProvenance> provenance;
};

struct TranscriptionResult {
  domain::SessionId session_id;
  domain::RunId run_id;
  domain::ArtifactMetadata input_artifact;
  std::string text;
};

class TranscriptionSurface final {
 public:
  TranscriptionSurface(backend::Backend& backend,
                       storage::SessionStore& session_store);

  [[nodiscard]] auto transcribe(TranscriptionRequest request,
                                std::stop_token stop_token = {})
      -> std::expected<TranscriptionResult, AudioError>;

 private:
  backend::Backend& m_backend;
  storage::SessionStore& m_session_store;
};

} // namespace aiforge::surfaces
