#include <aiforge/cli/command_registry.hpp>

#include <algorithm>
#include <exception>
#include <optional>
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <utility>

#include <aiforge/bootstrap.hpp>
#include <version.hpp>

namespace aiforge::cli {
namespace {

constexpr int success_exit_code = 0;
constexpr int failure_exit_code = 1;
constexpr int usage_exit_code = 2;

[[nodiscard]] auto registry_diagnostic(const RegistryDiagnosticCode code,
                                       std::string message,
                                       std::string command_id = {})
    -> RegistryDiagnostic {
  return {code, std::move(message), std::move(command_id)};
}

[[nodiscard]] auto validate_command(
    const CommandSpec& command, const bool root,
    std::unordered_set<std::string>& command_ids)
    -> std::optional<RegistryDiagnostic> {
  if (command.id.empty() || (root && !command.name.empty()) ||
      (!root && command.name.empty())) {
    return registry_diagnostic(RegistryDiagnosticCode::invalid_command,
                               "command identity or name is invalid", command.id);
  }
  if (!command_ids.insert(command.id).second) {
    return registry_diagnostic(RegistryDiagnosticCode::duplicate_command_id,
                               "command IDs must be globally unique", command.id);
  }
  if (command.handler == nullptr) {
    return registry_diagnostic(RegistryDiagnosticCode::missing_handler,
                               "every selectable command requires a handler",
                               command.id);
  }

  std::unordered_set<std::string> names;
  for (const auto& child : command.subcommands) {
    if (!names.insert(child.name).second) {
      return registry_diagnostic(RegistryDiagnosticCode::duplicate_command_name,
                                 "sibling command names must be unique", child.id);
    }
    if (auto error = validate_command(child, false, command_ids)) return error;
  }
  return std::nullopt;
}

[[nodiscard]] auto parser_command(const CommandSpec& command) -> CommandSchema {
  CommandSchema result;
  result.id = command.id;
  result.name = command.name;
  result.subcommand_required = command.subcommand_required;
  result.options.reserve(command.options.size());
  for (const auto& option : command.options) {
    result.options.push_back(option.parser);
  }
  result.positionals.reserve(command.positionals.size());
  for (const auto& positional : command.positionals) {
    result.positionals.push_back(positional.parser);
  }
  result.subcommands.reserve(command.subcommands.size());
  for (const auto& child : command.subcommands) {
    result.subcommands.push_back(parser_command(child));
  }
  return result;
}

struct LocatedCommand {
  const CommandSpec* command{};
  std::vector<std::string_view> names;
};

[[nodiscard]] auto locate_command(
    const CommandRegistry& registry,
    const std::span<const std::string> command_path)
    -> std::expected<LocatedCommand, RegistryDiagnostic> {
  if (command_path.empty() || command_path.front() != registry.root.id) {
    return std::unexpected(registry_diagnostic(
        RegistryDiagnosticCode::unknown_command_path,
        "the command path does not start at the registry root"));
  }

  LocatedCommand result{&registry.root, {}};
  for (const auto& id : command_path.subspan(1)) {
    const auto found = std::ranges::find(result.command->subcommands, id,
                                         &CommandSpec::id);
    if (found == result.command->subcommands.end()) {
      return std::unexpected(registry_diagnostic(
          RegistryDiagnosticCode::unknown_command_path,
          "the command path is not present in the registry", id));
    }
    result.command = &*found;
    result.names.push_back(result.command->name);
  }
  return result;
}

[[nodiscard]] auto control_names(const CommandRegistry& registry,
                                 const ControlRequestKind kind)
    -> std::vector<std::string> {
  const auto found = std::ranges::find(registry.controls, kind,
                                       &ControlOptionSchema::kind);
  return found == registry.controls.end() ? std::vector<std::string>{}
                                          : found->names;
}

[[nodiscard]] auto joined(const std::vector<std::string>& values,
                          const std::string_view separator) -> std::string {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result.append(separator);
    result.append(value);
  }
  return result;
}

auto append_rows(std::ostringstream& output,
                 const std::vector<std::pair<std::string, std::string>>& rows)
    -> void {
  std::size_t width{};
  for (const auto& [label, description] : rows) {
    static_cast<void>(description);
    width = std::max(width, label.size());
  }
  for (const auto& [label, description] : rows) {
    output << "  " << label << std::string(width - label.size() + 2, ' ')
           << description << '\n';
  }
}

[[nodiscard]] auto command_line(const CommandRegistry& registry,
                                const LocatedCommand& located) -> std::string {
  std::string result = registry.program_name;
  for (const auto name : located.names) {
    result.push_back(' ');
    result.append(name);
  }
  return result;
}

[[nodiscard]] auto option_label(const CommandOptionSpec& option) -> std::string {
  auto result = joined(option.parser.names, ", ");
  if (option.parser.value_kind != ArgumentValueKind::flag) {
    result.append(" <");
    result.append(option.value_name.empty() ? "value" : option.value_name);
    result.push_back('>');
  }
  return result;
}

[[nodiscard]] auto version_text(const CommandRegistry& registry) -> std::string {
  return registry.program_name + " " + registry.version + "\n";
}

auto safe_write(std::ostream& stream, const std::string_view value) noexcept
    -> bool {
  try {
    stream << value;
    return static_cast<bool>(stream);
  } catch (...) {
    return false;
  }
}

auto write_internal_failure(std::ostream& error,
                            const std::string_view program_name) noexcept
    -> void {
  safe_write(error, program_name);
  safe_write(error, ": command failed internally\n");
}

[[nodiscard]] auto format_project_version() -> std::string {
  auto result = std::to_string(VERSION_MAJOR) + "." +
                std::to_string(VERSION_MINOR) + "." +
                std::to_string(VERSION_PATCH);
  if (VERSION_TWEAK != 0) {
    result += "." + std::to_string(VERSION_TWEAK);
  }
  return result;
}

auto default_handler(CommandContext& context) -> int {
  context.error << bootstrap_status() << '\n';
  return success_exit_code;
}

auto unavailable_handler(CommandContext& context) -> int {
  const auto& command = context.invocation.command_path.back();
  context.error << "aiforge: '" << command
                << "' is not available in this build\n";
  return failure_exit_code;
}

auto version_handler(CommandContext& context) -> int {
  context.output << PROGRAM_NAME << ' ' << format_project_version() << '\n';
  return success_exit_code;
}

}  // namespace

