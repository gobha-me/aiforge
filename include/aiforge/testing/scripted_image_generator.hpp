#pragma once

#include <variant>
#include <vector>

#include <aiforge/backend/image_generator.hpp>

namespace aiforge::testing {

struct ImageGeneratorExchange {
  backend::ImageGenerationRequest expected_request;
  std::variant<backend::GeneratedImage, backend::ImageGenerationError> outcome;
  auto operator==(const ImageGeneratorExchange&) const -> bool = default;
};

class ScriptedImageGenerator final : public backend::ImageGenerator {
 public:
  explicit ScriptedImageGenerator(
      std::vector<ImageGeneratorExchange> exchanges = {});

  [[nodiscard]] auto generate(backend::ImageGenerationRequest request,
                              std::stop_token stop_token = {})
      -> std::expected<backend::GeneratedImage,
                       backend::ImageGenerationError> override;
  [[nodiscard]] auto recorded_requests() const noexcept
      -> const std::vector<backend::ImageGenerationRequest>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ImageGeneratorExchange> m_exchanges;
  std::vector<backend::ImageGenerationRequest> m_recorded_requests;
  std::size_t m_next{};
};

} // namespace aiforge::testing
