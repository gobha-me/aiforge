#include <aiforge/surfaces/slash_commands.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>

#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::surfaces {
namespace {

constexpr std::size_t maximum_registered_name_bytes = 64U;
constexpr std::size_t maximum_tool_profile_id_bytes = 64U;
constexpr std::size_t maximum_tool_name_bytes = 128U;

[[nodiscard]] auto registry_error(const SlashCommandRegistryErrorCode code,
                                  std::string message,
                                  std::string command_id = {})
    -> std::unexpected<SlashCommandRegistryError> {
  return std::unexpected(SlashCommandRegistryError{code, std::move(message),
                                                   std::move(command_id)});
}

[[nodiscard]] auto command_error(const SlashCommandErrorCode code,
                                 std::string message)
    -> std::unexpected<SlashCommandError> {
  return std::unexpected(SlashCommandError{code, std::move(message)});
}

[[nodiscard]] auto valid_identifier(const std::string_view value,
                                    const bool allow_dot) -> bool {
  if (value.empty()) return false;
  const auto first = static_cast<unsigned char>(value.front());
  if (first < 'a' || first > 'z') return false;
  return std::ranges::all_of(value.substr(1), [allow_dot](const char raw) {
    const auto ch = static_cast<unsigned char>(raw);
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
           ch == '_' || (allow_dot && ch == '.');
  });
}

[[nodiscard]] auto valid_bounded_identifier(const std::string_view value,
                                            const std::size_t maximum_bytes)
    -> bool {
  return value.size() <= maximum_bytes && valid_identifier(value, false);
}

[[nodiscard]] auto valid_tool_profile_id(const std::string_view value) -> bool {
  return valid_bounded_identifier(value, maximum_tool_profile_id_bytes);
}

[[nodiscard]] auto valid_tool_category(const std::string_view value) -> bool {
  return runtime::tool_category_from_name(value).has_value();
}

[[nodiscard]] auto valid_tool_name(const std::string_view value) -> bool {
  return value.size() <= maximum_tool_name_bytes &&
         detail::is_safe_utf8_text(value) &&
         value.find_first_of(" \t\r\n*?[]{}") == std::string_view::npos;
}

[[nodiscard]] auto valid_utf8_text(const std::string_view value,
                                   const bool allow_space, const bool allow_tab)
    -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || first == 0x7FU ||
        (first < 0x20U && !(allow_tab && first == '\t')) ||
        (!allow_space && first == ' ')) {
      return false;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto metadata_valid(const std::string_view value) -> bool {
  return !value.empty() && valid_utf8_text(value, true, true);
}

[[nodiscard]] auto idle_available(const SlashCommandContext& context) -> bool {
  return !context.run_active;
}

[[nodiscard]] auto editor_available(const SlashCommandContext& context)
    -> bool {
  return !context.run_active && context.editor_available;
}

[[nodiscard]] auto no_arguments(std::string_view arguments,
                                const SlashCommandAction action)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  if (!arguments.empty()) {
    return command_error(SlashCommandErrorCode::invalid_arguments,
                         "slash command does not accept arguments");
  }
  return SlashCommandResult{action, std::nullopt};
}

[[nodiscard]] auto help_handler(std::string_view arguments,
                                const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  if (arguments.starts_with('/')) arguments.remove_prefix(1);
  if (!arguments.empty() && !valid_identifier(arguments, false)) {
    return command_error(SlashCommandErrorCode::invalid_arguments,
                         "help accepts at most one command name");
  }
  return SlashCommandResult{
      SlashCommandAction::show_help,
      arguments.empty() ? std::nullopt : std::optional<std::string>{arguments}};
}

[[nodiscard]] auto quit_handler(std::string_view arguments,
                                const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return no_arguments(arguments, SlashCommandAction::quit);
}

[[nodiscard]] auto clear_handler(std::string_view arguments,
                                 const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return no_arguments(arguments, SlashCommandAction::clear_view);
}

[[nodiscard]] auto edit_handler(std::string_view arguments,
                                const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return no_arguments(arguments, SlashCommandAction::edit_draft);
}