auto make_parser_schema(const CommandRegistry& registry)
    -> std::expected<ParserSchema, RegistryDiagnostic> {
  if (registry.program_name.empty() || registry.version.empty()) {
    return std::unexpected(registry_diagnostic(
        RegistryDiagnosticCode::invalid_program,
        "the program name and version must be nonempty"));
  }
  std::unordered_set<std::string> command_ids;
  if (auto error = validate_command(registry.root, true, command_ids)) {
    return std::unexpected(std::move(*error));
  }
  return ParserSchema{parser_command(registry.root), registry.controls};
}

auto render_help(const CommandRegistry& registry,
                 const std::span<const std::string> command_path)
    -> std::expected<std::string, RegistryDiagnostic> {
  auto schema = make_parser_schema(registry);
  if (!schema) return std::unexpected(std::move(schema.error()));
  auto located = locate_command(registry, command_path);
  if (!located) return std::unexpected(std::move(located.error()));

  const auto& command = *located->command;
  std::ostringstream output;
  output << "Usage: " << command_line(registry, *located) << " [options]";
  if (!command.subcommands.empty()) {
    output << (command.subcommand_required ? " <command>" : " [command]");
  }
  for (const auto& positional : command.positionals) {
    const bool required = positional.parser.minimum_values != 0;
    output << ' ' << (required ? '<' : '[') << positional.parser.name;
    if (positional.parser.maximum_values > 1) output << "...";
    output << (required ? '>' : ']');
  }
  output << "\n\n" << command.help << '\n';

  if (!command.positionals.empty()) {
    output << "\nArguments:\n";
    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(command.positionals.size());
    for (const auto& positional : command.positionals) {
      rows.emplace_back(positional.parser.name, positional.help);
    }
    append_rows(output, rows);
  }

  std::vector<std::pair<std::string, std::string>> option_rows;
  option_rows.reserve(command.options.size() + registry.controls.size());
  for (const auto& option : command.options) {
    option_rows.emplace_back(option_label(option), option.help);
  }
  const auto help_names = control_names(registry, ControlRequestKind::help);
  if (!help_names.empty()) {
    option_rows.emplace_back(joined(help_names, ", "),
                             "Show help for this command.");
  }
  const auto version_names = control_names(registry, ControlRequestKind::version);
  if (!version_names.empty()) {
    option_rows.emplace_back(joined(version_names, ", "),
                             "Show version information.");
  }
  if (!option_rows.empty()) {
    output << "\nOptions:\n";
    append_rows(output, option_rows);
  }

  if (!command.subcommands.empty()) {
    output << "\nCommands:\n";
    std::vector<std::pair<std::string, std::string>> command_rows;
    command_rows.reserve(command.subcommands.size());
    for (const auto& child : command.subcommands) {
      command_rows.emplace_back(child.name, child.help);
    }
    append_rows(output, command_rows);
  }
  return std::move(output).str();
}

