#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/storage/artifact_store.hpp>
#include <aiforge/video/mp4.hpp>

namespace aiforge::runtime {

struct Mp4ArtifactPublication {
  domain::ArtifactId artifact_id;
  std::optional<domain::InvocationId> producing_invocation_id;
  std::optional<domain::InferenceId> producing_inference_id;
  auto operator==(const Mp4ArtifactPublication&) const -> bool = default;
};

enum class VideoArtifactErrorCode {
  invalid_request,
  invalid_media,
  unavailable,
  permission_denied,
  resource_exhausted,
  integrity_failure,
  cancelled,
  internal_failure,
};

struct VideoArtifactError {
  VideoArtifactErrorCode code{VideoArtifactErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const VideoArtifactError&) const -> bool = default;
};

[[nodiscard]] auto publish_mp4_artifact(storage::ArtifactStore& store,
                                        Mp4ArtifactPublication publication,
                                        std::vector<std::byte> encoded,
                                        video::Mp4Limits limits = {},
                                        std::stop_token stop_token = {})
    -> std::expected<domain::ArtifactMetadata, VideoArtifactError>;

[[nodiscard]] auto load_mp4_artifact(storage::ArtifactStore& store,
                                     const domain::ArtifactMetadata& metadata,
                                     video::Mp4Limits limits = {},
                                     std::stop_token stop_token = {})
    -> std::expected<storage::ArtifactRead, VideoArtifactError>;

} // namespace aiforge::runtime
