#include <aiforge/testing/scripted_tool_executor.hpp>

#include <optional>
#include <utility>

namespace aiforge::testing {
namespace {

class ScriptedToolStream final : public runtime::ToolExecutionStream {
 public:
  explicit ScriptedToolStream(ToolStreamScript script)
      : m_steps(std::move(script.steps)) {}

  auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    if (m_ended) return std::optional<runtime::ToolExecutionEvent>{};
    if (stop_token.stop_requested()) {
      m_ended = true;
      return std::unexpected(runtime::ToolExecutionError{
          runtime::ToolExecutionErrorCode::cancelled,
          "tool execution cancelled", false});
    }
    if (m_next_step >= m_steps.size()) {
      m_ended = true;
      return std::optional<runtime::ToolExecutionEvent>{};
    }

    const auto& step = m_steps[m_next_step++];
    if (const auto* event = std::get_if<runtime::ToolExecutionEvent>(&step)) {
      return std::optional<runtime::ToolExecutionEvent>{*event};
    }
    if (const auto* error = std::get_if<runtime::ToolExecutionError>(&step)) {
      m_ended = true;
      return std::unexpected(*error);
    }
    m_ended = true;
    return std::optional<runtime::ToolExecutionEvent>{};
  }

 private:
  std::vector<ScriptedToolStep> m_steps;
  std::size_t m_next_step{};
  bool m_ended{};
};

[[nodiscard]] auto cancelled_error() -> runtime::ToolExecutionError {
  return {runtime::ToolExecutionErrorCode::cancelled, "tool start cancelled",
          false};
}

} // namespace

ScriptedToolExecutor::ScriptedToolExecutor(
    std::vector<ScriptedToolExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {
}

auto ScriptedToolExecutor::validate(
    const domain::StructuredDataBlock& arguments) const
    -> std::expected<runtime::ValidatedToolArguments,
                     runtime::ToolExecutionError> {
  if (arguments.media_type != "application/json" || arguments.data.empty()) {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::invalid_arguments,
        "tool arguments must be nonempty application/json", false});
  }
  return runtime::ValidatedToolArguments{arguments};
}

auto ScriptedToolExecutor::start(runtime::ToolInvocation invocation,
                                 const std::stop_token stop_token)
    -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                     runtime::ToolExecutionError> {
  if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
  m_recorded_invocations.push_back(invocation);
  if (m_next_exchange >= m_exchanges.size()) {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::unavailable,
        "scripted tool has no exchange remaining", false});
  }
  const auto& exchange = m_exchanges[m_next_exchange];
  if (invocation != exchange.expected_invocation) {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::protocol_failure,
        "tool invocation did not match the script", false});
  }
  ++m_next_exchange;
  if (const auto* error =
          std::get_if<runtime::ToolExecutionError>(&exchange.outcome)) {
    return std::unexpected(*error);
  }
  return std::make_unique<ScriptedToolStream>(
      std::get<ToolStreamScript>(exchange.outcome));
}

auto ScriptedToolExecutor::recorded_invocations() const noexcept
    -> const std::vector<runtime::ToolInvocation>& {
  return m_recorded_invocations;
}

auto ScriptedToolExecutor::remaining_exchanges() const noexcept -> std::size_t {
  return m_exchanges.size() - m_next_exchange;
}

} // namespace aiforge::testing
