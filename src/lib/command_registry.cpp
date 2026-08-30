#include <aiforge/cli/command_registry.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <utility>
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
                               "command identity or name is invalid",
                               command.id);
  }
  if (!command_ids.insert(command.id).second) {
    return registry_diagnostic(RegistryDiagnosticCode::duplicate_command_id,
                               "command IDs must be globally unique",
                               command.id);
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
                                 "sibling command names must be unique",
                                 child.id);
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
    const auto found =
        std::ranges::find(result.command->subcommands, id, &CommandSpec::id);
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
  const auto found =
      std::ranges::find(registry.controls, kind, &ControlOptionSchema::kind);
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

[[nodiscard]] auto option_label(const CommandOptionSpec& option)
    -> std::string {
  auto result = joined(option.parser.names, ", ");
  if (option.parser.value_kind != ArgumentValueKind::flag) {
    result.append(" <");
    result.append(option.value_name.empty() ? "value" : option.value_name);
    result.push_back('>');
  }
  return result;
}

[[nodiscard]] auto version_text(const CommandRegistry& registry)
    -> std::string {
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

auto unavailable_handler(CommandContext& context) -> int {
  const auto& command = context.invocation.command_path.back();
  context.error << "aiforge: '" << command
                << "' is not available in this build\n";
  return failure_exit_code;
}

auto version_handler(CommandContext& context) -> int {
  context.output << PROGRAM_NAME << ' ' << project_version() << '\n';
  return success_exit_code;
}

[[nodiscard]] auto parsed_argument(const ParsedInvocation& invocation,
                                   const std::string_view id)
    -> const ParsedArgument* {
  const auto found =
      std::ranges::find(invocation.arguments, id, &ParsedArgument::id);
  return found == invocation.arguments.end() ? nullptr : &*found;
}

[[nodiscard]] auto parsed_text_values(const ParsedInvocation& invocation,
                                      const std::string_view id)
    -> std::optional<std::vector<std::string_view>> {
  const auto* argument = parsed_argument(invocation, id);
  if (argument == nullptr) return std::nullopt;
  std::vector<std::string_view> values;
  values.reserve(argument->values.size());
  for (const auto& value : argument->values) {
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr) return std::nullopt;
    values.push_back(*text);
  }
  return values;
}

