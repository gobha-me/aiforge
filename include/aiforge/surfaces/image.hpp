#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/backend/backend.hpp>
#include <aiforge/domain/events.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::surfaces {

enum class ImageErrorCode {
  invalid_input,
  context_failed,
  run_failed,
  cancelled,
  internal_failure,
};

struct ImageError {
  ImageErrorCode code{ImageErrorCode::internal_failure};
  std::string message;
  auto operator==(const ImageError&) const -> bool = default;
};

struct ImageRequest {
  std::string prompt;
  domain::ModelId model_id;
  std::optional<domain::RunProvenance> provenance;
};

struct ImageResult {
  domain::SessionId session_id;
  domain::RunId run_id;
  domain::ArtifactMetadata artifact;
  auto operator==(const ImageResult&) const -> bool = default;
};

class ImageSurface final {
 public:
  ImageSurface(backend::Backend& backend, storage::SessionStore& session_store);

  [[nodiscard]] auto generate(ImageRequest request,
                              std::stop_token stop_token = {})
      -> std::expected<ImageResult, ImageError>;

 private:
  backend::Backend& m_backend;
  storage::SessionStore& m_session_store;
};

} // namespace aiforge::surfaces
