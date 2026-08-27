#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

#include <aiforge/runtime/child_runner.hpp>

namespace aiforge::testing {

struct ChildRunEndOfStream {
  auto operator==(const ChildRunEndOfStream&) const -> bool = default;
};

struct ChildRunWaitForStop {
  auto operator==(const ChildRunWaitForStop&) const -> bool = default;
};

struct ChildRunResultAfterStop {
  runtime::ChildRunResult result;
  auto operator==(const ChildRunResultAfterStop&) const -> bool = default;
};

using ChildRunScriptStep =
    std::variant<runtime::ChildRunResult, runtime::ChildRunError,
                 ChildRunEndOfStream, ChildRunWaitForStop,
                 ChildRunResultAfterStop>;

struct ChildRunStreamScript {
  std::vector<ChildRunScriptStep> steps;
  auto operator==(const ChildRunStreamScript&) const -> bool = default;
};

using ScriptedChildRunOutcome =
    std::variant<ChildRunStreamScript, runtime::ChildRunError>;

struct ScriptedChildRunExchange {
  runtime::ChildRunInvocation expected_invocation;
  ScriptedChildRunOutcome outcome;
  auto operator==(const ScriptedChildRunExchange&) const -> bool = default;
};

class ScriptedChildRunner final : public runtime::ChildRunner {
 public:
  explicit ScriptedChildRunner(std::vector<ScriptedChildRunExchange> exchanges,
                               std::size_t maximum_concurrency = 1);

  [[nodiscard]] auto maximum_concurrency() const noexcept
      -> std::size_t override;

  [[nodiscard]] auto start(runtime::ChildRunInvocation invocation,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<runtime::ChildRunResultStream>,
                       runtime::ChildRunError> override;

  [[nodiscard]] auto recorded_invocations() const noexcept
      -> const std::vector<runtime::ChildRunInvocation>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ScriptedChildRunExchange> m_exchanges;
  std::vector<runtime::ChildRunInvocation> m_recorded_invocations;
  std::size_t m_next_exchange{};
  std::size_t m_maximum_concurrency{1};
  mutable std::mutex m_mutex;
};

}  // namespace aiforge::testing
