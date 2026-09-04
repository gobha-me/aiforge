#include <aiforge/runtime/video_artifacts.hpp>

#include <algorithm>
#include <ranges>
#include <string_view>
#include <utility>

#include <aiforge/detail/sha256.hpp>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto failure(const VideoArtifactErrorCode code,
                           std::string message, const bool retryable = false)
    -> std::unexpected<VideoArtifactError> {
  return std::unexpected(
      VideoArtifactError{code, std::move(message), retryable});
}

[[nodiscard]] auto valid_limits(const video::Mp4Limits& limits) -> bool {
  return limits.maximum_bytes >= 8 && limits.maximum_boxes != 0 &&
         limits.maximum_nesting_depth != 0 && limits.maximum_tracks != 0 &&
         limits.maximum_compatible_brands != 0;
}

[[nodiscard]] auto digest_of(const std::span<const std::byte> content,
                             const std::stop_token stop_token)
    -> std::expected<std::string, VideoArtifactError> {
  constexpr std::size_t chunk_bytes = std::size_t{64} * 1024U;
  detail::Sha256 digest;
  std::size_t offset{};
  while (offset < content.size()) {
    if (stop_token.stop_requested()) {
      return failure(VideoArtifactErrorCode::cancelled,
                     "video artifact operation cancelled");
    }
    const auto count = std::min(chunk_bytes, content.size() - offset);
    digest.update(content.subspan(offset, count));
    offset += count;
  }
  return "sha256:" + digest.finish();
}

[[nodiscard]] auto valid_digest(const std::string_view digest) -> bool {
  return digest.starts_with("sha256:") && digest.size() == 71 &&
         std::ranges::all_of(digest.substr(7), [](const unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] auto mapped_store_error(const storage::ArtifactStoreError& error,
                                      const std::string_view operation)
    -> std::unexpected<VideoArtifactError> {
  using StoreCode = storage::ArtifactStoreErrorCode;
  auto code = VideoArtifactErrorCode::unavailable;
  switch (error.code) {
    case StoreCode::invalid_request:
      code = VideoArtifactErrorCode::invalid_request;
      break;
    case StoreCode::permission_denied:
      code = VideoArtifactErrorCode::permission_denied;
      break;
    case StoreCode::resource_exhausted:
      code = VideoArtifactErrorCode::resource_exhausted;
      break;
    case StoreCode::cancelled: code = VideoArtifactErrorCode::cancelled; break;
    case StoreCode::internal_failure:
      code = VideoArtifactErrorCode::internal_failure;
      break;
    case StoreCode::unavailable:
    case StoreCode::io_failure:
      code = VideoArtifactErrorCode::unavailable;
      break;
  }
  return failure(code, "video artifact " + std::string{operation} + " failed",
                 error.retryable);
}

[[nodiscard]] auto mapped_validation_error(const video::Mp4Error& error)
    -> std::unexpected<VideoArtifactError> {
  if (error.code == video::Mp4ErrorCode::invalid_limits) {
    return failure(VideoArtifactErrorCode::invalid_request,
                   "video artifact limits are invalid");
  }
  if (error.code == video::Mp4ErrorCode::cancelled) {
    return failure(VideoArtifactErrorCode::cancelled,
                   "video artifact operation cancelled");
  }
  return failure(VideoArtifactErrorCode::invalid_media,
                 "video artifact is not valid bounded MP4");
}

[[nodiscard]] auto valid_metadata(const domain::ArtifactMetadata& metadata,
                                  const video::Mp4Limits& limits) -> bool {
  return valid_limits(limits) && metadata.media_type == "video/mp4" &&
         metadata.byte_size != 0 &&
         metadata.byte_size <= limits.maximum_bytes &&
         valid_digest(metadata.digest) &&
         !(metadata.producing_invocation_id &&
           metadata.producing_inference_id) &&
         !metadata.width && !metadata.height;
}

} // namespace

auto publish_mp4_artifact(storage::ArtifactStore& store,
                          Mp4ArtifactPublication publication,
                          std::vector<std::byte> encoded,
                          const video::Mp4Limits limits,
                          const std::stop_token stop_token)
    -> std::expected<domain::ArtifactMetadata, VideoArtifactError> {
  try {
    if (publication.producing_invocation_id &&
        publication.producing_inference_id) {
      return failure(VideoArtifactErrorCode::invalid_request,
                     "video artifact producers are invalid");
    }
    auto validated = video::validate_mp4(encoded, limits, stop_token);
    if (!validated) return mapped_validation_error(validated.error());
    if (stop_token.stop_requested()) {
      return failure(VideoArtifactErrorCode::cancelled,
                     "video artifact publication cancelled");
    }
    const auto expected_id = publication.artifact_id;
    const auto expected_invocation = publication.producing_invocation_id;
    const auto expected_inference = publication.producing_inference_id;
    auto expected_digest = digest_of(encoded, stop_token);
    if (!expected_digest)
      return std::unexpected(std::move(expected_digest.error()));
    auto stored = store.put({std::move(publication.artifact_id), "video/mp4",
                             std::move(publication.producing_invocation_id),
                             std::move(publication.producing_inference_id),
                             std::nullopt, std::nullopt},
                            encoded, stop_token);
    if (!stored) return mapped_store_error(stored.error(), "publication");
    if (stored->artifact_id != expected_id ||
        stored->media_type != "video/mp4" ||
        stored->byte_size != encoded.size() ||
        stored->digest != *expected_digest ||
        stored->producing_invocation_id != expected_invocation ||
        stored->producing_inference_id != expected_inference || stored->width ||
        stored->height) {
      return failure(VideoArtifactErrorCode::integrity_failure,
                     "video artifact publication metadata is inconsistent");
    }
    return std::move(*stored);
  } catch (...) {
    return failure(VideoArtifactErrorCode::internal_failure,
                   "video artifact publication failed internally");
  }
}

auto load_mp4_artifact(storage::ArtifactStore& store,
                       const domain::ArtifactMetadata& metadata,
                       const video::Mp4Limits limits,
                       const std::stop_token stop_token)
    -> std::expected<storage::ArtifactRead, VideoArtifactError> {
  try {
    if (!valid_metadata(metadata, limits)) {
      return failure(VideoArtifactErrorCode::invalid_request,
                     "video artifact metadata or limits are invalid");
    }
    if (stop_token.stop_requested()) {
      return failure(VideoArtifactErrorCode::cancelled,
                     "video artifact load cancelled");
    }
    auto read = store.get(metadata, limits.maximum_bytes, stop_token);
    if (!read) return mapped_store_error(read.error(), "load");
    if (read->metadata != metadata ||
        read->content.size() != metadata.byte_size) {
      return failure(VideoArtifactErrorCode::integrity_failure,
                     "video artifact content or metadata is inconsistent");
    }
    auto digest = digest_of(read->content, stop_token);
    if (!digest) return std::unexpected(std::move(digest.error()));
    if (*digest != metadata.digest) {
      return failure(VideoArtifactErrorCode::integrity_failure,
                     "video artifact content or metadata is inconsistent");
    }
    auto validated = video::validate_mp4(read->content, limits, stop_token);
    if (!validated) return mapped_validation_error(validated.error());
    return std::move(*read);
  } catch (...) {
    return failure(VideoArtifactErrorCode::internal_failure,
                   "video artifact load failed internally");
  }
}

} // namespace aiforge::runtime
