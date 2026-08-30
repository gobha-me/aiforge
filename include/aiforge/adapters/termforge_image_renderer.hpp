#pragma once

#include <expected>
#include <string>

#include <aiforge/storage/artifact_store.hpp>
#include <termforge/core/types.hpp>
#include <termforge/drivers/terminal_driver.hpp>

namespace aiforge::adapters {

enum class ImageRenderErrorCode {
  invalid_artifact,
  decode_failed,
  unsupported_geometry,
  terminal_failure,
  internal_failure,
};

struct ImageRenderError {
  ImageRenderErrorCode code{ImageRenderErrorCode::internal_failure};
  std::string message;
  auto operator==(const ImageRenderError&) const -> bool = default;
};

struct ImageRenderResult {
  bool encoded_passthrough{};
  std::string driver_name;
  auto operator==(const ImageRenderResult&) const -> bool = default;
};

[[nodiscard]] auto validate_image_artifact(
    const storage::ArtifactRead& artifact)
    -> std::expected<void, ImageRenderError>;

[[nodiscard]] auto render_image_artifact(const storage::ArtifactRead& artifact,
                                         termforge::TerminalDriver& driver,
                                         termforge::Rect destination)
    -> std::expected<ImageRenderResult, ImageRenderError>;

} // namespace aiforge::adapters
