#include <aiforge/adapters/venice_image_generator.hpp>

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

[[nodiscard]] auto mapped_error(const venice::Error& error)
    -> backend::ImageGenerationError {
  using Code = backend::ImageGenerationErrorCode;
  switch (error.kind) {
    case venice::ErrorKind::InvalidArg:
      return {Code::invalid_request, "Venice image request was invalid", false,
              error.status};
    case venice::ErrorKind::Auth:
      return {Code::authentication, "Venice authentication failed", false,
              error.status};
    case venice::ErrorKind::PaymentRequired:
      return {Code::payment_required,
              "Venice image generation requires payment", false, error.status};
    case venice::ErrorKind::RateLimited:
      return {Code::rate_limited, "Venice image generation was rate limited",
              true, error.status};
    case venice::ErrorKind::Network:
      return {Code::network, "Venice image generation network request failed",
              true, error.status};
    case venice::ErrorKind::Parse:
      return {Code::protocol, "Venice image response was invalid", false,
              error.status};
    case venice::ErrorKind::Cancelled:
      return {Code::cancelled, "Venice image generation was cancelled", false,
              error.status};
    case venice::ErrorKind::Http:
      return {Code::unavailable, "Venice image generation failed",
              error.status >= 500, error.status};
  }
  return {Code::unavailable, "Venice image generation failed", false,
          error.status};
}

[[nodiscard]] auto format_name(const std::string_view media_type)
    -> std::optional<std::string> {
  if (media_type == "image/png") return "png";
  if (media_type == "image/jpeg") return "jpeg";
  if (media_type == "image/webp") return "webp";
  return std::nullopt;
}

} // namespace

struct VeniceImageGenerator::Impl {
  Impl(credentials::Secret credential, VeniceImageGeneratorOptions value)
      : options(std::move(value)),
        client(std::move(credential).release(), options.base_url) {}

  VeniceImageGeneratorOptions options;
  venice::Client client;
};

VeniceImageGenerator::VeniceImageGenerator(credentials::Secret credential,
                                           VeniceImageGeneratorOptions options)
    : m_impl(
          std::make_unique<Impl>(std::move(credential), std::move(options))) {
}

VeniceImageGenerator::~VeniceImageGenerator() = default;
VeniceImageGenerator::VeniceImageGenerator(VeniceImageGenerator&&) noexcept =
    default;
auto VeniceImageGenerator::operator=(VeniceImageGenerator&&) noexcept
    -> VeniceImageGenerator& = default;

auto VeniceImageGenerator::generate(backend::ImageGenerationRequest request,
                                    const std::stop_token stop_token)
    -> std::expected<backend::GeneratedImage, backend::ImageGenerationError> {
  try {
    using Code = backend::ImageGenerationErrorCode;
    if (m_impl == nullptr || request.prompt.empty() ||
        request.prompt.size() > 1024U * 1024U) {
      return std::unexpected(backend::ImageGenerationError{
          Code::invalid_request, "image generation request is invalid", false,
          std::nullopt});
    }
    std::optional<std::string> format;
    if (request.requested_media_type) {
      format = format_name(*request.requested_media_type);
      if (!format) {
        return std::unexpected(backend::ImageGenerationError{
            Code::invalid_request, "image media type is unsupported", false,
            std::nullopt});
      }
    }
    venice::ImageGenerationRequest adapted;
    adapted.model = std::string{request.model_id.value()};
    adapted.prompt = std::move(request.prompt);
    adapted.return_binary = true;
    adapted.variants = 1;
    adapted.format = std::move(format);

    venice::CancelToken cancel;
    std::stop_callback cancellation{stop_token, [&] { cancel.cancel(); }};
    venice::RequestOptions options;
    options.connect_timeout = m_impl->options.connect_timeout;
    options.read_timeout = m_impl->options.read_timeout;
    options.write_timeout = m_impl->options.write_timeout;
    options.cancel = &cancel;
    auto generated = m_impl->client.generate_image(adapted, options);
    if (!generated) return std::unexpected(mapped_error(generated.error()));
    const auto* media = std::get_if<venice::GeneratedImageMedia>(&*generated);
    if (media == nullptr) {
      return std::unexpected(backend::ImageGenerationError{
          Code::protocol,
          "Venice returned JSON when binary image output was required", false,
          std::nullopt});
    }
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::ImageGenerationError{
          Code::cancelled, "Venice image generation was cancelled", false,
          std::nullopt});
    }
    std::vector<std::byte> bytes;
    bytes.reserve(media->bytes.size());
    for (const unsigned char byte : media->bytes)
      bytes.push_back(static_cast<std::byte>(byte));
    return backend::GeneratedImage{std::move(bytes), media->media_type};
  } catch (...) {
    return std::unexpected(backend::ImageGenerationError{
        backend::ImageGenerationErrorCode::internal_failure,
        "Venice image generation failed internally", false, std::nullopt});
  }
}

} // namespace aiforge::adapters