auto one_shot_handler(CommandContext& context,
                      const std::string_view argument_id,
                      const bool allow_empty_for_interactive) -> int {
  const auto session_prefix =
      argument_id.starts_with("root.") ? "root.session." : "chat.session.";
  const auto option_prefix =
      argument_id.starts_with("root.") ? "root." : "chat.";
  const auto resume = parsed_text_values(
      context.invocation, std::string{session_prefix} + "resume");
  const bool continue_latest =
      parsed_argument(context.invocation,
                      std::string{session_prefix} + "continue") != nullptr;
  const bool ephemeral =
      parsed_argument(context.invocation,
                      std::string{session_prefix} + "ephemeral") != nullptr;
  const auto persona_name = parsed_text_values(
      context.invocation, std::string{option_prefix} + "persona");
  const auto requested_model = parsed_text_values(
      context.invocation, std::string{option_prefix} + "model");
  const auto requested_spend_ceiling = parsed_text_values(
      context.invocation, std::string{option_prefix} + "session-max-spend");
  const bool no_persona =
      parsed_argument(context.invocation,
                      std::string{option_prefix} + "no-persona") != nullptr;
  const auto selections = static_cast<int>(resume.has_value()) +
                          static_cast<int>(continue_latest) +
                          static_cast<int>(ephemeral);
  if (selections > 1) {
    context.error << "aiforge: --resume, --continue, and --ephemeral are "
                     "mutually exclusive\n";
    return usage_exit_code;
  }
  if (persona_name && no_persona) {
    context.error
        << "aiforge: --persona and --no-persona are mutually exclusive\n";
    return usage_exit_code;
  }
  if (requested_model &&
      (requested_model->size() != 1 || requested_model->front().empty())) {
    context.error << "aiforge: model ID is invalid\n";
    return usage_exit_code;
  }
  std::optional<domain::SessionSpendCeiling> session_spend_ceiling;
  if (requested_spend_ceiling) {
    if (requested_spend_ceiling->size() != 1) {
      context.error << "aiforge: session spend ceiling is invalid\n";
      return usage_exit_code;
    }
    auto parsed =
        domain::SessionSpendCeiling::from(requested_spend_ceiling->front());
    if (!parsed) {
      context.error << "aiforge: --session-max-spend requires a positive USD "
                       "decimal with at most 6 fractional digits\n";
      return usage_exit_code;
    }
    session_spend_ceiling = std::move(*parsed);
  }
  persona::PersonaDirective persona_directive;
  if (persona_name) {
    if (persona_name->size() != 1 || persona_name->front().empty()) {
      context.error << "aiforge: persona name is invalid\n";
      return usage_exit_code;
    }
    persona_directive.kind = persona::PersonaDirectiveKind::select;
    persona_directive.name = std::string{persona_name->front()};
  } else if (no_persona) {
    persona_directive.kind = persona::PersonaDirectiveKind::disable;
  }
  std::optional<domain::SessionId> resume_id;
  if (resume) {
    if (resume->size() != 1) return usage_exit_code;
    auto parsed = domain::SessionId::from(std::string{resume->front()});
    if (!parsed) {
      context.error << "aiforge: session ID is invalid\n";
      return usage_exit_code;
    }
    resume_id = std::move(*parsed);
  }

  const auto prompt = parsed_text_values(context.invocation, argument_id);
  if (!prompt || prompt->empty()) {
    if (allow_empty_for_interactive && context.environment.input_is_terminal &&
        context.environment.output_is_terminal) {
      if (context.environment.interactive == nullptr) {
        return unavailable_handler(context);
      }
      auto session_mode = InteractiveCommand::SessionMode::create;
      if (resume_id) {
        session_mode = InteractiveCommand::SessionMode::resume;
      } else if (continue_latest) {
        session_mode = InteractiveCommand::SessionMode::continue_latest;
      } else if (ephemeral) {
        session_mode = InteractiveCommand::SessionMode::ephemeral;
      }
      auto result = context.environment.interactive->execute(
          {session_mode, std::move(resume_id), std::move(persona_directive),
           requested_model
               ? std::optional<std::string>{requested_model->front()}
               : std::nullopt,
           session_spend_ceiling},
          context.environment, context.output, context.error);
      if (result) return success_exit_code;
      if (!result.error().message.empty()) {
        context.error << "aiforge: " << result.error().message << '\n';
      }
      switch (result.error().kind) {
        case CommandFailureKind::usage: return usage_exit_code;
        case CommandFailureKind::cancelled: return 130;
        case CommandFailureKind::runtime: return failure_exit_code;
      }
    }
    context.error << "aiforge: a prompt is required for one-shot input\n";
    return usage_exit_code;
  }
  if (prompt->size() != 1 || prompt->front().empty()) {
    context.error << "aiforge: prompt must be nonempty\n";
    return usage_exit_code;
  }
  if (context.environment.one_shot == nullptr) {
    return unavailable_handler(context);
  }
  auto session_mode = OneShotCommand::SessionMode::create;
  if (resume_id) {
    session_mode = OneShotCommand::SessionMode::resume;
  } else if (continue_latest) {
    session_mode = OneShotCommand::SessionMode::continue_latest;
  } else if (ephemeral) {
    session_mode = OneShotCommand::SessionMode::ephemeral;
  }
  auto result = context.environment.one_shot->execute(
      {prompt->front(), session_mode, std::move(resume_id),
       std::move(persona_directive),
       requested_model ? std::optional<std::string>{requested_model->front()}
                       : std::nullopt,
       session_spend_ceiling},
      context.environment, context.output, context.error);
  if (result) return success_exit_code;
  if (!result.error().message.empty()) {
    context.error << "aiforge: " << result.error().message << '\n';
  }
  switch (result.error().kind) {
    case CommandFailureKind::usage: return usage_exit_code;
    case CommandFailureKind::cancelled: return 130;
    case CommandFailureKind::runtime: return failure_exit_code;
  }
  return failure_exit_code;
}