[[nodiscard]] auto trim_arguments(std::string_view value) -> std::string_view {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

[[nodiscard]] auto take_argument(std::string_view& arguments)
    -> std::string_view {
  arguments = trim_arguments(arguments);
  const auto separator = arguments.find_first_of(" \t");
  if (separator == std::string_view::npos) {
    return std::exchange(arguments, {});
  }
  const auto result = arguments.substr(0, separator);
  arguments = trim_arguments(arguments.substr(separator));
  return result;
}

[[nodiscard]] auto profile_selection_result(std::string_view arguments)
    -> std::optional<SlashCommandResult> {
  const auto profile_id = take_argument(arguments);
  if (!arguments.empty() || !valid_tool_profile_id(profile_id)) {
    return std::nullopt;
  }
  return SlashCommandResult{SlashCommandAction::select_tool_profile,
                            std::string{profile_id}};
}

using ToolTargetValidator = auto (*)(std::string_view) -> bool;

[[nodiscard]] auto toggle_result(std::string_view arguments,
                                 const SlashCommandAction enable_action,
                                 const SlashCommandAction disable_action,
                                 const ToolTargetValidator valid_target)
    -> std::optional<SlashCommandResult> {
  const auto target = take_argument(arguments);
  const auto state = take_argument(arguments);
  if (!arguments.empty() || !valid_target(target)) return std::nullopt;
  if (state == "on") {
    return SlashCommandResult{enable_action, std::string{target}};
  }
  if (state == "off") {
    return SlashCommandResult{disable_action, std::string{target}};
  }
  return std::nullopt;
}

[[nodiscard]] auto maximum_profile_result(
    std::string_view arguments, const SlashCommandAction set_action,
    const SlashCommandAction inherit_action)
    -> std::optional<SlashCommandResult> {
  const auto profile_id = take_argument(arguments);
  if (!arguments.empty()) return std::nullopt;
  if (profile_id == "inherit") {
    return SlashCommandResult{inherit_action, std::nullopt};
  }
  if (!valid_tool_profile_id(profile_id)) return std::nullopt;
  return SlashCommandResult{set_action, std::string{profile_id}};
}

[[nodiscard]] auto session_handler(std::string_view arguments,
                                   const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  if (arguments.empty() || arguments == "list") {
    return SlashCommandResult{SlashCommandAction::list_sessions, std::nullopt};
  }
  if (arguments == "new") {
    return SlashCommandResult{SlashCommandAction::new_session, std::nullopt};
  }

  constexpr std::string_view resume{"resume"};
  if (arguments.starts_with(resume) && arguments.size() > resume.size() &&
      (arguments[resume.size()] == ' ' || arguments[resume.size()] == '\t')) {
    const auto session_id = trim_arguments(arguments.substr(resume.size()));
    if (!session_id.empty()) {
      return SlashCommandResult{SlashCommandAction::resume_session,
                                std::string{session_id}};
    }
  }
  return command_error(SlashCommandErrorCode::invalid_arguments,
                       "session accepts list, resume <session-id>, or new");
}

[[nodiscard]] auto persona_handler(std::string_view arguments,
                                   const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  if (arguments.empty() || arguments == "list") {
    return SlashCommandResult{SlashCommandAction::list_personas, std::nullopt};
  }
  if (arguments == "off") {
    return SlashCommandResult{SlashCommandAction::disable_persona,
                              std::nullopt};
  }
  if (arguments == "manage") {
    return SlashCommandResult{SlashCommandAction::manage_personas,
                              std::nullopt};
  }
  constexpr std::string_view set{"set"};
  if (arguments.starts_with(set) && arguments.size() > set.size() &&
      (arguments[set.size()] == ' ' || arguments[set.size()] == '\t')) {
    const auto name = trim_arguments(arguments.substr(set.size()));
    if (!name.empty() && name.find_first_of(" \t") == std::string_view::npos) {
      return SlashCommandResult{SlashCommandAction::select_persona,
                                std::string{name}};
    }
  }
  return command_error(SlashCommandErrorCode::invalid_arguments,
                       "persona accepts list, set <name>, off, or manage");
}

[[nodiscard]] auto model_handler(std::string_view arguments,
                                 const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  if (arguments.empty())
    return SlashCommandResult{SlashCommandAction::choose_model, std::nullopt};
  if (arguments.find_first_of(" \t") == std::string_view::npos) {
    return SlashCommandResult{SlashCommandAction::choose_model,
                              std::string{arguments}};
  }
  return command_error(SlashCommandErrorCode::invalid_arguments,
                       "model accepts at most one model ID");
}

[[nodiscard]] auto character_handler(std::string_view arguments,
                                     const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  if (arguments.empty()) {
    return SlashCommandResult{SlashCommandAction::choose_provider_character,
                              std::nullopt};
  }
  if (arguments == "off") {
    return SlashCommandResult{SlashCommandAction::disable_provider_character,
                              std::nullopt};
  }
  constexpr std::string_view set{"set"};
  if (arguments.starts_with(set) && arguments.size() > set.size() &&
      (arguments[set.size()] == ' ' || arguments[set.size()] == '\t')) {
    const auto slug = trim_arguments(arguments.substr(set.size()));
    if (!slug.empty() && slug.find_first_of(" \t") == std::string_view::npos) {
      return SlashCommandResult{SlashCommandAction::choose_provider_character,
                                std::string{slug}};
    }
  }
  return command_error(SlashCommandErrorCode::invalid_arguments,
                       "character accepts set <slug> or off");
}

[[nodiscard]] auto usage_handler(std::string_view arguments,
                                 const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return no_arguments(arguments, SlashCommandAction::show_usage);
}

[[nodiscard]] auto settings_handler(std::string_view arguments,
                                    const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return no_arguments(arguments, SlashCommandAction::manage_request_settings);
}

[[nodiscard]] auto instructions_handler(std::string_view arguments,
                                        const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  if (arguments.empty()) {
    return SlashCommandResult{
        SlashCommandAction::manage_user_global_instructions, std::nullopt};
  }
  if (arguments == "on" || arguments == "off") {
    return SlashCommandResult{
        SlashCommandAction::manage_user_global_instructions,
        std::string{arguments}};
  }
  return command_error(SlashCommandErrorCode::invalid_arguments,
                       "instructions accepts on or off");
}

[[nodiscard]] auto tools_handler(std::string_view arguments,
                                 const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  if (arguments.empty()) {
    return SlashCommandResult{SlashCommandAction::manage_tool_profile,
                              std::nullopt};
  }
  const auto operation = take_argument(arguments);
  if (operation == "off" && arguments.empty()) {
    return SlashCommandResult{SlashCommandAction::disable_tools, std::nullopt};
  }
  if (operation == "reset" && arguments.empty()) {
    return SlashCommandResult{SlashCommandAction::reset_tool_narrowing,
                              std::nullopt};
  }

  if (operation == "profile") {
    if (auto result = profile_selection_result(arguments)) return *result;
  }

  if (operation == "category") {
    if (auto result = toggle_result(
            arguments, SlashCommandAction::enable_tool_category,
            SlashCommandAction::disable_tool_category, valid_tool_category))
      return *result;
  }

  if (operation == "tool") {
    if (auto result =
            toggle_result(arguments, SlashCommandAction::enable_tool,
                          SlashCommandAction::disable_tool, valid_tool_name))
      return *result;
  }

  if (operation == "model-max") {
    if (auto result = maximum_profile_result(
            arguments, SlashCommandAction::set_model_tool_profile_maximum,
            SlashCommandAction::inherit_model_tool_profile_maximum)) {
      return std::move(*result);
    }
  }
  if (operation == "persona-max") {
    if (auto result = maximum_profile_result(
            arguments, SlashCommandAction::set_persona_tool_profile_maximum,
            SlashCommandAction::inherit_persona_tool_profile_maximum)) {
      return std::move(*result);
    }
  }

  return command_error(SlashCommandErrorCode::invalid_arguments,
                       "tools accepts profile <id>, off, reset, category "
                       "<id> <on|off>, tool <name> <on|off>, model-max "
                       "<id>|inherit, or persona-max <id>|inherit");
}

[[nodiscard]] auto reasoning_handler(std::string_view arguments,
                                     const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  if (arguments == "show" || arguments == "hide") {
    return SlashCommandResult{SlashCommandAction::set_reasoning_visibility,
                              std::string{arguments}};
  }
  return command_error(SlashCommandErrorCode::invalid_arguments,
                       "reasoning accepts show or hide");
}

[[nodiscard]] auto plan_handler(std::string_view arguments,
                                const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return no_arguments(arguments, SlashCommandAction::show_plan);
}

[[nodiscard]] auto tasks_handler(std::string_view arguments,
                                 const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return no_arguments(arguments, SlashCommandAction::show_tasks);
}

[[nodiscard]] auto memory_handler(std::string_view arguments,
                                  const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  arguments = trim_arguments(arguments);
  return SlashCommandResult{
      SlashCommandAction::manage_memory,
      arguments.empty() ? std::nullopt : std::optional<std::string>{arguments}};
}

[[nodiscard]] auto builtin_specs() -> std::vector<SlashCommandSpec> {
  return {
      {"help", "help", "[command]", "Show available slash commands.",
       idle_available, help_handler},
      {"quit", "quit", "", "Exit interactive chat.", idle_available,
       quit_handler},
      {"clear", "clear", "", "Clear the transcript view only.", idle_available,
       clear_handler},
      {"edit", "edit", "", "Open an empty draft in the configured editor.",
       editor_available, edit_handler},
      {"session", "session", "[list | resume <session-id> | new]",
       "List, resume, or start interactive sessions.", idle_available,
       session_handler},
      {"persona", "persona", "[list | set <name> | off | manage]",
       "List, select, disable, or manage file-backed personas.", idle_available,
       persona_handler},
      {"character", "character", "[set <slug> | off]",
       "Choose or disable a provider character for future runs.",
       idle_available, character_handler},
      {"model", "model", "[model-id]", "Choose a text model for future runs.",
       idle_available, model_handler},
      {"settings", "settings", "",
       "Inspect or change request settings for future runs.", idle_available,
       settings_handler},
      {"instructions", "instructions", "[on | off]",
       "Inspect, edit, enable, or disable user-global instructions for future "
       "runs.",
       idle_available, instructions_handler},
      {"tools", "tools",
       "[profile <id> | off | reset | category <id> <on|off> | tool <name> "
       "<on|off> | model-max <id>|inherit | persona-max <id>|inherit]",
       "Inspect or narrow Chat tools and named maximum profiles for future "
       "runs.",
       idle_available, tools_handler},
      {"reasoning", "reasoning", "<show | hide>",
       "Show or hide retained reasoning text.", idle_available,
       reasoning_handler},
      {"usage", "usage", "", "Show session usage and reported cost.",
       idle_available, usage_handler},
      {"plan", "plan", "", "Show the current plan and review state.",
       idle_available, plan_handler},
      {"tasks", "tasks", "", "Show session tasks and project backlog.",
       idle_available, tasks_handler},
      {"memory", "memory",
       "[search <text> | accept|edit|reject|expire|accept-all|reject-all ...]",
       "Inspect and manage proposed, saved, and historical memory.",
       idle_available, memory_handler},
  };
}

} // namespace

