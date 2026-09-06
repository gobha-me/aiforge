#include <aiforge/testing/scripted_video_service.hpp>

#include <algorithm>
#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto failure(std::string message)
    -> std::unexpected<backend::VideoServiceError> {
  return std::unexpected(
      backend::VideoServiceError{backend::VideoServiceErrorCode::protocol,
                                 std::move(message), false, std::nullopt});
}

template <typename Value>
[[nodiscard]] auto result_from(const VideoScriptOutcome<Value>& outcome)
    -> std::expected<Value, backend::VideoServiceError> {
  if (const auto* error = std::get_if<backend::VideoServiceError>(&outcome)) {
    return std::unexpected(*error);
  }
  return std::get<Value>(outcome);
}

[[nodiscard]] auto valid_options(
    const backend::VideoServiceCallOptions& options) -> bool {
  return options.timeout > std::chrono::milliseconds::zero() &&
         options.maximum_response_bytes != 0;
}

[[nodiscard]] auto bounded_retrieval(
    const VideoScriptOutcome<backend::VideoRetrieval>& outcome,
    const std::size_t maximum_response_bytes)
    -> std::expected<backend::VideoRetrieval, backend::VideoServiceError> {
  auto result = result_from(outcome);
  if (result) {
    if (const auto* media = std::get_if<backend::VideoMedia>(&*result);
        media != nullptr && media->encoded.size() > maximum_response_bytes) {
      return failure("scripted video retrieval exceeded its response bound");
    }
  }
  return result;
}

[[nodiscard]] auto bounded_transcription(
    const VideoScriptOutcome<backend::VideoTranscription>& outcome,
    const std::size_t maximum_response_bytes)
    -> std::expected<backend::VideoTranscription, backend::VideoServiceError> {
  auto result = result_from(outcome);
  if (result) {
    const auto language_bytes =
        result->language ? result->language->size() : 0U;
    if (result->text.size() > maximum_response_bytes ||
        language_bytes > maximum_response_bytes - result->text.size()) {
      return failure(
          "scripted video transcription exceeded its response bound");
    }
  }
  return result;
}

} // namespace

ScriptedVideoService::ScriptedVideoService(
    std::vector<VideoQuoteExchange> quotes,
    std::vector<VideoQueueExchange> queues,
    std::vector<VideoRetrieveExchange> retrievals,
    std::vector<VideoCleanupExchange> cleanups,
    std::vector<VideoTranscriptionExchange> transcriptions)
    : m_quotes(std::move(quotes)), m_queues(std::move(queues)),
      m_retrievals(std::move(retrievals)), m_cleanups(std::move(cleanups)),
      m_transcriptions(std::move(transcriptions)) {
}

namespace {
auto maybe_request_stop(std::stop_source* source, const std::size_t after,
                        const std::size_t calls) noexcept -> void {
  if (source != nullptr && after == calls) source->request_stop();
}
} // namespace

auto ScriptedVideoService::quote(const domain::VideoGenerationSpec& spec,
                                 const backend::VideoServiceCallOptions options)
    -> std::expected<backend::VideoQuote, backend::VideoServiceError> {
  if (options.stop_token.stop_requested()) {
    return std::unexpected(backend::VideoServiceError{
        backend::VideoServiceErrorCode::cancelled,
        "scripted video quote cancelled", false, std::nullopt});
  }
  if (!valid_options(options)) return failure("scripted video options invalid");
  const VideoQuoteCall call{spec};
  m_recorded_calls.push_back(call);
  m_recorded_options.push_back(options);
  maybe_request_stop(m_stop_source, m_stop_after_call, m_recorded_calls.size());
  if (m_next_quote >= m_quotes.size())
    return failure("scripted video quote is exhausted");
  const auto& exchange = m_quotes[m_next_quote++];
  if (exchange.expected_call != call)
    return failure("video quote did not match script");
  return result_from(exchange.outcome);
}

auto ScriptedVideoService::queue(const domain::VideoOperationId& operation_id,
                                 const domain::VideoGenerationSpec& spec,
                                 const backend::VideoServiceCallOptions options)
    -> std::expected<backend::VideoQueued, backend::VideoServiceError> {
  if (options.stop_token.stop_requested()) {
    return std::unexpected(backend::VideoServiceError{
        backend::VideoServiceErrorCode::cancelled,
        "scripted video queue cancelled", false, std::nullopt});
  }
  if (!valid_options(options)) return failure("scripted video options invalid");
  const VideoQueueCall call{operation_id, spec};
  m_recorded_calls.push_back(call);
  m_recorded_options.push_back(options);
  maybe_request_stop(m_stop_source, m_stop_after_call, m_recorded_calls.size());
  const auto binding =
      std::ranges::find_if(m_queue_bindings, [&](const QueueBinding& value) {
        return value.call.operation_id == operation_id;
      });
  if (binding != m_queue_bindings.end()) {
    if (binding->call.spec != spec)
      return failure("video operation ID was reused with another spec");
    return binding->outcome;
  }
  if (m_next_queue >= m_queues.size())
    return failure("scripted video queue is exhausted");
  const auto& exchange = m_queues[m_next_queue++];
  if (exchange.expected_call != call)
    return failure("video queue did not match script");
  auto result = result_from(exchange.outcome);
  if (result) m_queue_bindings.push_back(QueueBinding{call, *result});
  return result;
}