auto root_handler(CommandContext& context) -> int {
  return one_shot_handler(context, "root.prompt", true);
}

auto chat_handler(CommandContext& context) -> int {
  return one_shot_handler(context, "chat.prompt", false);
}

auto models_handler(CommandContext& context) -> int {
  if (context.environment.models == nullptr)
    return unavailable_handler(context);
  auto result = context.environment.models->execute(
      context.environment, context.output, context.error);
  if (result) return success_exit_code;
  if (!result.error().message.empty())
    context.error << "aiforge: " << result.error().message << '\n';
  switch (result.error().kind) {
    case CommandFailureKind::usage: return usage_exit_code;
    case CommandFailureKind::cancelled: return 130;
    case CommandFailureKind::runtime: return failure_exit_code;
  }
  return failure_exit_code;
}

[[nodiscard]] auto command_result(std::expected<void, CommandFailure> result,
                                  CommandContext& context) -> int {
  if (result) return success_exit_code;
  if (!result.error().message.empty()) {
    context.error << "aiforge: " << result.error().message << '\n';
  }
  switch (result.error().kind) {
    case CommandFailureKind::usage: return usage_exit_code;
    case CommandFailureKind::cancelled: return 130;
    case CommandFailureKind::runtime: return failure_exit_code;
  }
  return failure_exit_code;
}

auto image_parent_handler(CommandContext& context) -> int {
  context.error << "aiforge: an image subcommand is required\n";
  return usage_exit_code;
}

auto image_generate_handler(CommandContext& context) -> int {
  const auto prompt =
      parsed_text_values(context.invocation, "image.generate.prompt");
  const auto model =
      parsed_text_values(context.invocation, "image.generate.model");
  const auto format =
      parsed_text_values(context.invocation, "image.generate.format");
  const auto output =
      parsed_text_values(context.invocation, "image.generate.output");
  if (!prompt || prompt->empty() || !model || model->size() != 1 ||
      model->front().empty() || (format && format->size() != 1) ||
      (output && output->size() != 1)) {
    return usage_exit_code;
  }
  std::string joined_prompt;
  for (const auto word : *prompt) {
    if (!joined_prompt.empty()) joined_prompt.push_back(' ');
    joined_prompt.append(word);
  }
  std::optional<std::string> selected_format;
  if (format) {
    if (format->front() != "auto" && format->front() != "png" &&
        format->front() != "jpeg" && format->front() != "webp") {
      context.error << "aiforge: --format must be auto, png, jpeg, or webp\n";
      return usage_exit_code;
    }
    selected_format = std::string{format->front()};
  }
  if (context.environment.image == nullptr) return unavailable_handler(context);
  return command_result(
      context.environment.image->generate(
          {std::move(joined_prompt), std::string{model->front()},
           std::move(selected_format),
           output ? std::optional<std::string>{output->front()} : std::nullopt},
          context.environment, context.output, context.error),
      context);
}

auto image_show_handler(CommandContext& context) -> int {
  const auto session =
      parsed_text_values(context.invocation, "image.show.session");
  const auto artifact =
      parsed_text_values(context.invocation, "image.show.artifact");
  const auto output =
      parsed_text_values(context.invocation, "image.show.output");
  if (!session || session->size() != 1 || (artifact && artifact->size() != 1) ||
      (output && output->size() != 1)) {
    return usage_exit_code;
  }
  auto session_id = domain::SessionId::from(std::string{session->front()});
  if (!session_id) {
    context.error << "aiforge: session ID is invalid\n";
    return usage_exit_code;
  }
  std::optional<domain::ArtifactId> artifact_id;
  if (artifact) {
    auto parsed = domain::ArtifactId::from(std::string{artifact->front()});
    if (!parsed) {
      context.error << "aiforge: artifact ID is invalid\n";
      return usage_exit_code;
    }
    artifact_id = std::move(*parsed);
  }
  if (context.environment.image == nullptr) return unavailable_handler(context);
  return command_result(
      context.environment.image->show(
          {std::move(*session_id), std::move(artifact_id),
           output ? std::optional<std::string>{output->front()} : std::nullopt},
          context.environment, context.output, context.error),
      context);
}

