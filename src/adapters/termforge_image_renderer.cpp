#include <aiforge/adapters/termforge_image_renderer.hpp>

#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <rasterforge/rasterforge.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(const ImageRenderErrorCode code, std::string message)
    -> std::unexpected<ImageRenderError> {
  return std::unexpected(ImageRenderError{code, std::move(message)});
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

[[nodiscard]] auto decoded_artifact(const storage::ArtifactRead& artifact)
    -> std::expected<rasterforge::DecodedImage, ImageRenderError> {
  if (!artifact.metadata.media_type.starts_with("image/") ||
      artifact.content.empty() ||
      artifact.metadata.byte_size != artifact.content.size()) {
    return failure(ImageRenderErrorCode::invalid_artifact,
                   "image artifact metadata is invalid");
  }
  rasterforge::DecodeOptions options;
  options.limits.max_input_bytes = 32U * 1024U * 1024U;
  options.limits.max_pixels = 16U * 1024U * 1024U;
  options.limits.max_output_bytes = 64U * 1024U * 1024U;
  options.limits.max_temporary_bytes = 64U * 1024U * 1024U;
  options.limits.max_dimension = 8192;
  auto decoded = rasterforge::decode(artifact.content, options);
  if (!decoded) {
    return failure(ImageRenderErrorCode::decode_failed,
                   "image artifact failed bounded decode");
  }
  if (media_type(decoded->format()) != artifact.metadata.media_type) {
    return failure(ImageRenderErrorCode::invalid_artifact,
                   "image artifact media type does not match its content");
  }
  const auto extent = decoded->output_extent();
  if (!artifact.metadata.width || !artifact.metadata.height ||
      *artifact.metadata.width != extent.width ||
      *artifact.metadata.height != extent.height ||
      extent.width >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      extent.height >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return failure(ImageRenderErrorCode::invalid_artifact,
                   "image artifact dimensions do not match decoded media");
  }
  return std::move(*decoded);
}

} // namespace

auto validate_image_artifact(const storage::ArtifactRead& artifact)
    -> std::expected<void, ImageRenderError> {
  try {
    auto decoded = decoded_artifact(artifact);
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    return {};
  } catch (...) {
    return failure(ImageRenderErrorCode::internal_failure,
                   "image validation failed internally");
  }
}

auto render_image_artifact(const storage::ArtifactRead& artifact,
                           termforge::TerminalDriver& driver,
                           const termforge::Rect destination)
    -> std::expected<ImageRenderResult, ImageRenderError> {
  try {
    if (destination.w <= 0 || destination.h <= 0) {
      return failure(ImageRenderErrorCode::unsupported_geometry,
                     "image destination is empty");
    }
    auto decoded = decoded_artifact(artifact);
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    const auto extent = decoded->output_extent();

    bool encoded_passthrough{};
    std::expected<void, termforge::ErrorEvent> rendered;
    if (artifact.metadata.media_type == "image/png" &&
        driver.supports_image_format(termforge::ImageFormat::Png)) {
      encoded_passthrough = true;
      rendered = driver.draw_image(
          destination,
          termforge::EncodedImage{termforge::ImageFormat::Png,
                                  artifact.content,
                                  {static_cast<int>(extent.width),
                                   static_cast<int>(extent.height)}});
    } else {
      std::vector<termforge::Pixel> pixels;
      pixels.reserve(static_cast<std::size_t>(extent.width) * extent.height);
      const auto view = decoded->view();
      for (std::uint32_t row{}; row < extent.height; ++row) {
        auto source = view.row(row);
        if (!source) {
          return failure(ImageRenderErrorCode::decode_failed,
                         "decoded image row is unavailable");
        }
        for (const auto pixel : *source) {
          pixels.push_back({pixel.r, pixel.g, pixel.b, pixel.a});
        }
      }
      rendered = driver.draw_image(
          destination,
          termforge::Image{static_cast<int>(extent.width),
                           static_cast<int>(extent.height), std::move(pixels)});
    }
    if (!rendered) {
      return failure(ImageRenderErrorCode::terminal_failure,
                     "terminal image rendering failed");
    }
    driver.flush();
    if (driver.take_output_error()) {
      return failure(ImageRenderErrorCode::terminal_failure,
                     "terminal image output failed");
    }
    return ImageRenderResult{encoded_passthrough, std::string{driver.name()}};
  } catch (...) {
    return failure(ImageRenderErrorCode::internal_failure,
                   "image rendering failed internally");
  }
}

} // namespace aiforge::adapters
