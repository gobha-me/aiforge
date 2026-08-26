#include <aiforge/adapters/venice_backend.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>
#include <venice/client.hpp>
#include <venice/options.hpp>
#include <venice/stream.hpp>
#include <venice/types.hpp>

namespace aiforge::adapters {
namespace {

struct AdapterEnd {};
using AdapterItem =
    std::variant<backend::BackendEvent, backend::BackendError, AdapterEnd>;

[[nodiscard]] auto adapter_error(const backend::BackendErrorKind kind,
                                 std::string message,
                                 const bool retryable = false,
                                 std::optional<int> status = std::nullopt)
    -> backend::BackendError {
  return {kind, std::move(message), retryable, status};
}

[[nodiscard]] auto request_error(std::string message) -> backend::BackendError {
  return adapter_error(backend::BackendErrorKind::request_rejected,
                       std::move(message));
}

[[nodiscard]] auto protocol_error() -> backend::BackendError {
  return adapter_error(backend::BackendErrorKind::protocol,
                       "Venice returned an invalid stream", false);
}

[[nodiscard]] auto map_error(const venice::Error& error)
    -> backend::BackendError {
  switch (error.kind) {
    case venice::ErrorKind::Cancelled:
      return adapter_error(backend::BackendErrorKind::cancelled,
                           "Venice request cancelled", false, error.status);
    case venice::ErrorKind::Auth:
      return adapter_error(backend::BackendErrorKind::authentication,
                           "Venice authentication failed", false, error.status);
    case venice::ErrorKind::RateLimited:
      return adapter_error(backend::BackendErrorKind::rate_limited,
                           "Venice rate limit reached", true, error.status);
    case venice::ErrorKind::Network:
      return adapter_error(backend::BackendErrorKind::network,
                           "Venice network request failed", true, error.status);
    case venice::ErrorKind::InvalidArg:
      return request_error("Venice request was invalid");
    case venice::ErrorKind::Parse:
      return protocol_error();
    case venice::ErrorKind::Http:
      return adapter_error(backend::BackendErrorKind::unavailable,
                           "Venice request failed", error.status >= 500,
                           error.status);
    default:
      return adapter_error(backend::BackendErrorKind::unavailable,
                           "Venice request failed", false, error.status);
  }
}

[[nodiscard]] auto role_name(const domain::Role role) -> std::string_view {
  switch (role) {
    case domain::Role::system:
      return "system";
    case domain::Role::user:
      return "user";
    case domain::Role::assistant:
      return "assistant";
    case domain::Role::tool:
      return "tool";
    case domain::Role::evidence:
      return "user";
  }
  return "user";
}

[[nodiscard]] auto message_text(const domain::Message& message)
    -> std::expected<std::string, backend::BackendError> {
  std::string result;
  if (message.role == domain::Role::evidence) {
    result = "[Untrusted evidence]\n";
  }
  for (const auto& block : message.content) {
    if (const auto* text = std::get_if<domain::TextBlock>(&block)) {
      result.append(text->text);
      continue;
    }
    if (const auto* structured =
            std::get_if<domain::StructuredDataBlock>(&block);
        structured != nullptr && message.role == domain::Role::tool &&
        structured->media_type == "application/json") {
      result.append(structured->data);
      continue;
    }
    return std::unexpected(request_error(
        "Venice adapter does not support this input content block"));
  }
  return result;
}

[[nodiscard]] auto make_request(const backend::BackendRequest& request)
    -> std::expected<venice::ChatRequest, backend::BackendError> {
  if (!request.options.extensions.empty()) {
    return std::unexpected(
        request_error("Venice extensions are not enabled for this milestone"));
  }
  if (request.options.max_output_tokens &&
      *request.options.max_output_tokens >
          static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    return std::unexpected(
        request_error("max output tokens exceed Venice range"));
  }
  if (request.options.seed &&
      *request.options.seed > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(request_error("seed exceeds Venice range"));
  }

  venice::ChatRequest result;
  result.model = std::string{request.model_id.value()};
  result.temperature = request.options.temperature;
  if (request.options.max_output_tokens) {
    result.max_tokens = static_cast<int>(*request.options.max_output_tokens);
  }
  if (request.options.seed) {
    result.seed = static_cast<std::int64_t>(*request.options.seed);
  }

  result.messages.reserve(request.context.entries.size());
  for (const auto& entry : request.context.entries) {
    auto text = message_text(entry.message);
    if (!text) return std::unexpected(std::move(text.error()));
    venice::Message message;
    message.role = std::string{role_name(entry.message.role)};
    message.content = nlohmann::json(std::move(*text));
    if (entry.message.invocation_id) {
      message.tool_call_id = std::string{entry.message.invocation_id->value()};
    }
    result.messages.push_back(std::move(message));
  }
  if (result.messages.empty()) {
    return std::unexpected(
        request_error("Venice request has no context messages"));
  }

  if (!request.tools.empty()) {
    result.tools.emplace();
    result.tools->reserve(request.tools.size());
    for (const auto& tool : request.tools) {
      try {
        auto schema = nlohmann::json::parse(tool.input_schema.data);
        result.tools->push_back(venice::tools::function(
            tool.name, tool.description, std::move(schema)));
      } catch (...) {
        return std::unexpected(request_error("tool schema is not valid JSON"));
      }
    }
  }
  return result;
}

[[nodiscard]] auto finish_reason(const std::string_view reason)
    -> domain::FinishReason {
  if (reason == "stop") return domain::FinishReason::stop;
  if (reason == "length") return domain::FinishReason::length;
  if (reason == "tool_calls" || reason == "tool_call") {
    return domain::FinishReason::tool_call;
  }
  if (reason == "content_filter") return domain::FinishReason::content_filter;
  return domain::FinishReason::other;
}

[[nodiscard]] auto checked_usage(const venice::Usage& usage,
                                 const domain::Usage& previous)
    -> std::expected<domain::Usage, backend::BackendError> {
  if (usage.prompt_tokens < 0 || usage.completion_tokens < 0 ||
      (usage.cached_tokens && *usage.cached_tokens < 0) ||
      (usage.reasoning_tokens && *usage.reasoning_tokens < 0)) {
    return std::unexpected(protocol_error());
  }
  const domain::Usage cumulative{
      static_cast<std::uint64_t>(usage.prompt_tokens),
      static_cast<std::uint64_t>(usage.completion_tokens),
      static_cast<std::uint64_t>(usage.cached_tokens.value_or(0)),
      static_cast<std::uint64_t>(usage.reasoning_tokens.value_or(0))};
  if (cumulative.input_tokens < previous.input_tokens ||
      cumulative.output_tokens < previous.output_tokens ||
      cumulative.cached_input_tokens < previous.cached_input_tokens ||
      cumulative.reasoning_tokens < previous.reasoning_tokens) {
    return std::unexpected(protocol_error());
  }
  return domain::Usage{
      cumulative.input_tokens - previous.input_tokens,
      cumulative.output_tokens - previous.output_tokens,
      cumulative.cached_input_tokens - previous.cached_input_tokens,
      cumulative.reasoning_tokens - previous.reasoning_tokens};
}

[[nodiscard]] auto decimal_amount(const double value)
    -> std::expected<domain::DecimalAmount, backend::BackendError> {
  if (!std::isfinite(value) || value < 0.0) {
    return std::unexpected(protocol_error());
  }
  std::array<char, 128> buffer{};
  const auto converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (converted.ec != std::errc{}) return std::unexpected(protocol_error());
  auto amount = domain::DecimalAmount::from(std::string_view{
      buffer.data(),
      static_cast<std::size_t>(converted.ptr - buffer.data())});
  if (!amount) return std::unexpected(protocol_error());
  return *amount;
}

[[nodiscard]] auto checked_cost(const nlohmann::json& price)
    -> std::expected<std::optional<domain::ReportedCost>,
                     backend::BackendError> {
  std::vector<domain::MonetaryAmount> amounts;
  const auto append = [&](const char* field, const std::string_view unit)
      -> std::expected<void, backend::BackendError> {
    if (!price.contains(field) || price.at(field).is_null()) return {};
    if (!price.at(field).is_number()) {
      return std::unexpected(protocol_error());
    }
    const auto value = price.at(field).get<double>();
    auto decimal = decimal_amount(value);
    if (!decimal) return std::unexpected(decimal.error());
    auto amount = domain::MonetaryAmount::create(std::string{unit}, *decimal);
    if (!amount) return std::unexpected(protocol_error());
    amounts.push_back(std::move(*amount));
    return {};
  };
  if (auto result = append("usd", "USD"); !result) {
    return std::unexpected(result.error());
  }
  if (auto result = append("diem", "venice.diem"); !result) {
    return std::unexpected(result.error());
  }
  if (amounts.empty()) return std::optional<domain::ReportedCost>{};
  auto cost = domain::ReportedCost::create(std::move(amounts));
  if (!cost) return std::unexpected(protocol_error());
  return std::optional<domain::ReportedCost>{std::move(*cost)};
}

class VeniceStream final : public backend::BackendStream {
 public:
  VeniceStream(venice::Client& client, venice::ChatRequest request,
               domain::MessageId assistant_message_id,
               const VeniceBackendOptions& options)
      : m_client(client),
        m_request(std::move(request)),
        m_assistant_message_id(std::move(assistant_message_id)),
        m_capacity(options.pending_events),
        m_options{options.connect_timeout, options.read_timeout,
                  options.write_timeout, &m_cancel} {
    m_worker = std::jthread([this] {
      try {
        produce();
      } catch (...) {
        try {
          static_cast<void>(
              emit(adapter_error(backend::BackendErrorKind::unavailable,
                                 "Venice stream failed internally")));
          static_cast<void>(emit(AdapterEnd{}));
        } catch (...) {
        }
      }
    });
  }