auto plan_handler(CommandContext& context) -> int {
  const auto resume =
      parsed_text_values(context.invocation, "plan.session.resume");
  const bool continue_latest =
      parsed_argument(context.invocation, "plan.session.continue") != nullptr;
  const bool jsonl =
      parsed_argument(context.invocation, "plan.jsonl") != nullptr;
  if (!jsonl) {
    context.error << "aiforge: plan currently requires --jsonl\n";
    return usage_exit_code;
  }
  if (static_cast<int>(resume.has_value()) +
          static_cast<int>(continue_latest) !=
      1) {
    context.error
        << "aiforge: plan requires exactly one of --resume or --continue\n";
    return usage_exit_code;
  }
  if (context.environment.input_is_terminal) {
    context.error
        << "aiforge: plan --jsonl requires noninteractive standard input\n";
    return usage_exit_code;
  }
  std::optional<domain::SessionId> session_id;
  if (resume) {
    if (resume->size() != 1) return usage_exit_code;
    auto parsed = domain::SessionId::from(std::string{resume->front()});
    if (!parsed) {
      context.error << "aiforge: session ID is invalid\n";
      return usage_exit_code;
    }
    session_id = std::move(*parsed);
  }
  if (context.environment.plan == nullptr) return unavailable_handler(context);
  auto result = context.environment.plan->execute(
      {continue_latest ? PlanCommand::SessionMode::continue_latest
                       : PlanCommand::SessionMode::resume,
       std::move(session_id)},
      context.environment, context.output, context.error);
  if (result) return success_exit_code;
  if (!result.error().message.empty()) {
    context.error << "aiforge: " << result.error().message << '\n';
  }
  switch (result.error().kind) {
    case CommandFailureKind::usage: return usage_exit_code;
    case CommandFailureKind::cancelled: return 130;
    case CommandFailureKind::runtime: return failure_exit_code;
  }
  return failure_exit_code;
}

auto login_handler(CommandContext& context) -> int {
  if (context.environment.login == nullptr) return unavailable_handler(context);
  auto result = context.environment.login->execute(
      context.environment, context.output, context.error);
  if (result) return success_exit_code;
  if (!result.error().message.empty()) {
    context.error << "aiforge: " << result.error().message << '\n';
  }
  switch (result.error().kind) {
    case CommandFailureKind::usage: return usage_exit_code;
    case CommandFailureKind::cancelled: return 130;
    case CommandFailureKind::runtime: return failure_exit_code;
  }
  return failure_exit_code;
}

auto write_config_warning(std::ostream& error, const std::string_view message)
    -> void {
  error << "aiforge: warning: " << message << '\n';
}

[[nodiscard]] auto resolved_process_config(std::ostream& error)
    -> std::expected<config::ResolvedConfig, int> {
  const auto& registry = config::builtin_config_registry();
  std::vector<config::ConfigLayer> layers;
  auto environment = config::environment_config_layer(registry);
  if (!environment) {
    error << "aiforge: " << environment.error().message << '\n';
    return std::unexpected(failure_exit_code);
  }
  layers.push_back(std::move(*environment));

  auto path = config::process_config_path();
  if (!path) {
    write_config_warning(error, path.error().message);
  } else {
    config::JsonConfigFileStore store{*path};
    auto file = store.load(registry);
    if (file) {
      layers.push_back(std::move(*file));
    } else {
      write_config_warning(error, file.error().message);
    }
  }

  auto resolved = config::resolve_config(registry, layers);
  if (!resolved) {
    error << "aiforge: " << resolved.error().message << '\n';
    return std::unexpected(failure_exit_code);
  }
  for (const auto& warning : resolved->diagnostics) {
    write_config_warning(error, warning.message);
  }
  return std::move(*resolved);
}