auto SlashCommandRegistry::create(std::vector<SlashCommandSpec> commands)
    -> std::expected<SlashCommandRegistry, SlashCommandRegistryError> {
  try {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const auto& command : commands) {
      if (!valid_identifier(command.id, true)) {
        return registry_error(SlashCommandRegistryErrorCode::invalid_id,
                              "slash command ID is invalid", command.id);
      }
      if (!valid_identifier(command.name, false) ||
          command.name.size() > maximum_registered_name_bytes) {
        return registry_error(SlashCommandRegistryErrorCode::invalid_name,
                              "slash command name is invalid", command.id);
      }
      if (!metadata_valid(command.help) ||
          (!command.arguments.empty() &&
           !valid_utf8_text(command.arguments, true, true))) {
        return registry_error(SlashCommandRegistryErrorCode::invalid_metadata,
                              "slash command metadata is invalid", command.id);
      }
      if (!ids.insert(command.id).second) {
        return registry_error(SlashCommandRegistryErrorCode::duplicate_id,
                              "slash command IDs must be unique", command.id);
      }
      if (!names.insert(command.name).second) {
        return registry_error(SlashCommandRegistryErrorCode::duplicate_name,
                              "slash command names must be unique", command.id);
      }
      if (command.available == nullptr) {
        return registry_error(
            SlashCommandRegistryErrorCode::missing_availability,
            "slash command availability predicate is missing", command.id);
      }
      if (command.handler == nullptr) {
        return registry_error(SlashCommandRegistryErrorCode::missing_handler,
                              "slash command handler is missing", command.id);
      }
    }
    return SlashCommandRegistry{std::move(commands)};
  } catch (...) {
    return registry_error(SlashCommandRegistryErrorCode::internal_failure,
                          "slash command registry construction failed");
  }
}

