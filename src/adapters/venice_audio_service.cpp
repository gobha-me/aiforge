#include <aiforge/adapters/venice_audio_service.hpp>

#include <cstddef>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

#include <venice/client.hpp>
#include <venice/options.hpp>
#include <venice/types.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto mapped_error(const venice::Error& error,
                                const std::string_view operation)
    -> backend::AudioServiceError {
  using Code = backend::AudioServiceErrorCode;
  switch (error.kind) {
    case venice::ErrorKind::InvalidArg:
      return {Code::invalid_request,
              "Venice " + std::string{operation} + " request was invalid",
              false, error.status};
    case venice::ErrorKind::Auth:
      return {Code::authentication, "Venice authentication failed", false,
              error.status};
    case venice::ErrorKind::PaymentRequired:
      return {Code::payment_required,
              "Venice " + std::string{operation} + " requires payment", false,
              error.status};
    case venice::ErrorKind::RateLimited:
      return {Code::rate_limited,
              "Venice " + std::string{operation} + " was rate limited", true,
              error.status};
    case venice::ErrorKind::Network:
      return {Code::network,
              "Venice " + std::string{operation} + " network request failed",
              true, error.status};
    case venice::ErrorKind::Parse:
      return {Code::protocol,
              "Venice " + std::string{operation} + " response was invalid",
              false, error.status};
    case venice::ErrorKind::ResponseTooLarge:
      return {Code::protocol,
              "Venice " + std::string{operation} +
                  " response exceeded its byte limit",
              false, error.status};
    case venice::ErrorKind::Cancelled:
      return {Code::cancelled,
              "Venice " + std::string{operation} + " was cancelled", false,
              error.status};
    case venice::ErrorKind::Http:
      return {Code::unavailable, "Venice " + std::string{operation} + " failed",
              error.status >= 500, error.status};
  }
  return {Code::unavailable, "Venice audio operation failed", false,
          error.status};
}

} // namespace

struct VeniceAudioService::Impl {
  Impl(credentials::Secret credential, VeniceAudioServiceOptions value)
      : options(std::move(value)),
        client(std::move(credential).release(), options.base_url) {}

  VeniceAudioServiceOptions options;
  venice::Client client;
};

VeniceAudioService::VeniceAudioService(credentials::Secret credential,
                                       VeniceAudioServiceOptions options)
    : m_impl(
          std::make_unique<Impl>(std::move(credential), std::move(options))) {
}

VeniceAudioService::~VeniceAudioService() = default;
VeniceAudioService::VeniceAudioService(VeniceAudioService&&) noexcept = default;
auto VeniceAudioService::operator=(VeniceAudioService&&) noexcept
    -> VeniceAudioService& = default;

auto VeniceAudioService::synthesize(backend::SpeechSynthesisRequest request,
                                    const std::stop_token stop_token)
    -> std::expected<backend::SynthesizedAudio, backend::AudioServiceError> {
  try {
    using Code = backend::AudioServiceErrorCode;
    if (m_impl == nullptr || request.text.empty()) {
      return std::unexpected(backend::AudioServiceError{
          Code::invalid_request, "speech synthesis request is invalid", false,
          std::nullopt});
    }
    venice::SpeechRequest adapted;
    adapted.input = std::move(request.text);
    adapted.model = std::string{request.model_id.value()};
    adapted.voice = std::string{request.voice_id.value()};
    adapted.language = std::move(request.language);
    adapted.response_format = "wav";
    venice::CancelToken cancel;
    std::stop_callback cancellation{stop_token, [&] { cancel.cancel(); }};
    venice::RequestOptions options;
    options.connect_timeout = m_impl->options.connect_timeout;
    options.read_timeout = m_impl->options.read_timeout;
    options.write_timeout = m_impl->options.write_timeout;
    options.cancel = &cancel;
    auto generated = m_impl->client.generate_speech(adapted, options);
    if (!generated)
      return std::unexpected(
          mapped_error(generated.error(), "speech synthesis"));
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::AudioServiceError{
          Code::cancelled, "Venice speech synthesis was cancelled", false,
          std::nullopt});
    }
    std::vector<std::byte> bytes;
    bytes.reserve(generated->bytes.size());
    for (const unsigned char byte : generated->bytes)
      bytes.push_back(static_cast<std::byte>(byte));
    return backend::SynthesizedAudio{std::move(bytes), generated->media_type};
  } catch (...) {
    return std::unexpected(backend::AudioServiceError{
        backend::AudioServiceErrorCode::internal_failure,
        "Venice speech synthesis failed internally", false, std::nullopt});
  }
}

auto VeniceAudioService::transcribe(backend::AudioTranscriptionRequest request,
                                    const std::stop_token stop_token)
    -> std::expected<backend::AudioTranscription, backend::AudioServiceError> {
  try {
    using Code = backend::AudioServiceErrorCode;
    if (m_impl == nullptr || request.encoded.empty() ||
        request.media_type != "audio/wav") {
      return std::unexpected(backend::AudioServiceError{
          Code::invalid_request, "audio transcription request is invalid",
          false, std::nullopt});
    }
    std::string bytes;
    bytes.reserve(request.encoded.size());
    for (const auto byte : request.encoded)
      bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    venice::AudioTranscriptionRequest adapted;
    adapted.file = {std::move(bytes), "audio.wav", "audio/wav"};
    adapted.model = std::string{request.model_id.value()};
    adapted.response_format = "text";
    adapted.language = std::move(request.language);
    venice::CancelToken cancel;
    std::stop_callback cancellation{stop_token, [&] { cancel.cancel(); }};
    venice::RequestOptions options;
    options.connect_timeout = m_impl->options.connect_timeout;
    options.read_timeout = m_impl->options.read_timeout;
    options.write_timeout = m_impl->options.write_timeout;
    options.cancel = &cancel;
    auto transcribed = m_impl->client.transcribe_audio(adapted, options);
    if (!transcribed)
      return std::unexpected(
          mapped_error(transcribed.error(), "audio transcription"));
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::AudioServiceError{
          Code::cancelled, "Venice audio transcription was cancelled", false,
          std::nullopt});
    }
    return std::visit(
        [](auto&& result) -> backend::AudioTranscription {
          return {std::move(result.text)};
        },
        std::move(*transcribed));
  } catch (...) {
    return std::unexpected(backend::AudioServiceError{
        backend::AudioServiceErrorCode::internal_failure,
        "Venice audio transcription failed internally", false, std::nullopt});
  }
}

} // namespace aiforge::adapters
