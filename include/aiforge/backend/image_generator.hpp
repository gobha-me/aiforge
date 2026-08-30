#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/ids.hpp>

namespace aiforge::backend {

struct ImageGenerationRequest {
  domain::ModelId model_id;
  std::string prompt;
  std::optional<std::string> requested_media_type;
  auto operator==(const ImageGenerationRequest&) const -> bool = default;
};

struct GeneratedImage {
  std::vector<std::byte> encoded;
  std::string media_type;
  auto operator==(const GeneratedImage&) const -> bool = default;
};

enum class ImageGenerationErrorCode {
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

struct ImageGenerationError {
  ImageGenerationErrorCode code{ImageGenerationErrorCode::internal_failure};
  std::string redacted_message;
  bool retryable{};
  std::optional<int> status_code;
  auto operator==(const ImageGenerationError&) const -> bool = default;
};

class ImageGenerator {
 public:
  virtual ~ImageGenerator() = default;
  [[nodiscard]] virtual auto generate(ImageGenerationRequest request,
                                      std::stop_token stop_token = {})
      -> std::expected<GeneratedImage, ImageGenerationError> = 0;
};

} // namespace aiforge::backend