auto SlashCommandRegistry::dispatch(
    const std::string_view input, const SlashCommandContext& context,
    const SlashCommandLimits limits) const noexcept
    -> std::expected<std::optional<SlashCommandResult>, SlashCommandError> {
  try {
    if (input.empty() || input.front() != '/') return std::nullopt;
    if (context.stop_token.stop_requested()) {
      return command_error(SlashCommandErrorCode::cancelled,
                           "slash command cancelled");
    }
    if (limits.maximum_input_bytes == 0 || limits.maximum_name_bytes == 0) {
      return command_error(SlashCommandErrorCode::invalid_input,
                           "slash command limits are invalid");
    }
    if (input.size() > limits.maximum_input_bytes) {
      return command_error(SlashCommandErrorCode::input_too_large,
                           "slash command exceeds the input limit");
    }
    if (!valid_utf8_text(input, true, true)) {
      return command_error(SlashCommandErrorCode::invalid_input,
                           "slash command must be valid text without controls");
    }

    const auto separator = input.find_first_of(" \t", 1);
    const auto name =
        input.substr(1, separator == std::string_view::npos ? input.size() - 1
                                                            : separator - 1);
    if (!valid_identifier(name, false) ||
        name.size() > limits.maximum_name_bytes) {
      return command_error(SlashCommandErrorCode::invalid_input,
                           "slash command name is invalid");
    }
    std::string_view arguments;
    if (separator != std::string_view::npos) {
      const auto first = input.find_first_not_of(" \t", separator);
      if (first != std::string_view::npos) arguments = input.substr(first);
    }

    const auto found =
        std::ranges::find(m_commands, name, &SlashCommandSpec::name);
    if (found == m_commands.end()) {
      return command_error(SlashCommandErrorCode::unknown_command,
                           "unknown slash command: /" + std::string{name});
    }
    if (!found->available(context)) {
      return command_error(SlashCommandErrorCode::unavailable_command,
                           "slash command is unavailable: /" +
                               std::string{name});
    }
    auto result = found->handler(arguments, context);
    if (!result) return std::unexpected(std::move(result.error()));
    return std::optional<SlashCommandResult>{std::move(*result)};
  } catch (...) {
    return command_error(SlashCommandErrorCode::internal_failure,
                         "slash command failed internally");
  }
}

