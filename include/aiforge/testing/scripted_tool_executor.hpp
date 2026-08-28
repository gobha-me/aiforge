#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <variant>
#include <vector>

#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::testing {

struct ToolEndOfStream {
  auto operator==(const ToolEndOfStream&) const -> bool = default;
};

using ScriptedToolStep =
    std::variant<runtime::ToolExecutionEvent, runtime::ToolExecutionError,
                 ToolEndOfStream>;

struct ToolStreamScript {
  std::vector<ScriptedToolStep> steps;
  auto operator==(const ToolStreamScript&) const -> bool = default;
};

using ScriptedToolOutcome =
    std::variant<ToolStreamScript, runtime::ToolExecutionError>;

struct ScriptedToolExchange {
  runtime::ToolInvocation expected_invocation;
  ScriptedToolOutcome outcome;
  auto operator==(const ScriptedToolExchange&) const -> bool = default;
};

class ScriptedToolExecutor final : public runtime::ToolExecutor {
 public:
  explicit ScriptedToolExecutor(std::vector<ScriptedToolExchange> exchanges);

  [[nodiscard]] auto validate(const domain::StructuredDataBlock& arguments)
      const -> std::expected<runtime::ValidatedToolArguments,
                             runtime::ToolExecutionError> override;

  [[nodiscard]] auto start(runtime::ToolInvocation invocation,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override;

  [[nodiscard]] auto recorded_invocations() const noexcept
      -> const std::vector<runtime::ToolInvocation>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ScriptedToolExchange> m_exchanges;
  std::vector<runtime::ToolInvocation> m_recorded_invocations;
  std::size_t m_next_exchange{};
};

} // namespace aiforge::testing