  ~VeniceStream() override {
    {
      std::lock_guard lock(m_mutex);
      m_closed = true;
    }
    m_cancel.cancel();
    m_ready.notify_all();
    m_space.notify_all();
    m_worker.request_stop();
    if (m_worker.joinable()) m_worker.join();
  }

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    std::stop_callback cancelled{stop_token, [this] {
                                   m_cancel.cancel();
                                   m_ready.notify_all();
                                 }};
    std::unique_lock lock(m_mutex);
    m_ready.wait(lock, [&] { return m_closed || !m_items.empty(); });
    if (m_items.empty()) {
      return std::unexpected(adapter_error(backend::BackendErrorKind::cancelled,
                                           "Venice stream closed"));
    }
    auto item = std::move(m_items.front());
    m_items.pop_front();
    lock.unlock();
    m_space.notify_one();

    if (auto* event = std::get_if<backend::BackendEvent>(&item)) {
      return std::optional<backend::BackendEvent>{std::move(*event)};
    }
    if (auto* error = std::get_if<backend::BackendError>(&item)) {
      return std::unexpected(std::move(*error));
    }
    return std::optional<backend::BackendEvent>{};
  }

 private:
  auto emit(AdapterItem item) -> bool {
    std::unique_lock lock(m_mutex);
    m_space.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });
    if (m_closed) return false;
    m_items.push_back(std::move(item));
    lock.unlock();
    m_ready.notify_one();
    return true;
  }

  auto produce() -> void {
    venice::StreamAccumulator accumulator{/*keep_chunks=*/false};
    bool response_started{};
    bool finish_observed{};
    std::optional<backend::BackendError> local_error;
    std::map<int, domain::InvocationId> invocation_by_index;
    domain::Usage cumulative_usage;
    bool cost_emitted{};

    const auto ensure_started = [&] -> bool {
      if (response_started) return true;
      response_started = true;
      return emit(backend::ResponseStarted{"venice-response"});
    };

    const auto observe = [&](const venice::StreamDelta& delta) -> bool {
      if (!ensure_started()) return false;
      if (delta.reasoning_content &&
          !emit(backend::ReasoningDelta{std::string{*delta.reasoning_content},
                                        {}})) {
        return false;
      }
      if (delta.content &&
          !emit(backend::ContentDelta{
              m_assistant_message_id,
              domain::TextBlock{std::string{*delta.content}}})) {
        return false;
      }
      if (delta.refusal && !emit(backend::ContentDelta{
                               m_assistant_message_id,
                               domain::StructuredDataBlock{
                                   "application/vnd.aiforge.refusal+text",
                                   std::string{*delta.refusal}}})) {
        return false;
      }

      for (const auto& tool : delta.tool_calls) {
        std::optional<domain::InvocationId> invocation;
        if (!tool.id.empty()) {
          auto parsed = domain::InvocationId::from(tool.id);
          if (!parsed) {
            local_error = protocol_error();
            return false;
          }
          invocation = std::move(*parsed);
          if (tool.index)
            invocation_by_index.insert_or_assign(*tool.index, *invocation);
        } else if (tool.index && invocation_by_index.contains(*tool.index)) {
          invocation = invocation_by_index.at(*tool.index);
        }
        if (!invocation) {
          local_error = protocol_error();
          return false;
        }
        if (!emit(backend::ToolCallDelta{std::move(*invocation), tool.name,
                                         tool.arguments})) {
          return false;
        }
      }

      if (delta.usage != nullptr) {
        try {
          const auto parsed = delta.usage->get<venice::Usage>();
          auto incremental = checked_usage(parsed, cumulative_usage);
          if (!incremental) {
            local_error = std::move(incremental.error());
            return false;
          }
          cumulative_usage.input_tokens += incremental->input_tokens;
          cumulative_usage.output_tokens += incremental->output_tokens;
          cumulative_usage.cached_input_tokens +=
              incremental->cached_input_tokens;
          cumulative_usage.reasoning_tokens += incremental->reasoning_tokens;
          if (!emit(backend::UsageObserved{*incremental})) return false;
        } catch (...) {
          local_error = protocol_error();
          return false;
        }
      }

      if (delta.cost != nullptr) {
        try {
          auto cost = checked_cost(*delta.cost);
          if (!cost) {
            local_error = std::move(cost.error());
            return false;
          }
          if (*cost) {
            if (cost_emitted) {
              local_error = protocol_error();
              return false;
            }
            cost_emitted = true;
            if (!emit(backend::CostObserved{std::move(**cost)})) return false;
          }
        } catch (...) {
          local_error = protocol_error();
          return false;
        }
      }

      if (delta.finish_reason) {
        // Venice can send finish_reason before trailing empty-choice usage and
        // cost frames. ResponseFinished is terminal in the neutral protocol,
        // so observe it here and emit it only after chat_stream returns.
        if (finish_observed) {
          local_error = protocol_error();
          return false;
        }
        finish_observed = true;
      }
      return true;
    };

    auto response =
        m_client.chat_stream(m_request, accumulator, observe, m_options);
    if (local_error) {
      static_cast<void>(emit(std::move(*local_error)));
      static_cast<void>(emit(AdapterEnd{}));
      return;
    }
    if (!response) {
      static_cast<void>(emit(map_error(response.error())));
      static_cast<void>(emit(AdapterEnd{}));
      return;
    }
    if (!ensure_started()) return;
    if (!finish_observed || response->finish_reason.empty()) {
      static_cast<void>(emit(protocol_error()));
    } else {
      static_cast<void>(emit(
          backend::ResponseFinished{finish_reason(response->finish_reason)}));
    }
    static_cast<void>(emit(AdapterEnd{}));
  }

  venice::Client& m_client;
  venice::ChatRequest m_request;
  domain::MessageId m_assistant_message_id;
  std::size_t m_capacity;
  venice::CancelToken m_cancel;
  venice::RequestOptions m_options;
  std::jthread m_worker;
  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::condition_variable m_space;
  std::deque<AdapterItem> m_items;
  bool m_closed{};
};

}  // namespace

