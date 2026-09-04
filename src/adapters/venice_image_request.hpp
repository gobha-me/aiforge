#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>

#include <aiforge/backend/image_generator.hpp>

namespace venice {
class Client;
}

namespace aiforge::adapters::detail {

struct VeniceImageRequestOptions {
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> read_timeout;
  std::optional<std::chrono::milliseconds> write_timeout;
  std::size_t maximum_response_bytes{32U * 1024U * 1024U};
};

[[nodiscard]] auto generate_venice_image(
    venice::Client& client, backend::ImageGenerationRequest request,
    VeniceImageRequestOptions options, std::stop_token stop_token = {})
    -> std::expected<backend::GeneratedImage, backend::ImageGenerationError>;

} // namespace aiforge::adapters::detail
