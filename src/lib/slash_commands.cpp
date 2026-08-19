#include <aiforge/surfaces/slash_commands.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace aiforge::surfaces {
namespace {

constexpr std::size_t maximum_registered_name_bytes = 64U;

[[nodiscard]] auto registry_error(const SlashCommandRegistryErrorCode code,
                                  std::string message,
                                  std::string command_id = {})
    -> std::unexpected<SlashCommandRegistryError> {
  return std::unexpected(
      SlashCommandRegistryError{code, std::move(message),
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
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || (allow_dot && ch == '.');
  });
}

[[nodiscard]] auto valid_utf8_text(const std::string_view value,
                                   const bool allow_space,
                                   const bool allow_tab) -> bool {
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
      arguments.empty() ? std::nullopt
                        : std::optional<std::string>{arguments}};
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

[[nodiscard]] auto builtin_specs() -> std::vector<SlashCommandSpec> {
  return {
      {"help", "help", "[command]", "Show available slash commands.",
       idle_available, help_handler},
      {"quit", "quit", "", "Exit interactive chat.", idle_available,
       quit_handler},
      {"clear", "clear", "", "Clear the transcript view only.",
       idle_available, clear_handler},
      {"edit", "edit", "", "Open an empty draft in the configured editor.",
       editor_available, edit_handler},
  };
}

}  // namespace

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

auto SlashCommandRegistry::dispatch(const std::string_view input,
                                    const SlashCommandContext& context,
                                    const SlashCommandLimits limits) const
    noexcept
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
    const auto name = input.substr(
        1, separator == std::string_view::npos ? input.size() - 1
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

    const auto found = std::ranges::find(m_commands, name,
                                         &SlashCommandSpec::name);
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
    if (name &&
        (!valid_identifier(*name, false) ||
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

auto SlashCommandRegistry::complete(const std::string_view prefix,
                                    const SlashCommandContext& context,
                                    const SlashCommandLimits limits) const
    noexcept -> std::expected<std::vector<std::string>, SlashCommandError> {
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

}  // namespace aiforge::surfaces
