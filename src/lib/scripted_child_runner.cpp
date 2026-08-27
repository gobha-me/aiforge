#include <aiforge/testing/scripted_child_runner.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace aiforge::testing {
namespace {

class ScriptedChildRunStream final : public runtime::ChildRunResultStream {
 public:
  explicit ScriptedChildRunStream(ChildRunStreamScript script)
      : m_steps(std::move(script.steps)) {}

  auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<runtime::ChildRunResult>,
                       runtime::ChildRunError> override {
    if (m_ended)
      return std::optional<runtime::ChildRunResult>{};
    if (m_next_step >= m_steps.size()) {
      m_ended = true;
      return std::optional<runtime::ChildRunResult>{};
    }
    const auto& step = m_steps[m_next_step++];
    if (stop_token.stop_requested() &&
        !std::holds_alternative<ChildRunResultAfterStop>(step)) {
      m_ended = true;
      return std::unexpected(
          runtime::ChildRunError{runtime::ChildRunErrorCode::cancelled,
                                 "child-run stop was requested", false});
    }
    if (const auto* result = std::get_if<runtime::ChildRunResult>(&step)) {
      return std::optional<runtime::ChildRunResult>{*result};
    }
    if (const auto* error = std::get_if<runtime::ChildRunError>(&step)) {
      m_ended = true;
      return std::unexpected(*error);
    }
    if (std::holds_alternative<ChildRunWaitForStop>(step)) {
      std::mutex mutex;
      std::unique_lock lock(mutex);
      std::condition_variable_any changed;
      changed.wait(lock, stop_token, [] { return false; });
      m_ended = true;
      return std::unexpected(runtime::ChildRunError{
          runtime::ChildRunErrorCode::cancelled,
          "scripted child run observed cancellation", false});
    }
    if (const auto* late = std::get_if<ChildRunResultAfterStop>(&step)) {
      std::mutex mutex;
      std::unique_lock lock(mutex);
      std::condition_variable_any changed;
      changed.wait(lock, stop_token, [] { return false; });
      m_ended = true;
      return std::optional<runtime::ChildRunResult>{late->result};
    }
    m_ended = true;
    return std::optional<runtime::ChildRunResult>{};
  }

 private:
  std::vector<ChildRunScriptStep> m_steps;
  std::size_t m_next_step{};
  bool m_ended{};
};

[[nodiscard]] auto cancelled_error() -> runtime::ChildRunError {
  return {runtime::ChildRunErrorCode::cancelled,
          "child runner start was cancelled", false};
}

}  // namespace

ScriptedChildRunner::ScriptedChildRunner(
    std::vector<ScriptedChildRunExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {}

auto ScriptedChildRunner::start(runtime::ChildRunInvocation invocation,
                                const std::stop_token stop_token)
    -> std::expected<std::unique_ptr<runtime::ChildRunResultStream>,
                     runtime::ChildRunError> {
  if (stop_token.stop_requested())
    return std::unexpected(cancelled_error());
  m_recorded_invocations.push_back(invocation);
  if (m_next_exchange >= m_exchanges.size()) {
    return std::unexpected(runtime::ChildRunError{
        runtime::ChildRunErrorCode::unavailable,
        "scripted child runner has no exchange remaining", false});
  }
  const auto& exchange = m_exchanges[m_next_exchange];
  if (invocation != exchange.expected_invocation) {
    return std::unexpected(runtime::ChildRunError{
        runtime::ChildRunErrorCode::protocol_failure,
        "child-run invocation did not match the script", false});
  }
  ++m_next_exchange;
  if (const auto* error =
          std::get_if<runtime::ChildRunError>(&exchange.outcome)) {
    return std::unexpected(*error);
  }
  auto stream = std::make_unique<ScriptedChildRunStream>(
      std::get<ChildRunStreamScript>(exchange.outcome));
  return std::unique_ptr<runtime::ChildRunResultStream>{std::move(stream)};
}

auto ScriptedChildRunner::recorded_invocations() const noexcept
    -> const std::vector<runtime::ChildRunInvocation>& {
  return m_recorded_invocations;
}

auto ScriptedChildRunner::remaining_exchanges() const noexcept -> std::size_t {
  return m_exchanges.size() - m_next_exchange;
}

}  // namespace aiforge::testing