auto CommandDispatcher::dispatch(
    const CommandRegistry& registry,
    const std::span<const std::string_view> arguments, std::ostream& output,
    std::ostream& error, const ParseLimits limits) const noexcept -> int {
  try {
    auto schema = make_parser_schema(registry);
    if (!schema) {
      safe_write(error, registry.program_name +
                            ": internal command registry error\n");
      return failure_exit_code;
    }

    auto parsed = ArgumentParser{}.parse(*schema, arguments, limits);
    if (!parsed) {
      const auto internal = parsed.error().code == ParseDiagnosticCode::invalid_schema ||
                            parsed.error().code == ParseDiagnosticCode::invalid_limits ||
                            parsed.error().code == ParseDiagnosticCode::adapter_failure;
      if (internal) {
        safe_write(error, registry.program_name +
                              ": internal command dispatch error\n");
        return failure_exit_code;
      }
      auto message = registry.program_name + ": " + parsed.error().message + "\n";
      message += "Try '" + registry.program_name +
                 " --help' for more information.\n";
      safe_write(error, message);
      return usage_exit_code;
    }

    if (const auto* control = std::get_if<ControlRequest>(&*parsed)) {
      if (control->kind == ControlRequestKind::version) {
        return safe_write(output, version_text(registry)) ? success_exit_code
                                                          : failure_exit_code;
      }
      auto help = render_help(registry, control->command_path);
      if (!help) {
        safe_write(error, registry.program_name +
                              ": internal command registry error\n");
        return failure_exit_code;
      }
      return safe_write(output, *help) ? success_exit_code : failure_exit_code;
    }

    auto& invocation = std::get<ParsedInvocation>(*parsed);
    auto located = locate_command(registry, invocation.command_path);
    if (!located || located->command->handler == nullptr) {
      safe_write(error, registry.program_name +
                            ": internal command registry error\n");
      return failure_exit_code;
    }
    CommandContext context{invocation, output, error};
    const auto result = located->command->handler(context);
    return output && error ? result : failure_exit_code;
  } catch (const std::exception&) {
    write_internal_failure(error, registry.program_name);
    return failure_exit_code;
  } catch (...) {
    write_internal_failure(error, registry.program_name);
    return failure_exit_code;
  }
}

auto builtin_command_registry() -> const CommandRegistry& {
  static const CommandRegistry registry{
      std::string{PROGRAM_NAME},
      format_project_version(),
      {"root",
       "",
       "AIForge terminal AI client.",
       false,
       {},
       {},
       {{"chat",
         "chat",
         "Run a one-shot chat request.",
         false,
         {},
         {{{"chat.prompt", "prompt", ArgumentValueKind::text, 0, 1},
           "Prompt text."}},
         {},
         unavailable_handler},
        {"models", "models", "List available models.", false, {}, {}, {},
         unavailable_handler},
        {"config", "config", "Inspect or update configuration.", false, {},
         {}, {}, unavailable_handler},
        {"version", "version", "Show version information.", false, {}, {}, {},
         version_handler}},
       default_handler},
      {{ControlRequestKind::help, {"-h", "--help"}},
       {ControlRequestKind::version, {"--version"}}}};
  return registry;
}

auto run_cli(const std::span<const std::string_view> arguments,
             std::ostream& output, std::ostream& error) noexcept -> int {
  return CommandDispatcher{}.dispatch(builtin_command_registry(), arguments,
                                      output, error);
}

}  // namespace aiforge::cli