auto config_parent_handler(CommandContext& context) -> int {
  context.error << "aiforge: a config subcommand is required\n";
  return usage_exit_code;
}

auto config_show_handler(CommandContext& context) -> int {
  auto resolved = resolved_process_config(context.error);
  if (!resolved) return resolved.error();
  for (const auto& entry : resolved->entries) {
    context.output << entry.key << '\t';
    if (entry.sensitive && entry.value) {
      context.output << "<redacted>";
    } else if (entry.value) {
      context.output << config::format_config_value(*entry.value);
    } else {
      context.output << "<unset>";
    }
    context.output << '\t'
                   << (entry.source ? config::config_source_name(*entry.source)
                                    : std::string_view{"unset"})
                   << '\n';
  }
  return success_exit_code;
}

auto config_get_handler(CommandContext& context) -> int {
  const auto key = parsed_text_values(context.invocation, "config.get.key");
  if (!key || key->size() != 1) return usage_exit_code;
  auto resolved = resolved_process_config(context.error);
  if (!resolved) return resolved.error();
  const auto* entry = resolved->find(key->front());
  if (entry == nullptr) {
    context.error << "aiforge: unknown configuration key\n";
    return usage_exit_code;
  }
  if (!entry->value) {
    context.error << "aiforge: configuration key is unset\n";
    return failure_exit_code;
  }
  context.output << (entry->sensitive
                         ? "<redacted>"
                         : config::format_config_value(*entry->value))
                 << '\n';
  return success_exit_code;
}

auto config_set_handler(CommandContext& context) -> int {
  const auto assignment =
      parsed_text_values(context.invocation, "config.set.assignment");
  if (!assignment || assignment->size() < 2) return usage_exit_code;
  const auto key = assignment->front();
  const auto values = std::span<const std::string_view>{*assignment}.subspan(1);
  const auto& registry = config::builtin_config_registry();
  const auto found =
      std::ranges::find(registry.keys, key, &config::ConfigKeySpec::id);
  if (found == registry.keys.end() || !found->file_writable ||
      found->sensitive) {
    context.error << "aiforge: unknown or non-writable configuration key\n";
    return usage_exit_code;
  }
  auto parsed =
      config::parse_config_value(*found, values, config::ConfigSource::file);
  if (!parsed) {
    context.error << "aiforge: " << parsed.error().message << '\n';
    return usage_exit_code;
  }
  auto path = config::process_config_path();
  if (!path) {
    context.error << "aiforge: " << path.error().message << '\n';
    return failure_exit_code;
  }
  auto changed = config::JsonConfigFileStore{*path}.set(registry, key, *parsed);
  if (!changed) {
    context.error << "aiforge: " << changed.error().message << '\n';
    return failure_exit_code;
  }
  return success_exit_code;
}

auto config_unset_handler(CommandContext& context) -> int {
  const auto key = parsed_text_values(context.invocation, "config.unset.key");
  if (!key || key->size() != 1) return usage_exit_code;
  const auto& registry = config::builtin_config_registry();
  const auto found = std::ranges::find(registry.keys, key->front(),
                                       &config::ConfigKeySpec::id);
  if (found == registry.keys.end() || !found->file_writable ||
      found->sensitive) {
    context.error << "aiforge: unknown or non-writable configuration key\n";
    return usage_exit_code;
  }
  auto path = config::process_config_path();
  if (!path) {
    context.error << "aiforge: " << path.error().message << '\n';
    return failure_exit_code;
  }
  auto changed =
      config::JsonConfigFileStore{*path}.unset(registry, key->front());
  if (!changed) {
    context.error << "aiforge: " << changed.error().message << '\n';
    return failure_exit_code;
  }
  return success_exit_code;
}

} // namespace

