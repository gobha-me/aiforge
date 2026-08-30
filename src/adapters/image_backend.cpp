#include <aiforge/adapters/image_backend.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <rasterforge/rasterforge.hpp>

namespace aiforge::adapters {
namespace {

class ImageStream final : public backend::BackendStream {
 public:
  explicit ImageStream(std::vector<backend::BackendEvent> events)
      : m_events(std::move(events)) {}

  [[nodiscard]] auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::cancelled, "image request cancelled",
          false, std::nullopt});
    }
    if (m_next == m_events.size()) return std::nullopt;
    return std::move(m_events[m_next++]);
  }

 private:
  std::vector<backend::BackendEvent> m_events;
  std::size_t m_next{};
};

[[nodiscard]] auto backend_failure(const backend::BackendErrorKind kind,
                                   std::string message,
                                   const bool retryable = false,
                                   std::optional<int> status = std::nullopt)
    -> std::unexpected<backend::BackendError> {
  return std::unexpected(backend::BackendError{kind, std::move(message),
                                               retryable, std::move(status)});
}

[[nodiscard]] auto mapped_error(const backend::ImageGenerationError& error)
    -> std::unexpected<backend::BackendError> {
  using Code = backend::ImageGenerationErrorCode;
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
  return backend_failure(kind, error.redacted_message, error.retryable,
                         error.status_code);
}

[[nodiscard]] auto media_type(const rasterforge::ImageFormat format)
    -> std::string_view {
  switch (format) {
    case rasterforge::ImageFormat::png: return "image/png";
    case rasterforge::ImageFormat::jpeg: return "image/jpeg";
    case rasterforge::ImageFormat::webp: return "image/webp";
  }
  return {};
}

[[nodiscard]] auto valid_sha256_digest(const std::string_view digest) -> bool {
  return digest.starts_with("sha256:") && digest.size() == 71 &&
         std::ranges::all_of(digest.substr(7), [](const unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] auto prompt_from(const backend::BackendRequest& request)
    -> std::optional<std::string> {
  if (!request.tools.empty() || !request.options.extensions.empty() ||
      request.options.temperature || request.options.max_output_tokens ||
      request.options.seed || !request.assistant_continuation_state.empty()) {
    return std::nullopt;
  }
  const domain::Message* user_message{};
  for (const auto& entry : request.context.entries) {
    const auto& message = entry.message;
    if (message.role == domain::Role::system) continue;
    if (message.role != domain::Role::user || user_message != nullptr) {
      return std::nullopt;
    }
    user_message = &message;
  }
  if (user_message == nullptr || user_message->content.size() != 1 ||
      !user_message->tool_calls.empty() || user_message->invocation_id) {
    return std::nullopt;
  }
  const auto* text =
      std::get_if<domain::TextBlock>(&user_message->content.front());
  if (text == nullptr) return std::nullopt;
  return text->text;
}

} // namespace

ImageBackend::ImageBackend(backend::ImageGenerator& generator,
                           storage::ArtifactStore& artifact_store,
                           ImageBackendOptions options)
    : m_generator(generator), m_artifact_store(artifact_store),
      m_options(std::move(options)) {
}

auto ImageBackend::start(backend::BackendRequest request,
                         const std::stop_token stop_token)
    -> std::expected<std::unique_ptr<backend::BackendStream>,
                     backend::BackendError> {
  try {
    if (stop_token.stop_requested())
      return backend_failure(backend::BackendErrorKind::cancelled,
                             "image request cancelled");
    auto prompt = prompt_from(request);
    if (!prompt || prompt->empty() ||
        prompt->size() > m_options.maximum_prompt_bytes ||
        m_options.maximum_encoded_bytes == 0 || m_options.maximum_pixels == 0 ||
        m_options.maximum_decoded_bytes == 0 ||
        m_options.maximum_temporary_bytes == 0 ||
        m_options.maximum_dimension == 0) {
      return backend_failure(backend::BackendErrorKind::request_rejected,
                             "image request is invalid");
    }
    auto generated = m_generator.generate(
        {request.model_id, std::move(*prompt), m_options.requested_media_type},
        stop_token);
    if (!generated) return mapped_error(generated.error());
    if (generated->encoded.empty() ||
        generated->encoded.size() > m_options.maximum_encoded_bytes ||
        (m_options.requested_media_type &&
         generated->media_type != *m_options.requested_media_type)) {
      return backend_failure(backend::BackendErrorKind::protocol,
                             "generated image media is invalid");
    }
    if (stop_token.stop_requested()) {
      return backend_failure(backend::BackendErrorKind::cancelled,
                             "image request cancelled");
    }
    rasterforge::DecodeOptions decode_options;
    decode_options.limits.max_input_bytes = m_options.maximum_encoded_bytes;
    decode_options.limits.max_pixels = m_options.maximum_pixels;
    decode_options.limits.max_output_bytes = m_options.maximum_decoded_bytes;
    decode_options.limits.max_temporary_bytes =
        m_options.maximum_temporary_bytes;
    decode_options.limits.max_dimension = m_options.maximum_dimension;
    auto decoded = rasterforge::decode(generated->encoded, decode_options);
    if (!decoded || media_type(decoded->format()) != generated->media_type) {
      return backend_failure(backend::BackendErrorKind::protocol,
                             "generated image failed bounded validation");
    }
    if (stop_token.stop_requested())
      return backend_failure(backend::BackendErrorKind::cancelled,
                             "image request cancelled");
    const auto extent = decoded->output_extent();
    auto artifact_id = domain::ArtifactId::from(
        "image-" + std::string{request.inference_id.value()});
    if (!artifact_id) {
      return backend_failure(backend::BackendErrorKind::protocol,
                             "generated image identity is invalid");
    }
    const auto expected_artifact_id = *artifact_id;
    auto stored = m_artifact_store.put(
        {std::move(*artifact_id), generated->media_type, std::nullopt,
         request.inference_id, extent.width, extent.height},
        generated->encoded, stop_token);
    if (!stored) {
      const auto kind =
          stored.error().code == storage::ArtifactStoreErrorCode::cancelled
              ? backend::BackendErrorKind::cancelled
              : backend::BackendErrorKind::unavailable;
      return backend_failure(kind, "generated image could not be stored",
                             stored.error().retryable);
    }
    if (stored->artifact_id != expected_artifact_id ||
        stored->media_type != generated->media_type ||
        stored->byte_size != generated->encoded.size() ||
        !valid_sha256_digest(stored->digest) ||
        stored->producing_invocation_id ||
        stored->producing_inference_id != request.inference_id ||
        stored->width != extent.width || stored->height != extent.height) {
      return backend_failure(backend::BackendErrorKind::protocol,
                             "artifact store returned invalid image metadata");
    }
    std::vector<backend::BackendEvent> events;
    events.emplace_back(backend::ResponseStarted{
        "image:" + std::string{request.inference_id.value()}});
    events.emplace_back(backend::ArtifactProduced{
        std::move(*stored), std::string{"generated image"}});
    events.emplace_back(backend::ResponseFinished{domain::FinishReason::stop});
    return std::make_unique<ImageStream>(std::move(events));
  } catch (...) {
    return backend_failure(backend::BackendErrorKind::unavailable,
                           "image backend failed internally");
  }
}

} // namespace aiforge::adapters
