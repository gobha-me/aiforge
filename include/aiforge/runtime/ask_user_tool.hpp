#pragma once

#include <cstddef>
#include <expected>

#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

struct AskUserLimits {
  std::size_t questions{3};
  std::size_t options_per_question{8};
  std::size_t prompt_bytes{1024};
  std::size_t description_bytes{1024};
  std::size_t label_bytes{256};
  std::size_t other_answer_bytes{4096};
  auto operator==(const AskUserLimits&) const -> bool = default;
};

[[nodiscard]] auto ask_user_declaration(const AskUserLimits& limits = {})
    -> backend::ToolDeclaration;

[[nodiscard]] auto register_ask_user_tool(ToolRegistry& registry,
                                          bool interactive_input_available,
                                          AskUserLimits limits = {})
    -> std::expected<void, ToolRegistryError>;

} // namespace aiforge::runtime