struct VeniceBackend::Impl {
  explicit Impl(credentials::Secret credential,
                VeniceBackendOptions backend_options)
      : options(std::move(backend_options)),
        client(std::move(credential).release(), options.base_url) {}

  VeniceBackendOptions options;
  venice::Client client;
};

VeniceBackend::VeniceBackend(credentials::Secret credential,
                             VeniceBackendOptions options)
    : m_impl(std::make_unique<Impl>(std::move(credential),
                                    std::move(options))) {}

VeniceBackend::~VeniceBackend() = default;
VeniceBackend::VeniceBackend(VeniceBackend&&) noexcept = default;
auto VeniceBackend::operator=(VeniceBackend&&) noexcept
    -> VeniceBackend& = default;

auto VeniceBackend::start(backend::BackendRequest request,
                          const std::stop_token stop_token)
    -> std::expected<std::unique_ptr<backend::BackendStream>,
                     backend::BackendError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(adapter_error(backend::BackendErrorKind::cancelled,
                                           "Venice request cancelled"));
    }
    if (m_impl == nullptr || m_impl->options.pending_events == 0) {
      return std::unexpected(
          request_error("Venice adapter limits are invalid"));
    }
    auto mapped = make_request(request);
    if (!mapped) return std::unexpected(std::move(mapped.error()));
    return std::make_unique<VeniceStream>(m_impl->client, std::move(*mapped),
                                          request.assistant_message_id,
                                          m_impl->options);
  } catch (...) {
    return std::unexpected(adapter_error(backend::BackendErrorKind::unavailable,
                                         "Venice stream could not start"));
  }
}

