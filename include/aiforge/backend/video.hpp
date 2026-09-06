#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/domain/video.hpp>

namespace aiforge::backend {

class TransientVideoUrl final {
 public:
  [[nodiscard]] static auto from(std::string value)
      -> std::expected<TransientVideoUrl, std::string>;
  [[nodiscard]] auto value() const noexcept -> std::string_view;

 private:
  explicit TransientVideoUrl(std::string value) : m_value(std::move(value)) {}
  std::string m_value;
};

struct VideoQuote {
  domain::MonetaryAmount amount;
  auto operator==(const VideoQuote&) const -> bool = default;
};

struct VideoQueued {
  domain::VideoJobId job_id;
  auto operator==(const VideoQueued&) const -> bool = default;
};

struct VideoStatus {
  domain::VideoJobState state{domain::VideoJobState::queued};
  auto operator==(const VideoStatus&) const -> bool = default;
};

struct VideoMedia {
  std::vector<std::byte> encoded;
  std::string media_type;
  auto operator==(const VideoMedia&) const -> bool = default;
};

using VideoRetrieval = std::variant<VideoStatus, VideoMedia>;

struct VideoCleanup {
  bool completed{};
  auto operator==(const VideoCleanup&) const -> bool = default;
};

struct VideoTranscriptionRequest {
  domain::VideoOperationId operation_id;
  domain::ModelId model_id;
  TransientVideoUrl url;
};

struct VideoTranscription {
  std::string text;
  std::optional<std::string> language;
  auto operator==(const VideoTranscription&) const -> bool = default;
};

enum class VideoServiceErrorCode {
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

struct VideoServiceError {
  VideoServiceErrorCode code{VideoServiceErrorCode::internal_failure};
  std::string redacted_message;
  bool retryable{};
  std::optional<int> status_code;
  auto operator==(const VideoServiceError&) const -> bool = default;
};

struct VideoServiceCallOptions {
  std::chrono::milliseconds timeout{std::chrono::minutes{10}};
  std::stop_token stop_token;
  std::size_t maximum_response_bytes{std::size_t{64} * std::size_t{1024}};
};

class VideoService {
 public:
  virtual ~VideoService() = default;

  // Every call must honor the positive elapsed-time and response-size ceilings
  // and cancellation token in its options before buffering a response.

  [[nodiscard]] virtual auto quote(const domain::VideoGenerationSpec& spec,
                                   VideoServiceCallOptions options = {})
      -> std::expected<VideoQuote, VideoServiceError> = 0;
  // Implementations must provide semantic idempotency: the same operation ID
  // and equal spec returns the same job; a different spec fails closed.
  [[nodiscard]] virtual auto queue(const domain::VideoOperationId& operation_id,
                                   const domain::VideoGenerationSpec& spec,
                                   VideoServiceCallOptions options = {})
      -> std::expected<VideoQueued, VideoServiceError> = 0;
  [[nodiscard]] virtual auto retrieve(const domain::VideoJobId& job_id,
                                      const domain::ModelId& model_id,
                                      VideoServiceCallOptions options = {})
      -> std::expected<VideoRetrieval, VideoServiceError> = 0;
  // Cleanup is retry-safe after an indeterminate response or persistence
  // failure. Repeating a completed cleanup reports completed.
  [[nodiscard]] virtual auto cleanup(const domain::VideoJobId& job_id,
                                     const domain::ModelId& model_id,
                                     VideoServiceCallOptions options = {})
      -> std::expected<VideoCleanup, VideoServiceError> = 0;
  [[nodiscard]] virtual auto transcribe(VideoTranscriptionRequest request,
                                        VideoServiceCallOptions options = {})
      -> std::expected<VideoTranscription, VideoServiceError> = 0;
};

} // namespace aiforge::backend