auto SlashCommandRegistry::describe(
    const std::optional<std::string_view> name,
    const SlashCommandContext& context) const noexcept
    -> std::expected<std::vector<SlashCommandDescription>, SlashCommandError> {
  try {
    if (context.stop_token.stop_requested()) {
      return command_error(SlashCommandErrorCode::cancelled,
                           "slash command listing cancelled");
    }
    if (name && (!valid_identifier(*name, false) ||
                 name->size() > maximum_registered_name_bytes)) {
      return command_error(SlashCommandErrorCode::invalid_input,
                           "slash command help target is invalid");
    }
    std::vector<SlashCommandDescription> result;
    for (const auto& command : m_commands) {
      if (name && command.name != *name) continue;
      result.push_back({command.id, command.name, command.arguments,
                        command.help, command.available(context)});
    }
    if (name && result.empty()) {
      return command_error(SlashCommandErrorCode::unknown_command,
                           "unknown slash command: /" + std::string{*name});
    }
    return result;
  } catch (...) {
    return command_error(SlashCommandErrorCode::internal_failure,
                         "slash command listing failed internally");
  }
}

auto SlashCommandRegistry::complete(
    const std::string_view prefix, const SlashCommandContext& context,
    const SlashCommandLimits limits) const noexcept
    -> std::expected<std::vector<std::string>, SlashCommandError> {
  try {
    if (context.stop_token.stop_requested()) {
      return command_error(SlashCommandErrorCode::cancelled,
                           "slash command completion cancelled");
    }
    auto normalized = prefix;
    if (normalized.starts_with('/')) normalized.remove_prefix(1);
    if (limits.maximum_input_bytes == 0 || limits.maximum_name_bytes == 0) {
      return command_error(SlashCommandErrorCode::invalid_input,
                           "slash command limits are invalid");
    }
    if (normalized.size() > limits.maximum_name_bytes) {
      return command_error(SlashCommandErrorCode::input_too_large,
                           "slash command prefix exceeds the name limit");
    }
    if (!normalized.empty() && !valid_identifier(normalized, false)) {
      return command_error(SlashCommandErrorCode::invalid_input,
                           "slash command prefix is invalid");
    }
    std::vector<std::string> result;
    for (const auto& command : m_commands) {
      if (command.available(context) && command.name.starts_with(normalized)) {
        result.push_back(command.name);
      }
    }
    return result;
  } catch (...) {
    return command_error(SlashCommandErrorCode::internal_failure,
                         "slash command completion failed internally");
  }
}

auto builtin_slash_command_registry() -> const SlashCommandRegistry& {
  static const auto registry = [] {
    auto made = SlashCommandRegistry::create(builtin_specs());
    if (made) return std::move(*made);
    auto fallback = SlashCommandRegistry::create({});
    return std::move(*fallback);
  }();
  return registry;
}

} // namespace aiforge::surfaces