auto make_parser_schema(const CommandRegistry& registry)
    -> std::expected<ParserSchema, RegistryDiagnostic> {
  if (registry.program_name.empty() || registry.version.empty()) {
    return std::unexpected(
        registry_diagnostic(RegistryDiagnosticCode::invalid_program,
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
  const auto version_names =
      control_names(registry, ControlRequestKind::version);
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
  CommandEnvironment environment{std::cin, true,    true,   true,
                                 {},       nullptr, nullptr};
  return dispatch(registry, arguments, environment, output, error, limits);
}

auto CommandDispatcher::dispatch(
    const CommandRegistry& registry,
    const std::span<const std::string_view> arguments,
    CommandEnvironment& environment, std::ostream& output, std::ostream& error,
    const ParseLimits limits) const noexcept -> int {
  try {
    auto schema = make_parser_schema(registry);
    if (!schema) {
      safe_write(error,
                 registry.program_name + ": internal command registry error\n");
      return failure_exit_code;
    }

    auto parsed = ArgumentParser{}.parse(*schema, arguments, limits);
    if (!parsed) {
      const auto internal =
          parsed.error().code == ParseDiagnosticCode::invalid_schema ||
          parsed.error().code == ParseDiagnosticCode::invalid_limits ||
          parsed.error().code == ParseDiagnosticCode::adapter_failure;
      if (internal) {
        safe_write(error, registry.program_name +
                              ": internal command dispatch error\n");
        return failure_exit_code;
      }
      auto message =
          registry.program_name + ": " + parsed.error().message + "\n";
      message +=
          "Try '" + registry.program_name + " --help' for more information.\n";
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
      safe_write(error,
                 registry.program_name + ": internal command registry error\n");
      return failure_exit_code;
    }
    CommandContext context{invocation, environment, output, error};
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
  const auto session_options = [](const std::string_view prefix) {
    return std::vector<CommandOptionSpec>{
        {{std::string{prefix} + ".session.resume",
          {"--resume"},
          ArgumentValueKind::text,
          0,
          1},
         "session-id",
         "Resume an exact durable session."},
        {{std::string{prefix} + ".session.continue",
          {"--continue"},
          ArgumentValueKind::flag,
          0,
          1},
         {},
         "Continue the most recently active durable session."},
        {{std::string{prefix} + ".session.ephemeral",
          {"--ephemeral"},
          ArgumentValueKind::flag,
          0,
          1},
         {},
         "Run without creating or opening durable session storage."},
        {{std::string{prefix} + ".persona",
          {"--persona"},
          ArgumentValueKind::text,
          0,
          1},
         "name",
         "Select a file-backed persona."},
        {{std::string{prefix} + ".model",
          {"--model"},
          ArgumentValueKind::text,
          0,
          1},
         "model-id",
         "Select and validate a text model."},
        {{std::string{prefix} + ".session-max-spend",
          {"--session-max-spend"},
          ArgumentValueKind::text,
          0,
          1},
         "USD",
         "Set or narrow the durable session inference spend ceiling."},
        {{std::string{prefix} + ".no-persona",
          {"--no-persona"},
          ArgumentValueKind::flag,
          0,
          1},
         {},
         "Disable any inherited persona."}};
  };
  static const CommandRegistry registry{
      std::string{PROGRAM_NAME},
      project_version(),
      {"root",
       "",
       "AIForge terminal AI client.",
       false,
       session_options("root"),
       {{{"root.prompt", "prompt", ArgumentValueKind::text, 0, 1},
         "Prompt text for a one-shot request."}},
       {{"chat",
         "chat",
         "Run a one-shot chat request.",
         false,
         session_options("chat"),
         {{{"chat.prompt", "prompt", ArgumentValueKind::text, 1, 1},
           "Prompt text."}},
         {},
         chat_handler},
        {"models",
         "models",
         "List available models.",
         false,
         {},
         {},
         {},
         models_handler},
        {"image",
         "image",
         "Generate or redisplay durable image artifacts.",
         true,
         {},
         {},
         {{"image-generate",
           "generate",
           "Generate and store an image.",
           false,
           {{{"image.generate.model",
              {"--model"},
              ArgumentValueKind::text,
              1,
              1},
             "model-id",
             "Select and validate an image model."},
            {{"image.generate.format",
              {"--format"},
              ArgumentValueKind::text,
              0,
              1},
             "auto|png|jpeg|webp",
             "Request an encoded image format."},
            {{"image.generate.output",
              {"--output"},
              ArgumentValueKind::text,
              0,
              1},
             "path",
             "Export the stored bytes without overwriting an existing file."}},
           {{{"image.generate.prompt", "prompt", ArgumentValueKind::text, 1,
              256},
             "Image prompt text."}},
           {},
           image_generate_handler},
          {"image-show",
           "show",
           "Redisplay an image from durable session history.",
           false,
           {{{"image.show.session",
              {"--session"},
              ArgumentValueKind::text,
              1,
              1},
             "session-id",
             "Select an exact durable session."},
            {{"image.show.artifact",
              {"--artifact"},
              ArgumentValueKind::text,
              0,
              1},
             "artifact-id",
             "Select an exact image artifact; the latest is the default."},
            {{"image.show.output", {"--output"}, ArgumentValueKind::text, 0, 1},
             "path",
             "Export the stored bytes without overwriting an existing file."}},
           {},
           {},
           image_show_handler}},
         image_parent_handler},
        {"login",
         "login",
         "Store a Venice API credential from terminal input.",
         false,
         {},
         {},
         {},
         login_handler},
        {"plan",
         "plan",
         "Inspect and control durable plans and tasks through JSON Lines.",
         false,
         {{{"plan.jsonl", {"--jsonl"}, ArgumentValueKind::flag, 0, 1},
           {},
           "Use the versioned JSON Lines control protocol."},
          {{"plan.session.resume", {"--resume"}, ArgumentValueKind::text, 0, 1},
           "session-id",
           "Resume an exact durable session."},
          {{"plan.session.continue",
            {"--continue"},
            ArgumentValueKind::flag,
            0,
            1},
           {},
           "Use the most recently active durable session."}},
         {},
         {},
         plan_handler},
        {"config",
         "config",
         "Inspect or update configuration.",
         true,
         {},
         {},
         {{"config-show",
           "show",
           "Show resolved configuration.",
           false,
           {},
           {},
           {},
           config_show_handler},
          {"config-get",
           "get",
           "Get one resolved configuration value.",
           false,
           {},
           {{{"config.get.key", "key", ArgumentValueKind::text, 1, 1},
             "Dotted configuration key."}},
           {},
           config_get_handler},
          {"config-set",
           "set",
           "Set one configuration-file value.",
           false,
           {},
           {{{"config.set.assignment", "key-value", ArgumentValueKind::text, 2,
              257},
             "Dotted key followed by its value or list items."}},
           {},
           config_set_handler},
          {"config-unset",
           "unset",
           "Remove one configuration-file value.",
           false,
           {},
           {{{"config.unset.key", "key", ArgumentValueKind::text, 1, 1},
             "Dotted configuration key."}},
           {},
           config_unset_handler}},
         config_parent_handler},
        {"version",
         "version",
         "Show version information.",
         false,
         {},
         {},
         {},
         version_handler}},
       root_handler},
      {{ControlRequestKind::help, {"-h", "--help"}},
       {ControlRequestKind::version, {"--version"}}}};
  return registry;
}

auto run_cli(const std::span<const std::string_view> arguments,
             std::ostream& output, std::ostream& error) noexcept -> int {
  return CommandDispatcher{}.dispatch(builtin_command_registry(), arguments,
                                      output, error);
}

auto run_cli(const std::span<const std::string_view> arguments,
             CommandEnvironment& environment, std::ostream& output,
             std::ostream& error) noexcept -> int {
  return CommandDispatcher{}.dispatch(builtin_command_registry(), arguments,
                                      environment, output, error);
}

auto project_version() -> std::string {
  auto result = std::to_string(VERSION_MAJOR) + "." +
                std::to_string(VERSION_MINOR) + "." +
                std::to_string(VERSION_PATCH);
  if (VERSION_TWEAK != 0) {
    result += "." + std::to_string(VERSION_TWEAK);
  }
  return result;
}

} // namespace aiforge::cli
