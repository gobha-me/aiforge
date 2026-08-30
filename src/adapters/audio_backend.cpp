#include <aiforge/adapters/audio_backend.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <aiforge/audio/wav.hpp>

namespace aiforge::adapters {
namespace {

class AudioStream final : public backend::BackendStream {
 public:
  explicit AudioStream(std::vector<backend::BackendEvent> events)
      : m_events(std::move(events)) {}

  [[nodiscard]] auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::cancelled, "audio request cancelled",
          false, std::nullopt});
    }
    if (m_next == m_events.size()) return std::nullopt;
    return std::move(m_events[m_next++]);
  }

 private:
  std::vector<backend::BackendEvent> m_events;
  std::size_t m_next{};
};

[[nodiscard]] auto failure(backend::BackendErrorKind kind, std::string message,
                           bool retryable = false,
                           std::optional<int> status = std::nullopt)
    -> std::unexpected<backend::BackendError> {
  return std::unexpected(backend::BackendError{kind, std::move(message),
                                               retryable, std::move(status)});
}

[[nodiscard]] auto mapped_error(const backend::AudioServiceError& error)
    -> std::unexpected<backend::BackendError> {
  using Code = backend::AudioServiceErrorCode;
  auto kind = backend::BackendErrorKind::unavailable;
  switch (error.code) {
    case Code::invalid_request:
      kind = backend::BackendErrorKind::request_rejected;
      break;
    case Code::authentication:
      kind = backend::BackendErrorKind::authentication;
      break;
    case Code::rate_limited:
      kind = backend::BackendErrorKind::rate_limited;
      break;
    case Code::network: kind = backend::BackendErrorKind::network; break;
    case Code::protocol: kind = backend::BackendErrorKind::protocol; break;
    case Code::cancelled: kind = backend::BackendErrorKind::cancelled; break;
    case Code::payment_required:
    case Code::unavailable:
    case Code::internal_failure:
      kind = backend::BackendErrorKind::unavailable;
      break;
  }
  return failure(kind, error.redacted_message, error.retryable,
                 error.status_code);
}

