#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/events.hpp>

namespace aiforge::backend {

using ExtensionMap = std::map<std::string, domain::StructuredDataBlock>;

struct GenerationOptions {
  std::optional<double> temperature;
  std::optional<std::uint64_t> max_output_tokens;
  std::optional<std::uint64_t> seed;
  ExtensionMap extensions;
  auto operator==(const GenerationOptions&) const -> bool = default;
};

struct ToolDeclaration {
  std::string name;
  std::string description;
  domain::StructuredDataBlock input_schema;
  std::vector<domain::Effect> effects;
  std::vector<domain::CapabilityScope> capability_scopes;
  auto operator==(const ToolDeclaration&) const -> bool = default;
};

struct BackendRequest {
  domain::InferenceId inference_id;
  // The runtime owns message identity. Adapters echo this ID on every content
  // delta instead of deriving a provider-specific identity.
  domain::MessageId assistant_message_id;
  domain::ModelId model_id;
  // Produced by the application runtime before an adapter maps provider roles.
  domain::ConstructedContext context;
  std::vector<ToolDeclaration> tools;
  GenerationOptions options;
  auto operator==(const BackendRequest&) const -> bool = default;
};

struct ResponseStarted {
  std::string response_id;
  auto operator==(const ResponseStarted&) const -> bool = default;
};

struct ContentDelta {
  domain::MessageId message_id;
  domain::ContentBlock delta;
  auto operator==(const ContentDelta&) const -> bool = default;
};

struct ReasoningDelta {
  std::optional<std::string> text;
  domain::Metadata metadata;
  auto operator==(const ReasoningDelta&) const -> bool = default;
};

struct ToolCallDelta {
  domain::InvocationId invocation_id;
  std::string tool_name;
  std::string arguments_fragment;
  auto operator==(const ToolCallDelta&) const -> bool = default;
};

struct CitationObserved {
  domain::CitationBlock citation;
  auto operator==(const CitationObserved&) const -> bool = default;
};

struct UsageObserved {
  // Incremental usage since the previous observation in this stream. Adapters
  // that receive cumulative provider counters must emit only the difference.
  domain::Usage usage;
  auto operator==(const UsageObserved&) const -> bool = default;
};

struct ResponseFinished {
  domain::FinishReason reason;
  auto operator==(const ResponseFinished&) const -> bool = default;
};

struct ResponseCancelled {
  std::optional<std::string> reason;
  auto operator==(const ResponseCancelled&) const -> bool = default;
};

using BackendEvent = std::variant<ResponseStarted, ContentDelta, ReasoningDelta,
                                  ToolCallDelta, CitationObserved, UsageObserved,
                                  ResponseFinished, ResponseCancelled>;

enum class BackendErrorKind {
  request_rejected,
  unavailable,
  authentication,
  rate_limited,
  network,
  protocol,
  cancelled,
  script_mismatch,
  script_exhausted,
};

struct BackendError {
  BackendErrorKind kind;
  std::string redacted_message;
  bool retryable{};
  std::optional<int> status_code;
  auto operator==(const BackendError&) const -> bool = default;
};

struct ModelContextInfo {
  domain::ModelId model_id;
  std::uint64_t context_window_tokens{};
  std::optional<std::uint64_t> maximum_output_tokens;
  auto operator==(const ModelContextInfo&) const -> bool = default;
};

class BackendStream {
 public:
  virtual ~BackendStream() = default;

  [[nodiscard]] virtual auto next(std::stop_token stop_token)
      -> std::expected<std::optional<BackendEvent>, BackendError> = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;

  [[nodiscard]] virtual auto start(BackendRequest request, std::stop_token stop_token)
      -> std::expected<std::unique_ptr<BackendStream>, BackendError> = 0;
};

class ModelContextProvider {
 public:
  virtual ~ModelContextProvider() = default;

  [[nodiscard]] virtual auto lookup(const domain::ModelId& model_id,
                                    std::stop_token stop_token)
      -> std::expected<ModelContextInfo, BackendError> = 0;
};

}  // namespace aiforge::backend