auto ScriptedVideoService::retrieve(
    const domain::VideoJobId& job_id, const domain::ModelId& model_id,
    const backend::VideoServiceCallOptions options)
    -> std::expected<backend::VideoRetrieval, backend::VideoServiceError> {
  if (options.stop_token.stop_requested()) {
    return std::unexpected(backend::VideoServiceError{
        backend::VideoServiceErrorCode::cancelled,
        "scripted video retrieval cancelled", false, std::nullopt});
  }
  if (!valid_options(options)) return failure("scripted video options invalid");
  const VideoRetrieveCall call{job_id, model_id};
  m_recorded_calls.push_back(call);
  m_recorded_options.push_back(options);
  maybe_request_stop(m_stop_source, m_stop_after_call, m_recorded_calls.size());
  if (m_next_retrieval >= m_retrievals.size())
    return failure("scripted video retrieval is exhausted");
  const auto& exchange = m_retrievals[m_next_retrieval++];
  if (exchange.expected_call != call)
    return failure("video retrieval did not match script");
  return bounded_retrieval(exchange.outcome, options.maximum_response_bytes);
}

auto ScriptedVideoService::cleanup(
    const domain::VideoJobId& job_id, const domain::ModelId& model_id,
    const backend::VideoServiceCallOptions options)
    -> std::expected<backend::VideoCleanup, backend::VideoServiceError> {
  if (options.stop_token.stop_requested()) {
    return std::unexpected(backend::VideoServiceError{
        backend::VideoServiceErrorCode::cancelled,
        "scripted video cleanup cancelled", false, std::nullopt});
  }
  if (!valid_options(options)) return failure("scripted video options invalid");
  const VideoCleanupCall call{job_id, model_id};
  m_recorded_calls.push_back(call);
  m_recorded_options.push_back(options);
  maybe_request_stop(m_stop_source, m_stop_after_call, m_recorded_calls.size());
  const auto binding = std::ranges::find_if(
      m_cleanup_bindings,
      [&](const CleanupBinding& value) { return value.call == call; });
  if (binding != m_cleanup_bindings.end()) return binding->outcome;
  if (m_next_cleanup >= m_cleanups.size())
    return failure("scripted video cleanup is exhausted");
  const auto& exchange = m_cleanups[m_next_cleanup++];
  if (exchange.expected_call != call)
    return failure("video cleanup did not match script");
  auto result = result_from(exchange.outcome);
  if (result && result->completed)
    m_cleanup_bindings.push_back(CleanupBinding{call, *result});
  return result;
}

auto ScriptedVideoService::transcribe(
    backend::VideoTranscriptionRequest request,
    const backend::VideoServiceCallOptions options)
    -> std::expected<backend::VideoTranscription, backend::VideoServiceError> {
  if (options.stop_token.stop_requested()) {
    return std::unexpected(backend::VideoServiceError{
        backend::VideoServiceErrorCode::cancelled,
        "scripted video transcription cancelled", false, std::nullopt});
  }
  if (!valid_options(options)) return failure("scripted video options invalid");
  const VideoTranscriptionCall call{request.operation_id, request.model_id};
  m_recorded_calls.push_back(call);
  m_recorded_options.push_back(options);
  maybe_request_stop(m_stop_source, m_stop_after_call, m_recorded_calls.size());
  if (m_next_transcription >= m_transcriptions.size())
    return failure("scripted video transcription is exhausted");
  const auto& exchange = m_transcriptions[m_next_transcription++];
  if (exchange.expected_call != call)
    return failure("video transcription did not match script");
  return bounded_transcription(exchange.outcome,
                               options.maximum_response_bytes);
}

auto ScriptedVideoService::recorded_calls() const noexcept
    -> const std::vector<VideoServiceCall>& {
  return m_recorded_calls;
}

auto ScriptedVideoService::recorded_options() const noexcept
    -> const std::vector<backend::VideoServiceCallOptions>& {
  return m_recorded_options;
}

auto ScriptedVideoService::request_stop_after_call(
    std::stop_source& source, const std::size_t call_number) noexcept -> void {
  m_stop_source = &source;
  m_stop_after_call = call_number;
}

} // namespace aiforge::testing
