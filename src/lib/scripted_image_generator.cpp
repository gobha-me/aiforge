#include <aiforge/testing/scripted_image_generator.hpp>

#include <utility>

namespace aiforge::testing {

ScriptedImageGenerator::ScriptedImageGenerator(
    std::vector<ImageGeneratorExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {
}

auto ScriptedImageGenerator::generate(backend::ImageGenerationRequest request,
                                      const std::stop_token stop_token)
    -> std::expected<backend::GeneratedImage, backend::ImageGenerationError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::ImageGenerationError{
          backend::ImageGenerationErrorCode::cancelled,
          "image generation cancelled", false, std::nullopt});
    }
    m_recorded_requests.push_back(request);
    if (m_next >= m_exchanges.size()) {
      return std::unexpected(backend::ImageGenerationError{
          backend::ImageGenerationErrorCode::unavailable,
          "scripted image generator has no exchange remaining", false,
          std::nullopt});
    }
    const auto& exchange = m_exchanges[m_next];
    if (exchange.expected_request != request) {
      return std::unexpected(backend::ImageGenerationError{
          backend::ImageGenerationErrorCode::internal_failure,
          "image generation request did not match the script", false,
          std::nullopt});
    }
    ++m_next;
    if (const auto* error =
            std::get_if<backend::ImageGenerationError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<backend::GeneratedImage>(exchange.outcome);
  } catch (...) {
    return std::unexpected(backend::ImageGenerationError{
        backend::ImageGenerationErrorCode::internal_failure,
        "scripted image generator failed internally", false, std::nullopt});
  }
}

auto ScriptedImageGenerator::recorded_requests() const noexcept
    -> const std::vector<backend::ImageGenerationRequest>& {
  return m_recorded_requests;
}

auto ScriptedImageGenerator::remaining_exchanges() const noexcept
    -> std::size_t {
  return m_exchanges.size() - m_next;
}

} // namespace aiforge::testing
