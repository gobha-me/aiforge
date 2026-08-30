#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <aiforge/backend/image_generator.hpp>
#include <aiforge/credentials/credential.hpp>

namespace aiforge::adapters {

struct VeniceImageGeneratorOptions {
  std::string base_url{"https://api.venice.ai/api/v1"};
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> read_timeout;
  std::optional<std::chrono::milliseconds> write_timeout;
};

class VeniceImageGenerator final : public backend::ImageGenerator {
 public:
  explicit VeniceImageGenerator(credentials::Secret credential,
                                VeniceImageGeneratorOptions options = {});
  ~VeniceImageGenerator() override;

  VeniceImageGenerator(const VeniceImageGenerator&) = delete;
  auto operator=(const VeniceImageGenerator&) -> VeniceImageGenerator& = delete;
  VeniceImageGenerator(VeniceImageGenerator&&) noexcept;
  auto operator=(VeniceImageGenerator&&) noexcept -> VeniceImageGenerator&;

  [[nodiscard]] auto generate(backend::ImageGenerationRequest request,
                              std::stop_token stop_token = {})
      -> std::expected<backend::GeneratedImage,
                       backend::ImageGenerationError> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::adapters
