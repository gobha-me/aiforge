#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>

#include <aiforge/detail/utf8_text.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace aiforge::runtime {
namespace {

constexpr std::size_t kMaximumToolNameBytes{128};
constexpr std::size_t kMaximumDescriptionBytes{std::size_t{16} * 1024U};
constexpr std::size_t kMaximumSchemaBytes{std::size_t{1024} * 1024U};
constexpr std::size_t kMaximumScopeBytes{std::size_t{16} * 1024U};
constexpr std::size_t kMaximumSubsetTools{256};
constexpr std::size_t kMaximumExecutorContractBytes{128};

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

[[nodiscard]] auto valid_effect(const domain::Effect effect) noexcept -> bool {
  switch (effect) {
    case domain::Effect::read:
    case domain::Effect::write:
    case domain::Effect::remove:
    case domain::Effect::execute:
    case domain::Effect::network:
    case domain::Effect::communicate:
    case domain::Effect::spend:
    case domain::Effect::change_infrastructure:
    case domain::Effect::change_privileges: return true;
  }
  return false;
}

[[nodiscard]] auto valid_executor_contract(const ToolExecutorContract& contract)
    -> bool {
  const auto valid_field = [](const std::string_view value) {
    return !value.empty() && value.size() <= kMaximumExecutorContractBytes &&
           detail::is_safe_utf8_text(value) &&
           value.find_first_of("\r\n\t") == std::string_view::npos;
  };
  return valid_field(contract.identity) && valid_field(contract.version);
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
    if (!valid_effect(effect) || !effects.insert(effect).second) {
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
    if (!scopes
             .emplace(normalized->effect,
                      std::pair{normalized->kind, normalized->value})
             .second) {
      return invalid("tool capability scopes are invalid or duplicated");
    }
  }
  if (std::ranges::any_of(effects, [&](const auto effect) {
        return std::ranges::none_of(scopes, [effect](const auto& scope) {
          return scope.first == effect;
        });
      })) {
    return invalid("every tool effect requires an explicit capability scope");
  }
  return {};
}

} // namespace

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

auto ToolRegistrySnapshot::subset(const std::span<const std::string> names)
    const -> std::expected<ToolRegistrySnapshot, ToolRegistryError> {
  try {
    if (names.size() > kMaximumSubsetTools) {
      return invalid("tool subset contains too many names");
    }
    std::set<std::string_view> selected;
    for (const auto& name : names) {
      if (!selected.insert(name).second) {
        return std::unexpected(
            ToolRegistryError{ToolRegistryErrorCode::duplicate_name,
                              "tool subset contains a duplicate name"});
      }
    }

    std::vector<RegisteredTool> tools;
    std::vector<backend::ToolDeclaration> declarations;
    tools.reserve(selected.size());
    declarations.reserve(selected.size());
    for (const auto& tool : m_tools) {
      if (!selected.contains(tool.declaration.name)) continue;
      tools.push_back(tool);
      declarations.push_back(tool.declaration);
    }
    if (tools.size() != selected.size()) {
      return invalid("tool subset contains an unknown name");
    }
    return ToolRegistrySnapshot{std::move(tools), std::move(declarations)};
  } catch (...) {
    return std::unexpected(
        ToolRegistryError{ToolRegistryErrorCode::internal_failure,
                          "tool subset selection failed internally"});
  }
}

