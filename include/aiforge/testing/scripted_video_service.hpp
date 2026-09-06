#pragma once

#include <cstddef>
#include <stop_token>
#include <variant>
#include <vector>

#include <aiforge/backend/video.hpp>

namespace aiforge::testing {

struct VideoQuoteCall {
  domain::VideoGenerationSpec spec;
  auto operator==(const VideoQuoteCall&) const -> bool = default;
};

struct VideoQueueCall {
  domain::VideoOperationId operation_id;
  domain::VideoGenerationSpec spec;
  auto operator==(const VideoQueueCall&) const -> bool = default;
};

struct VideoRetrieveCall {
  domain::VideoJobId job_id;
  domain::ModelId model_id;
  auto operator==(const VideoRetrieveCall&) const -> bool = default;
};

struct VideoCleanupCall {
  domain::VideoJobId job_id;
  domain::ModelId model_id;
  auto operator==(const VideoCleanupCall&) const -> bool = default;
};

// The transient URL is deliberately absent from this captured call.
struct VideoTranscriptionCall {
  domain::VideoOperationId operation_id;
  domain::ModelId model_id;
  auto operator==(const VideoTranscriptionCall&) const -> bool = default;
};

using VideoServiceCall =
    std::variant<VideoQuoteCall, VideoQueueCall, VideoRetrieveCall,
                 VideoCleanupCall, VideoTranscriptionCall>;

template <typename Value>
using VideoScriptOutcome = std::variant<Value, backend::VideoServiceError>;

template <typename Call, typename Value> struct VideoServiceExchange {
  Call expected_call;
  VideoScriptOutcome<Value> outcome;
};

using VideoQuoteExchange =
    VideoServiceExchange<VideoQuoteCall, backend::VideoQuote>;
using VideoQueueExchange =
    VideoServiceExchange<VideoQueueCall, backend::VideoQueued>;
using VideoRetrieveExchange =
    VideoServiceExchange<VideoRetrieveCall, backend::VideoRetrieval>;
using VideoCleanupExchange =
    VideoServiceExchange<VideoCleanupCall, backend::VideoCleanup>;
using VideoTranscriptionExchange =
    VideoServiceExchange<VideoTranscriptionCall, backend::VideoTranscription>;

class ScriptedVideoService final : public backend::VideoService {
 public:
  explicit ScriptedVideoService(
      std::vector<VideoQuoteExchange> quotes = {},
      std::vector<VideoQueueExchange> queues = {},
      std::vector<VideoRetrieveExchange> retrievals = {},
      std::vector<VideoCleanupExchange> cleanups = {},
      std::vector<VideoTranscriptionExchange> transcriptions = {});

  [[nodiscard]] auto quote(const domain::VideoGenerationSpec& spec,
                           backend::VideoServiceCallOptions options = {})
      -> std::expected<backend::VideoQuote,
                       backend::VideoServiceError> override;
  [[nodiscard]] auto queue(const domain::VideoOperationId& operation_id,
                           const domain::VideoGenerationSpec& spec,
                           backend::VideoServiceCallOptions options = {})
      -> std::expected<backend::VideoQueued,
                       backend::VideoServiceError> override;
  [[nodiscard]] auto retrieve(const domain::VideoJobId& job_id,
                              const domain::ModelId& model_id,
                              backend::VideoServiceCallOptions options = {})
      -> std::expected<backend::VideoRetrieval,
                       backend::VideoServiceError> override;
  [[nodiscard]] auto cleanup(const domain::VideoJobId& job_id,
                             const domain::ModelId& model_id,
                             backend::VideoServiceCallOptions options = {})
      -> std::expected<backend::VideoCleanup,
                       backend::VideoServiceError> override;
  [[nodiscard]] auto transcribe(backend::VideoTranscriptionRequest request,
                                backend::VideoServiceCallOptions options = {})
      -> std::expected<backend::VideoTranscription,
                       backend::VideoServiceError> override;

  [[nodiscard]] auto recorded_calls() const noexcept
      -> const std::vector<VideoServiceCall>&;
  [[nodiscard]] auto recorded_options() const noexcept
      -> const std::vector<backend::VideoServiceCallOptions>&;
  auto request_stop_after_call(std::stop_source& source,
                               std::size_t call_number) noexcept -> void;

 private:
  struct QueueBinding {
    VideoQueueCall call;
    backend::VideoQueued outcome;
  };
  struct CleanupBinding {
    VideoCleanupCall call;
    backend::VideoCleanup outcome;
  };

  std::vector<VideoQuoteExchange> m_quotes;
  std::vector<VideoQueueExchange> m_queues;
  std::vector<VideoRetrieveExchange> m_retrievals;
  std::vector<VideoCleanupExchange> m_cleanups;
  std::vector<VideoTranscriptionExchange> m_transcriptions;
  std::vector<VideoServiceCall> m_recorded_calls;
  std::vector<backend::VideoServiceCallOptions> m_recorded_options;
  std::vector<QueueBinding> m_queue_bindings;
  std::vector<CleanupBinding> m_cleanup_bindings;
  std::size_t m_next_quote{};
  std::size_t m_next_queue{};
  std::size_t m_next_retrieval{};
  std::size_t m_next_cleanup{};
  std::size_t m_next_transcription{};
  std::stop_source* m_stop_source{};
  std::size_t m_stop_after_call{};
};

} // namespace aiforge::testing