auto VeniceBackend::lookup(const domain::ModelId& model_id,
                           const std::stop_token stop_token)
    -> std::expected<backend::ModelContextInfo, backend::BackendError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(adapter_error(backend::BackendErrorKind::cancelled,
                                           "Venice request cancelled"));
    }
    if (m_impl == nullptr) {
      return std::unexpected(
          request_error("Venice adapter is not initialized"));
    }

    venice::CancelToken cancellation;
    std::stop_callback cancel_callback{stop_token,
                                       [&cancellation] { cancellation.cancel(); }};
    const auto models = m_impl->client.models(
        "text", {m_impl->options.connect_timeout, m_impl->options.read_timeout,
                 m_impl->options.write_timeout, &cancellation});
    if (!models) return std::unexpected(map_error(models.error()));

    const auto found = std::ranges::find(*models, model_id.value(),
                                         &venice::Model::id);
    if (found == models->end()) {
      return std::unexpected(
          request_error("configured Venice model was not found"));
    }
    if (found->offline.value_or(false)) {
      return std::unexpected(adapter_error(
          backend::BackendErrorKind::unavailable,
          "configured Venice model is unavailable", true));
    }

    auto context_tokens = found->available_context_tokens
                              ? found->available_context_tokens
                              : found->context_length;
    if (context_tokens && found->context_length) {
      context_tokens = std::min(*context_tokens, *found->context_length);
    }
    if (!context_tokens || *context_tokens <= 0) {
      return std::unexpected(
          request_error("configured Venice model has no context capacity"));
    }
    std::optional<std::uint64_t> maximum_output;
    if (found->max_completion_tokens && *found->max_completion_tokens > 0) {
      maximum_output =
          static_cast<std::uint64_t>(*found->max_completion_tokens);
    }
    return backend::ModelContextInfo{
        model_id, static_cast<std::uint64_t>(*context_tokens), maximum_output};
  } catch (...) {
    return std::unexpected(adapter_error(backend::BackendErrorKind::unavailable,
                                         "Venice model lookup failed"));
  }
}

}  // namespace aiforge::adapters