auto ToolRegistry::register_tool(
    backend::ToolDeclaration declaration,
    std::shared_ptr<ToolExecutor> executor, ToolExecutionLimits limits,
    std::optional<ToolExecutorContract> executor_contract)
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
    if (executor_contract && !valid_executor_contract(*executor_contract)) {
      return invalid("tool executor contract identity or version is invalid");
    }
    if (std::ranges::any_of(m_tools, [&](const auto& tool) {
          return tool.declaration.name == declaration.name;
        })) {
      return std::unexpected(
          ToolRegistryError{ToolRegistryErrorCode::duplicate_name,
                            "tool name is already registered"});
    }
    m_tools.push_back(RegisteredTool{std::move(declaration), limits,
                                     std::move(executor),
                                     std::move(executor_contract)});
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
    for (const auto& tool : m_tools)
      declarations.push_back(tool.declaration);
    return ToolRegistrySnapshot{m_tools, std::move(declarations)};
  } catch (...) {
    return std::unexpected(
        ToolRegistryError{ToolRegistryErrorCode::internal_failure,
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

auto tool_continuation_messages(std::span<const domain::RunEvent> events)
    -> std::expected<std::vector<domain::Message>, ToolExecutionError> {
  try {
    std::vector<domain::Message> result;
    std::optional<domain::Message> assistant;
    std::optional<domain::InferenceId> assistant_inference;
    bool assistant_finished{};
    std::vector<domain::Message> pending_terminal;
    std::set<domain::InvocationId> proposed_invocations;
    std::set<domain::InvocationId> terminal_invocations;

    const auto terminal_message =
        [&](const domain::InvocationId& invocation_id,
            const std::optional<domain::MessageId>& message_id,
            std::vector<domain::ContentBlock> content)
        -> std::expected<domain::Message, ToolExecutionError> {
      if (!message_id || !proposed_invocations.contains(invocation_id) ||
          !terminal_invocations.insert(invocation_id).second) {
        return std::unexpected(ToolExecutionError{
            ToolExecutionErrorCode::protocol_failure,
            "tool continuation history is incomplete or ambiguous", false});
      }
      if (content.empty()) {
        content.emplace_back(
            domain::TextBlock{"tool completed without output"});
      }
      return domain::Message{*message_id, domain::Role::tool,
                             std::move(content), invocation_id};
    };

    const auto flush_complete_turn =
        [&]() -> std::expected<void, ToolExecutionError> {
      if (!assistant || !assistant_finished) return {};
      if (pending_terminal.size() > assistant->tool_calls.size()) {
        return std::unexpected(ToolExecutionError{
            ToolExecutionErrorCode::protocol_failure,
            "assistant tool-call history has excess terminal results", false});
      }
      if (pending_terminal.size() < assistant->tool_calls.size()) return {};

      result.push_back(std::move(*assistant));
      result.insert(result.end(),
                    std::make_move_iterator(pending_terminal.begin()),
                    std::make_move_iterator(pending_terminal.end()));
      assistant.reset();
      assistant_inference.reset();
      assistant_finished = false;
      pending_terminal.clear();
      return {};
    };

    for (const auto& event : events) {
      if (const auto* started =
              std::get_if<domain::AssistantContentStarted>(&event.payload)) {
        if (assistant) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "assistant tool-call history overlaps inference turns", false});
        }
        assistant = domain::Message{
            started->message_id, domain::Role::assistant, {}, std::nullopt, {}};
        assistant_inference = started->inference_id;
        assistant_finished = false;
      } else if (const auto* delta =
                     std::get_if<domain::AssistantContentDeltaAdded>(
                         &event.payload)) {
        if (!assistant || !assistant_inference || assistant_finished ||
            delta->message_id != assistant->message_id ||
            delta->inference_id != *assistant_inference) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "assistant tool-call content has no active inference", false});
        }
        assistant->content.push_back(delta->delta);
      } else if (const auto* proposed =
                     std::get_if<domain::ToolProposed>(&event.payload)) {
        if (!assistant || assistant_finished ||
            !proposed_invocations.insert(proposed->invocation_id).second) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "tool proposal has no unique assistant turn", false});
        }
        assistant->tool_calls.push_back(domain::ToolCall{
            proposed->invocation_id, proposed->tool_name, proposed->arguments});
      } else if (const auto* recorded =
                     std::get_if<domain::ToolResultRecorded>(&event.payload)) {
        auto message =
            terminal_message(recorded->invocation_id,
                             recorded->result_message_id, recorded->content);
        if (!message) return std::unexpected(std::move(message.error()));
        if (!assistant) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "tool result has no assistant tool-call turn", false});
        }
        pending_terminal.push_back(std::move(*message));
        if (auto flushed = flush_complete_turn(); !flushed) {
          return std::unexpected(std::move(flushed.error()));
        }
      } else if (const auto* failed =
                     std::get_if<domain::ToolErrored>(&event.payload)) {
        auto message =
            terminal_message(failed->invocation_id, failed->result_message_id,
                             {domain::TextBlock{failed->error.message.empty()
                                                    ? "tool invocation failed"
                                                    : failed->error.message}});
        if (!message) return std::unexpected(std::move(message.error()));
        if (!assistant) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "tool error has no assistant tool-call turn", false});
        }
        pending_terminal.push_back(std::move(*message));
        if (auto flushed = flush_complete_turn(); !flushed) {
          return std::unexpected(std::move(flushed.error()));
        }
      } else if (const auto* finished =
                     std::get_if<domain::AssistantContentFinished>(
                         &event.payload)) {
        if (!assistant || !assistant_inference || assistant_finished ||
            finished->message_id != assistant->message_id ||
            finished->inference_id != *assistant_inference) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "assistant tool-call finish has no active inference", false});
        }
        assistant_finished = true;
        if (assistant->tool_calls.empty() && !pending_terminal.empty()) {
          return std::unexpected(ToolExecutionError{
              ToolExecutionErrorCode::protocol_failure,
              "tool results have no assistant tool calls", false});
        }
        if (assistant->tool_calls.empty()) {
          assistant.reset();
          assistant_inference.reset();
          assistant_finished = false;
        } else if (auto flushed = flush_complete_turn(); !flushed) {
          return std::unexpected(std::move(flushed.error()));
        }
      }
    }
    if (assistant && !assistant_finished) {
      return std::unexpected(ToolExecutionError{
          ToolExecutionErrorCode::protocol_failure,
          "tool continuation history ends inside an inference", false});
    }
    return result;
  } catch (...) {
    return std::unexpected(ToolExecutionError{
        ToolExecutionErrorCode::internal_failure,
        "tool continuation projection failed internally", false});
  }
}

} // namespace aiforge::runtime
