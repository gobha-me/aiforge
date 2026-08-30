#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <aiforge/backend/backend.hpp>
#include <aiforge/backend/image_generator.hpp>
#include <aiforge/storage/artifact_store.hpp>

namespace aiforge::adapters {

struct ImageBackendOptions {
  std::optional<std::string> requested_media_type;
  std::size_t maximum_prompt_bytes{1024U * 1024U};
  std::size_t maximum_encoded_bytes{32U * 1024U * 1024U};
  std::uint64_t maximum_pixels{16U * 1024U * 1024U};
  std::uint64_t maximum_decoded_bytes{64U * 1024U * 1024U};
  std::uint64_t maximum_temporary_bytes{64U * 1024U * 1024U};
  std::uint32_t maximum_dimension{8192};
};

class ImageBackend final : public backend::Backend {
 public:
  ImageBackend(backend::ImageGenerator& generator,
               storage::ArtifactStore& artifact_store,
               ImageBackendOptions options);

  [[nodiscard]] auto start(backend::BackendRequest request,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override;

 private:
  backend::ImageGenerator& m_generator;
  storage::ArtifactStore& m_artifact_store;
  ImageBackendOptions m_options;
};

} // namespace aiforge::adapters
