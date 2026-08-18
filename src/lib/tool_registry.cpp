#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/runtime/tool_policy.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace aiforge::runtime {
namespace {

constexpr std::size_t kMaximumToolNameBytes{128};
constexpr std::size_t kMaximumDescriptionBytes{16U * 1024U};
constexpr std::size_t kMaximumSchemaBytes{1024U * 1024U};
constexpr std::size_t kMaximumScopeBytes{16U * 1024U};

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto invalid(std::string message)
    -> std::unexpected<ToolRegistryError> {
  return std::unexpected(ToolRegistryError{
      ToolRegistryErrorCode::invalid_declaration, std::move(message)});
}

[[nodiscard]] auto valid_limits(const ToolExecutionLimits& limits) -> bool {
  return limits.output_bytes != 0 && limits.progress_events != 0 &&
         limits.timeout > std::chrono::milliseconds::zero();
}

[[nodiscard]] auto valid_declaration(
    const backend::ToolDeclaration& declaration,
    const ToolExecutionLimits& limits)
    -> std::expected<void, ToolRegistryError> {
  if (declaration.name.empty() ||
      declaration.name.size() > kMaximumToolNameBytes ||
      has_control_character(declaration.name)) {
    return invalid(
        "tool name is empty, oversized, or contains control characters");
  }
  if (declaration.description.empty() ||
      declaration.description.size() > kMaximumDescriptionBytes ||
      has_control_character(declaration.description)) {
    return invalid(
        "tool description is empty, oversized, or contains control characters");
  }
  if (declaration.input_schema.media_type != "application/schema+json" ||
      declaration.input_schema.data.empty() ||
      declaration.input_schema.data.size() > kMaximumSchemaBytes) {
    return invalid("tool input schema must be bounded application/schema+json");
  }
  const auto schema = nlohmann::json::parse(declaration.input_schema.data,
                                            nullptr, false, false);
  if (schema.is_discarded() || !schema.is_object()) {
    return invalid("tool input schema must contain a valid JSON object");
  }
  if (!valid_limits(limits)) {
    return invalid("tool execution limits must be positive");
  }

  std::set<domain::Effect> effects;
  for (const auto effect : declaration.effects) {
    if (!effects.insert(effect).second) {
      return invalid("tool effects must be unique");
    }
  }
  std::set<std::pair<domain::Effect, std::pair<std::string, std::string>>>
      scopes;
  for (const auto& scope : declaration.capability_scopes) {
    const auto normalized = normalize_capability_scope(scope);
    if (!effects.contains(scope.effect) || scope.kind.empty() ||
        scope.value.empty() || scope.kind.size() > kMaximumScopeBytes ||
        scope.value.size() > kMaximumScopeBytes ||
        has_control_character(scope.kind) ||
        has_control_character(scope.value) || !normalized) {
      return invalid("tool capability scopes are invalid or duplicated");
    }
    if (!scopes.emplace(normalized->effect,
                        std::pair{normalized->kind, normalized->value})
             .second) {
      return invalid("tool capability scopes are invalid or duplicated");
    }
  }
  return {};
}

}  // namespace

auto ToolRegistrySnapshot::declarations() const noexcept
    -> const std::vector<backend::ToolDeclaration>& {
  return m_declarations;
}

auto ToolRegistrySnapshot::find(const std::string_view name) const noexcept
    -> const RegisteredTool* {
  const auto found = std::ranges::find(m_tools, name, [](const auto& tool) {
    return std::string_view{tool.declaration.name};
  });
  return found == m_tools.end() ? nullptr : &*found;
}

auto ToolRegistry::register_tool(backend::ToolDeclaration declaration,
                                 std::shared_ptr<ToolExecutor> executor,
                                 ToolExecutionLimits limits)
    -> std::expected<void, ToolRegistryError> {
  try {
    if (!executor) {
      return std::unexpected(
          ToolRegistryError{ToolRegistryErrorCode::missing_executor,
                            "tool registration requires an executor"});
    }
    if (auto checked = valid_declaration(declaration, limits); !checked) {
      return checked;
    }
    if (std::ranges::any_of(m_tools, [&](const auto& tool) {
          return tool.declaration.name == declaration.name;
        })) {
      return std::unexpected(
          ToolRegistryError{ToolRegistryErrorCode::duplicate_name,
                            "tool name is already registered"});
    }
    m_tools.push_back(
        RegisteredTool{std::move(declaration), limits, std::move(executor)});
    return {};
  } catch (...) {
    return std::unexpected(
        ToolRegistryError{ToolRegistryErrorCode::internal_failure,
                          "tool registration failed internally"});
  }
}

auto ToolRegistry::snapshot() const
    -> std::expected<ToolRegistrySnapshot, ToolRegistryError> {
  try {
    std::vector<backend::ToolDeclaration> declarations;
    declarations.reserve(m_tools.size());
    for (const auto& tool : m_tools) declarations.push_back(tool.declaration);
    return ToolRegistrySnapshot{m_tools, std::move(declarations)};
  } catch (...) {
    return std::unexpected(ToolRegistryError{
        ToolRegistryErrorCode::internal_failure,
        "tool registry snapshot failed internally"});
  }
}

auto tool_result_messages(std::span<const domain::RunEvent> events)
    -> std::expected<std::vector<domain::Message>, ToolExecutionError> {
  try {
    std::vector<domain::Message> result;
    std::set<domain::InvocationId> terminal_invocations;
    for (const auto& event : events) {
      if (const auto* recorded =
              std::get_if<domain::ToolResultRecorded>(&event.payload)) {
        if (!recorded->result_message_id ||
            !terminal_invocations.insert(recorded->invocation_id).second) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "tool result history lacks a unique message identity", false});
        }
        auto content = recorded->content;
        if (content.empty()) {
          content.emplace_back(
              domain::TextBlock{"tool completed without output"});
        }
        result.push_back(domain::Message{*recorded->result_message_id,
                                         domain::Role::tool, std::move(content),
                                         recorded->invocation_id});
      } else if (const auto* failed =
                     std::get_if<domain::ToolErrored>(&event.payload)) {
        if (!failed->result_message_id ||
            !terminal_invocations.insert(failed->invocation_id).second) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "tool error history lacks a unique message identity", false});
        }
        result.push_back(
            domain::Message{*failed->result_message_id,
                            domain::Role::tool,
                            {domain::TextBlock{failed->error.message.empty()
                                                   ? "tool invocation failed"
                                                   : failed->error.message}},
                            failed->invocation_id});
      }
    }
    return result;
  } catch (...) {
    return std::unexpected(
        ToolExecutionError{ToolExecutionErrorCode::internal_failure,
                           "tool result projection failed internally", false});
  }
}

}  // namespace aiforge::runtime