[[nodiscard]] auto safe_text(const std::string_view value,
                             const std::size_t maximum_bytes) -> bool {
  if (value.empty() || value.size() > maximum_bytes) return false;
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if ((first < 0x20U && first != '\n' && first != '\t') || first == 0x7FU) {
      return false;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto valid_language(const std::optional<std::string>& language)
    -> bool {
  if (!language) return true;
  if (language->empty() || language->size() > 64 || language->front() == '-' ||
      language->back() == '-') {
    return false;
  }
  bool hyphen{};
  for (const unsigned char byte : *language) {
    if (byte == '-') {
      if (hyphen) return false;
      hyphen = true;
    } else {
      if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9'))) {
        return false;
      }
      hyphen = false;
    }
  }
  return true;
}

[[nodiscard]] auto valid_request_shape(const backend::BackendRequest& request)
    -> bool {
  return request.tools.empty() && request.options.extensions.empty() &&
         !request.options.temperature && !request.options.max_output_tokens &&
         !request.options.seed && request.assistant_continuation_state.empty();
}

[[nodiscard]] auto one_user_message(const backend::BackendRequest& request)
    -> const domain::Message* {
  const domain::Message* user{};
  for (const auto& entry : request.context.entries) {
    if (entry.message.role == domain::Role::system) continue;
    if (entry.message.role != domain::Role::user || user != nullptr)
      return nullptr;
    user = &entry.message;
  }
  return user;
}

[[nodiscard]] auto valid_digest(const std::string_view digest) -> bool {
  return digest.starts_with("sha256:") && digest.size() == 71 &&
         std::ranges::all_of(digest.substr(7), [](const unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

} // namespace

SpeechBackend::SpeechBackend(backend::AudioService& service,
                             storage::ArtifactStore& artifact_store,
                             domain::VoiceId voice_id,
                             std::optional<std::string> language,
                             AudioBackendLimits limits)
    : m_service(service), m_artifact_store(artifact_store),
      m_voice_id(std::move(voice_id)), m_language(std::move(language)),
      m_limits(limits) {
}

auto SpeechBackend::start(backend::BackendRequest request,
                          const std::stop_token stop_token)
    -> std::expected<std::unique_ptr<backend::BackendStream>,
                     backend::BackendError> {
  try {
    const auto* user = one_user_message(request);
    if (stop_token.stop_requested())
      return failure(backend::BackendErrorKind::cancelled,
                     "speech synthesis cancelled");
    if (!valid_request_shape(request) || user == nullptr ||
        user->content.size() != 1 || !user->tool_calls.empty() ||
        user->invocation_id || m_limits.maximum_text_bytes == 0 ||
        m_limits.maximum_audio_bytes < 44 || !valid_language(m_language)) {
      return failure(backend::BackendErrorKind::request_rejected,
                     "speech synthesis request is invalid");
    }
    const auto* text = std::get_if<domain::TextBlock>(&user->content.front());
    if (text == nullptr ||
        !safe_text(text->text, m_limits.maximum_text_bytes)) {
      return failure(backend::BackendErrorKind::request_rejected,
                     "speech synthesis text is invalid");
    }
    auto synthesized = m_service.synthesize(
        {request.model_id, m_voice_id, text->text, m_language}, stop_token);
    if (!synthesized) return mapped_error(synthesized.error());
    if (synthesized->media_type != "audio/wav" ||
        synthesized->encoded.size() > m_limits.maximum_audio_bytes ||
        !audio::validate_pcm_wav(
            synthesized->encoded,
            {.maximum_bytes = m_limits.maximum_audio_bytes})) {
      return failure(backend::BackendErrorKind::protocol,
                     "synthesized audio is not valid bounded PCM WAV");
    }
    if (stop_token.stop_requested())
      return failure(backend::BackendErrorKind::cancelled,
                     "speech synthesis cancelled");
    auto artifact_id = domain::ArtifactId::from(
        "audio-" + std::string{request.inference_id.value()});
    if (!artifact_id)
      return failure(backend::BackendErrorKind::protocol,
                     "synthesized audio identity is invalid");
    const auto expected_id = *artifact_id;
    auto stored = m_artifact_store.put({std::move(*artifact_id), "audio/wav",
                                        std::nullopt, request.inference_id},
                                       synthesized->encoded, stop_token);
    if (!stored) {
      return failure(
          stored.error().code == storage::ArtifactStoreErrorCode::cancelled
              ? backend::BackendErrorKind::cancelled
              : backend::BackendErrorKind::unavailable,
          "synthesized audio could not be stored", stored.error().retryable);
    }
    if (stored->artifact_id != expected_id ||
        stored->media_type != "audio/wav" ||
        stored->byte_size != synthesized->encoded.size() ||
        !valid_digest(stored->digest) || stored->producing_invocation_id ||
        stored->producing_inference_id != request.inference_id ||
        stored->width || stored->height) {
      return failure(backend::BackendErrorKind::protocol,
                     "artifact store returned invalid audio metadata");
    }
    std::vector<backend::BackendEvent> events;
    events.emplace_back(backend::ResponseStarted{
        "speech:" + std::string{request.inference_id.value()}});
    events.emplace_back(backend::ArtifactProduced{
        std::move(*stored), std::string{"synthesized speech"}});
    events.emplace_back(backend::ResponseFinished{domain::FinishReason::stop});
    return std::make_unique<AudioStream>(std::move(events));
  } catch (...) {
    return failure(backend::BackendErrorKind::unavailable,
                   "speech synthesis failed internally");
  }
}

TranscriptionBackend::TranscriptionBackend(
    backend::AudioService& service, storage::ArtifactStore& artifact_store,
    domain::ArtifactMetadata input_artifact,
    std::optional<std::string> language, AudioBackendLimits limits)
    : m_service(service), m_artifact_store(artifact_store),
      m_input_artifact(std::move(input_artifact)),
      m_language(std::move(language)), m_limits(limits) {
}

auto TranscriptionBackend::start(backend::BackendRequest request,
                                 const std::stop_token stop_token)
    -> std::expected<std::unique_ptr<backend::BackendStream>,
                     backend::BackendError> {
  try {
    const auto* user = one_user_message(request);
    if (stop_token.stop_requested())
      return failure(backend::BackendErrorKind::cancelled,
                     "audio transcription cancelled");
    if (!valid_request_shape(request) || user == nullptr ||
        user->content.size() != 1 || !user->tool_calls.empty() ||
        user->invocation_id || m_limits.maximum_text_bytes == 0 ||
        m_limits.maximum_audio_bytes < 44 || !valid_language(m_language)) {
      return failure(backend::BackendErrorKind::request_rejected,
                     "audio transcription request is invalid");
    }
    const auto* reference =
        std::get_if<domain::ArtifactReferenceBlock>(&user->content.front());
    if (reference == nullptr ||
        reference->artifact_id != m_input_artifact.artifact_id ||
        m_input_artifact.media_type != "audio/wav" ||
        m_input_artifact.producing_invocation_id ||
        m_input_artifact.producing_inference_id || m_input_artifact.width ||
        m_input_artifact.height) {
      return failure(backend::BackendErrorKind::request_rejected,
                     "audio transcription artifact is invalid");
    }
    auto input = m_artifact_store.get(m_input_artifact,
                                      m_limits.maximum_audio_bytes, stop_token);
    if (!input) {
      return failure(input.error().code ==
                             storage::ArtifactStoreErrorCode::cancelled
                         ? backend::BackendErrorKind::cancelled
                         : backend::BackendErrorKind::unavailable,
                     "transcription input artifact could not be read",
                     input.error().retryable);
    }
    if (input->metadata != m_input_artifact ||
        !audio::validate_pcm_wav(
            input->content, {.maximum_bytes = m_limits.maximum_audio_bytes})) {
      return failure(backend::BackendErrorKind::protocol,
                     "transcription input is not valid bounded PCM WAV");
    }
    auto transcription = m_service.transcribe(
        {request.model_id, std::move(input->content), "audio/wav", m_language},
        stop_token);
    if (!transcription) return mapped_error(transcription.error());
    if (!safe_text(transcription->text, m_limits.maximum_text_bytes)) {
      return failure(backend::BackendErrorKind::protocol,
                     "audio transcript is invalid");
    }
    std::vector<backend::BackendEvent> events;
    events.emplace_back(backend::ResponseStarted{
        "transcription:" + std::string{request.inference_id.value()}});
    events.emplace_back(backend::ContentDelta{
        request.assistant_message_id,
        domain::TextBlock{std::move(transcription->text)}});
    events.emplace_back(backend::ResponseFinished{domain::FinishReason::stop});
    return std::make_unique<AudioStream>(std::move(events));
  } catch (...) {
    return failure(backend::BackendErrorKind::unavailable,
                   "audio transcription failed internally");
  }
}

} // namespace aiforge::adapters
