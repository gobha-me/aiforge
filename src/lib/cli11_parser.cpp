#include <aiforge/cli/parser.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <charconv>
#include <climits>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aiforge::cli {
namespace {

struct ArgumentBinding {
  std::string id;
  ArgumentValueKind value_kind{ArgumentValueKind::text};
  std::size_t minimum{};
  std::size_t maximum{1};
  CLI::Option* option{};
  std::vector<std::string> raw_values;
};

struct CommandBinding {
  const CommandSchema* schema{};
  CLI::App* app{};
  std::vector<std::unique_ptr<ArgumentBinding>> options;
  std::vector<std::unique_ptr<ArgumentBinding>> positionals;
  std::vector<std::unique_ptr<CommandBinding>> subcommands;
};

[[nodiscard]] auto diagnostic(const ParseDiagnosticCode code, std::string message,
                              std::optional<std::string> schema_id = std::nullopt,
                              std::optional<std::size_t> token_index = std::nullopt)
    -> ParseDiagnostic {
  return {code, token_index, std::move(schema_id), std::move(message)};
}

[[nodiscard]] auto has_space_or_comma(const std::string_view value) -> bool {
  return value.find_first_of(" \t\r\n,") != std::string_view::npos;
}

[[nodiscard]] auto valid_command_name(const std::string_view value) -> bool {
  return !value.empty() && value.front() != '-' && !has_space_or_comma(value);
}

[[nodiscard]] auto valid_option_name(const std::string_view value) -> bool {
  return value.size() >= 2 && value.front() == '-' && value != "--" &&
         !has_space_or_comma(value);
}

[[nodiscard]] auto validate_cardinality(const std::size_t minimum,
                                        const std::size_t maximum) -> bool {
  return maximum != 0 && minimum <= maximum &&
         maximum <= static_cast<std::size_t>(INT_MAX);
}

[[nodiscard]] auto known_value_kind(const ArgumentValueKind kind) -> bool {
  switch (kind) {
    case ArgumentValueKind::flag:
    case ArgumentValueKind::boolean:
    case ArgumentValueKind::signed_integer:
    case ArgumentValueKind::unsigned_integer:
    case ArgumentValueKind::text:
      return true;
  }
  return false;
}

auto validate_command(const CommandSchema& command, const bool root,
                      const std::vector<ControlOptionSchema>& controls,
                      std::unordered_set<std::string>& identities)
    -> std::optional<ParseDiagnostic> {
  if (command.id.empty() || !identities.insert(command.id).second) {
    return diagnostic(ParseDiagnosticCode::invalid_schema,
                      "command and argument IDs must be nonempty and unique",
                      command.id.empty() ? std::nullopt
                                         : std::optional<std::string>{command.id});
  }
  if ((root && !command.name.empty()) ||
      (!root && !valid_command_name(command.name))) {
    return diagnostic(ParseDiagnosticCode::invalid_schema,
                      "the root name must be empty and nested command names must be valid",
                      command.id);
  }
  if (command.subcommand_required && command.subcommands.empty()) {
    return diagnostic(ParseDiagnosticCode::invalid_schema,
                      "a command cannot require a subcommand when none are declared",
                      command.id);
  }
  if (!command.subcommands.empty() &&
      std::ranges::any_of(command.positionals, [](const auto& positional) {
        return positional.minimum_values != 0;
      })) {
    return diagnostic(
        ParseDiagnosticCode::invalid_schema,
        "a command with subcommands cannot also require positional values",
        command.id);
  }

  std::unordered_set<std::string> local_names;
  for (const auto& control : controls) {
    for (const auto& name : control.names) local_names.insert(name);
  }

  for (const auto& option : command.options) {
    if (option.id.empty() || !identities.insert(option.id).second) {
      return diagnostic(ParseDiagnosticCode::invalid_schema,
                        "command and argument IDs must be nonempty and unique",
                        option.id.empty() ? std::nullopt
                                          : std::optional<std::string>{option.id});
    }
    if (!known_value_kind(option.value_kind) || option.names.empty() ||
        !validate_cardinality(option.minimum_occurrences,
                              option.maximum_occurrences)) {
      return diagnostic(ParseDiagnosticCode::invalid_schema,
                        "option names and cardinality must be explicit and valid",
                        option.id);
    }
    for (const auto& name : option.names) {
      if (!valid_option_name(name) || !local_names.insert(name).second) {
        return diagnostic(ParseDiagnosticCode::invalid_schema,
                          "option and control names must be valid and unique within a command",
                          option.id);
      }
    }
  }

  for (std::size_t index = 0; index < command.positionals.size(); ++index) {
    const auto& positional = command.positionals[index];
    if (positional.id.empty() || !identities.insert(positional.id).second) {
      return diagnostic(ParseDiagnosticCode::invalid_schema,
                        "command and argument IDs must be nonempty and unique",
                        positional.id.empty()
                            ? std::nullopt
                            : std::optional<std::string>{positional.id});
    }
    if (!valid_command_name(positional.name) ||
        !known_value_kind(positional.value_kind) ||
        positional.value_kind == ArgumentValueKind::flag ||
        !validate_cardinality(positional.minimum_values,
                              positional.maximum_values)) {
      return diagnostic(ParseDiagnosticCode::invalid_schema,
                        "positional names, value kinds, and cardinality must be valid",
                        positional.id);
    }
    if (index + 1 < command.positionals.size() &&
        (positional.minimum_values != 1 || positional.maximum_values != 1)) {
      return diagnostic(ParseDiagnosticCode::invalid_schema,
                        "only the final positional may be optional or repeated",
                        positional.id);
    }
  }

  std::unordered_set<std::string> subcommand_names;
  for (const auto& subcommand : command.subcommands) {
    if (!subcommand_names.insert(subcommand.name).second) {
      return diagnostic(ParseDiagnosticCode::invalid_schema,
                        "subcommand names must be unique within a command",
                        subcommand.id);
    }
    if (auto result =
            validate_command(subcommand, false, controls, identities)) {
      return result;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto validate_schema(const ParserSchema& schema)
    -> std::optional<ParseDiagnostic> {
  std::unordered_set<ControlRequestKind> control_kinds;
  std::unordered_set<std::string> control_names;
  for (const auto& control : schema.controls) {
    if ((control.kind != ControlRequestKind::help &&
         control.kind != ControlRequestKind::version) ||
        control.names.empty() || !control_kinds.insert(control.kind).second) {
      return diagnostic(ParseDiagnosticCode::invalid_schema,
                        "control kinds must be unique and have at least one name");
    }
    for (const auto& name : control.names) {
      if (!valid_option_name(name) || !control_names.insert(name).second) {
        return diagnostic(ParseDiagnosticCode::invalid_schema,
                          "control names must be valid and unique");
      }
    }
  }

  std::unordered_set<std::string> identities;
  return validate_command(schema.root, true, schema.controls, identities);
}

[[nodiscard]] auto validate_limits(
    const std::span<const std::string_view> arguments, const ParseLimits limits)
    -> std::optional<ParseDiagnostic> {
  if (limits.maximum_argument_count == 0 ||
      limits.maximum_argument_bytes == 0 || limits.maximum_total_bytes == 0) {
    return diagnostic(ParseDiagnosticCode::invalid_limits,
                      "all parser limits must be positive");
  }
  if (arguments.size() > limits.maximum_argument_count) {
    return diagnostic(ParseDiagnosticCode::too_many_arguments,
                      "the argument count exceeds the configured limit");
  }

  std::size_t total{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto size = arguments[index].size();
    if (size > limits.maximum_argument_bytes) {
      return diagnostic(ParseDiagnosticCode::argument_too_large,
                        "an argument exceeds the configured byte limit",
                        std::nullopt, index);
    }
    if (size > std::numeric_limits<std::size_t>::max() - total ||
        total + size > limits.maximum_total_bytes) {
      return diagnostic(ParseDiagnosticCode::arguments_too_large,
                        "the arguments exceed the configured total byte limit",
                        std::nullopt, index);
    }
    total += size;
  }
  return std::nullopt;
}

[[nodiscard]] auto join_names(const std::vector<std::string>& names)
    -> std::string {
  std::string result;
  for (const auto& name : names) {
    if (!result.empty()) result.push_back(',');
    result.append(name);
  }
  return result;
}

auto configure_controls(CLI::App& app,
                        const std::vector<ControlOptionSchema>& controls) -> void {
  app.set_help_flag("");
  app.set_version_flag("");
  for (const auto& control : controls) {
    if (control.kind == ControlRequestKind::help) {
      app.set_help_flag(join_names(control.names))->disable_flag_override();
    } else {
      app.set_version_flag(join_names(control.names), "")
          ->disable_flag_override();
    }
  }
}

auto build_command(const CommandSchema& schema, CLI::App& app,
                   const std::vector<ControlOptionSchema>& controls)
    -> std::unique_ptr<CommandBinding> {
  auto command = std::make_unique<CommandBinding>();
  command->schema = &schema;
  command->app = &app;

  configure_controls(app, controls);
  app.allow_extras(false);
  app.fallthrough(false);
  app.subcommand_fallthrough(false);
  app.validate_positionals();

  for (const auto& option_schema : schema.options) {
    auto binding = std::make_unique<ArgumentBinding>();
    binding->id = option_schema.id;
    binding->value_kind = option_schema.value_kind;
    binding->minimum = option_schema.minimum_occurrences;
    binding->maximum = option_schema.maximum_occurrences;
    const auto names = join_names(option_schema.names);
    if (option_schema.value_kind == ArgumentValueKind::flag) {
      binding->option = app.add_flag(names);
      binding->option->disable_flag_override();
    } else {
      binding->option = app.add_option(names, binding->raw_values);
      binding->option
          ->type_size(1)
          ->allow_extra_args(false)
          ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    }
    if (option_schema.minimum_occurrences != 0) binding->option->required();
    command->options.push_back(std::move(binding));
  }

  for (const auto& positional_schema : schema.positionals) {
    auto binding = std::make_unique<ArgumentBinding>();
    binding->id = positional_schema.id;
    binding->value_kind = positional_schema.value_kind;
    binding->minimum = positional_schema.minimum_values;
    binding->maximum = positional_schema.maximum_values;
    binding->option = app.add_option(positional_schema.name, binding->raw_values);
    binding->option->expected(static_cast<int>(positional_schema.minimum_values),
                              static_cast<int>(positional_schema.maximum_values));
    if (positional_schema.minimum_values != 0) binding->option->required();
    command->positionals.push_back(std::move(binding));
  }

  for (const auto& subcommand_schema : schema.subcommands) {
    auto* subcommand = app.add_subcommand(subcommand_schema.name);
    command->subcommands.push_back(
        build_command(subcommand_schema, *subcommand, controls));
  }
  if (!schema.subcommands.empty()) {
    app.require_subcommand(schema.subcommand_required ? 1 : 0, 1);
  }
  return command;
}

[[nodiscard]] auto active_path(CommandBinding& root)
    -> std::vector<CommandBinding*> {
  std::vector<CommandBinding*> path{&root};
  auto* current = &root;
  while (true) {
    const auto found = std::ranges::find_if(
        current->subcommands,
        [](const auto& child) { return child->app->parsed() != 0; });
    if (found == current->subcommands.end()) break;
    current = found->get();
    path.push_back(current);
  }
  return path;
}

[[nodiscard]] auto command_path(CommandBinding& root)
    -> std::vector<std::string> {
  auto path = active_path(root);
  std::vector<std::string> result;
  result.reserve(path.size());
  for (const auto* command : path) result.push_back(command->schema->id);
  return result;
}

[[nodiscard]] auto lowercase_ascii(std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::transform(result, result.begin(), [](const unsigned char character) {
    if (character >= 'A' && character <= 'Z') {
      return static_cast<char>(character - 'A' + 'a');
    }
    return static_cast<char>(character);
  });
  return result;
}

[[nodiscard]] auto convert_value(const ArgumentBinding& binding,
                                 const std::string_view raw)
    -> std::optional<ParsedValue> {
  switch (binding.value_kind) {
    case ArgumentValueKind::flag:
      return ParsedValue{true};
    case ArgumentValueKind::boolean: {
      const auto normalized = lowercase_ascii(raw);
      if (normalized == "true" || normalized == "1" || normalized == "on" ||
          normalized == "yes") {
        return ParsedValue{true};
      }
      if (normalized == "false" || normalized == "0" || normalized == "off" ||
          normalized == "no") {
        return ParsedValue{false};
      }
      return std::nullopt;
    }
    case ArgumentValueKind::signed_integer: {
      std::int64_t value{};
      const auto [end, error] =
          std::from_chars(raw.data(), raw.data() + raw.size(), value, 10);
      if (error != std::errc{} || end != raw.data() + raw.size()) {
        return std::nullopt;
      }
      return ParsedValue{value};
    }
    case ArgumentValueKind::unsigned_integer: {
      std::uint64_t value{};
      const auto [end, error] =
          std::from_chars(raw.data(), raw.data() + raw.size(), value, 10);
      if (error != std::errc{} || end != raw.data() + raw.size()) {
        return std::nullopt;
      }
      return ParsedValue{value};
    }
    case ArgumentValueKind::text:
      return ParsedValue{std::string{raw}};
  }
  return std::nullopt;
}

[[nodiscard]] auto append_arguments(CommandBinding& command,
                                    std::vector<ParsedArgument>& arguments)
    -> std::optional<ParseDiagnostic> {
  const auto append = [&](const ArgumentBinding& binding)
      -> std::optional<ParseDiagnostic> {
    const auto count = binding.value_kind == ArgumentValueKind::flag
                           ? binding.option->count()
                           : binding.raw_values.size();
    if (count < binding.minimum || count > binding.maximum) {
      return diagnostic(ParseDiagnosticCode::cardinality_violation,
                        "an argument occurrence count violates its schema",
                        binding.id);
    }
    if (count == 0) return std::nullopt;

    ParsedArgument parsed{binding.id, {}};
    parsed.values.reserve(count);
    if (binding.value_kind == ArgumentValueKind::flag) {
      for (std::size_t index = 0; index < count; ++index) {
        parsed.values.emplace_back(true);
      }
    } else {
      for (const auto& raw : binding.raw_values) {
        auto value = convert_value(binding, raw);
        if (!value) {
          return diagnostic(ParseDiagnosticCode::invalid_value,
                            "an argument value does not match its declared type",
                            binding.id);
        }
        parsed.values.push_back(std::move(*value));
      }
    }
    arguments.push_back(std::move(parsed));
    return std::nullopt;
  };

  for (const auto& binding : command.options) {
    if (auto result = append(*binding)) return result;
  }
  for (const auto& binding : command.positionals) {
    if (auto result = append(*binding)) return result;
  }
  return std::nullopt;
}

[[nodiscard]] auto invocation(CommandBinding& root)
    -> std::expected<ParseOutcome, ParseDiagnostic> {
  const auto path = active_path(root);
  ParsedInvocation result;
  result.command_path.reserve(path.size());
  for (auto* command : path) {
    result.command_path.push_back(command->schema->id);
    if (auto error = append_arguments(*command, result.arguments)) {
      return std::unexpected(std::move(*error));
    }
  }
  return ParseOutcome{std::move(result)};
}

[[nodiscard]] auto first_token_index(
    const std::span<const std::string_view> arguments,
    const std::string_view token) -> std::optional<std::size_t> {
  const auto found = std::ranges::find(arguments, token);
  if (found == arguments.end()) return std::nullopt;
  return static_cast<std::size_t>(found - arguments.begin());
}

[[nodiscard]] auto option_name(const std::string_view token) -> std::string_view {
  const auto separator = token.find('=');
  return token.substr(0, separator);
}

[[nodiscard]] auto is_after_delimiter(
    const std::span<const std::string_view> arguments, const std::size_t index)
    -> bool {
  const auto delimiter = std::ranges::find(arguments.first(index), "--");
  return delimiter != arguments.begin() + static_cast<std::ptrdiff_t>(index);
}

[[nodiscard]] auto classify_extra(
    CommandBinding& root, const std::vector<ControlOptionSchema>& controls,
    const std::span<const std::string_view> arguments) -> ParseDiagnostic {
  auto path = active_path(root);
  auto* command = path.back();
  const auto remaining = root.app->remaining(true);
  if (remaining.empty()) {
    return diagnostic(ParseDiagnosticCode::unexpected_argument,
                      "the command line contains an unexpected argument");
  }

  const auto& token = remaining.front();
  const auto index = first_token_index(arguments, token);
  const bool after_delimiter = index && is_after_delimiter(arguments, *index);
  if (!after_delimiter && token.starts_with('-')) {
    const auto name = option_name(token);
    std::size_t prefix_matches{};
    bool exact{};
    const auto inspect = [&](const std::string& candidate) {
      if (candidate == name) exact = true;
      if (name.starts_with("--") && candidate.starts_with(name) &&
          candidate.size() > name.size()) {
        ++prefix_matches;
      }
    };
    for (const auto& option : command->schema->options) {
      for (const auto& candidate : option.names) inspect(candidate);
    }
    for (const auto& control : controls) {
      for (const auto& candidate : control.names) inspect(candidate);
    }
    if (exact) {
      return diagnostic(ParseDiagnosticCode::cardinality_violation,
                        "an option was supplied in a disallowed form or count",
                        std::nullopt, index);
    }
    if (prefix_matches > 1) {
      return diagnostic(ParseDiagnosticCode::ambiguous_option,
                        "an option prefix matches more than one declared option",
                        std::nullopt, index);
    }
    return diagnostic(ParseDiagnosticCode::unknown_option,
                      "the command line contains an unknown option", std::nullopt,
                      index);
  }

  const bool has_selected_child = std::ranges::any_of(
      command->subcommands,
      [](const auto& child) { return child->app->parsed() != 0; });
  if (!has_selected_child && !command->subcommands.empty()) {
    std::size_t prefix_matches{};
    bool exact{};
    for (const auto& child : command->subcommands) {
      const auto& candidate = child->schema->name;
      if (candidate == token) exact = true;
      if (candidate.starts_with(token) && candidate.size() > token.size()) {
        ++prefix_matches;
      }
    }
    if (exact) {
      return diagnostic(ParseDiagnosticCode::cardinality_violation,
                        "more than one command was supplied at the same level",
                        command->schema->id, index);
    }
    if (prefix_matches > 1) {
      return diagnostic(ParseDiagnosticCode::ambiguous_command,
                        "a command prefix matches more than one declared command",
                        command->schema->id, index);
    }
    return diagnostic(ParseDiagnosticCode::unknown_command,
                      "the command line contains an unknown command",
                      command->schema->id, index);
  }
  return diagnostic(ParseDiagnosticCode::unexpected_argument,
                    "the command line contains an unexpected argument",
                    command->schema->id, index);
}

[[nodiscard]] auto missing_required(CommandBinding& root) -> ParseDiagnostic {
  auto path = active_path(root);
  auto* command = path.back();
  const bool has_selected_child = std::ranges::any_of(
      command->subcommands,
      [](const auto& child) { return child->app->parsed() != 0; });
  if (command->schema->subcommand_required && !has_selected_child) {
    return diagnostic(ParseDiagnosticCode::missing_command,
                      "the selected command requires a subcommand",
                      command->schema->id);
  }

  for (auto* active : path) {
    for (const auto& binding : active->options) {
      const auto count = binding->value_kind == ArgumentValueKind::flag
                             ? binding->option->count()
                             : binding->raw_values.size();
      if (count < binding->minimum) {
        return diagnostic(ParseDiagnosticCode::missing_required_argument,
                          "a required argument was not supplied", binding->id);
      }
    }
    for (const auto& binding : active->positionals) {
      if (binding->raw_values.size() < binding->minimum) {
        return diagnostic(ParseDiagnosticCode::missing_required_argument,
                          "a required argument was not supplied", binding->id);
      }
    }
  }
  return diagnostic(ParseDiagnosticCode::missing_required_argument,
                    "a required argument was not supplied");
}

[[nodiscard]] auto find_local_option(const CommandSchema& command,
                                     const std::string_view name)
    -> const OptionSchema* {
  for (const auto& option : command.options) {
    if (std::ranges::contains(option.names, name)) return &option;
  }
  return nullptr;
}

[[nodiscard]] auto find_subcommand(const CommandSchema& command,
                                   const std::string_view name)
    -> const CommandSchema* {
  for (const auto& child : command.subcommands) {
    if (child.name == name) return &child;
  }
  return nullptr;
}

[[nodiscard]] auto is_control_name(
    const std::vector<ControlOptionSchema>& controls,
    const std::string_view name) -> bool {
  return std::ranges::any_of(controls, [&](const auto& control) {
    return std::ranges::contains(control.names, name);
  });
}

[[nodiscard]] auto prepare_cli_arguments(
    const ParserSchema& schema,
    const std::span<const std::string_view> arguments)
    -> std::expected<std::vector<std::string>, ParseDiagnostic> {
  std::vector<std::string> result;
  result.reserve(arguments.size() + 1);
  const auto* command = &schema.root;
  bool positional_only{};

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto token = arguments[index];
    if (positional_only) {
      result.emplace_back(token);
      continue;
    }
    if (token == "--") {
      positional_only = true;
      result.emplace_back(token);
      continue;
    }
    if (const auto* child = find_subcommand(*command, token)) {
      command = child;
      result.emplace_back(token);
      continue;
    }

    const auto separator = token.find('=');
    const auto name = option_name(token);
    const auto* option = find_local_option(*command, name);
    if (option == nullptr || option->value_kind == ArgumentValueKind::flag) {
      result.emplace_back(token);
      continue;
    }
    if (separator != std::string_view::npos) {
      if (separator + 1 == token.size()) {
        result.emplace_back(token.substr(0, separator));
        result.emplace_back();
      } else {
        result.emplace_back(token);
      }
      continue;
    }

    const bool missing = index + 1 == arguments.size() ||
                         arguments[index + 1] == "--" ||
                         find_local_option(
                             *command, option_name(arguments[index + 1])) != nullptr ||
                         is_control_name(schema.controls,
                                         option_name(arguments[index + 1]));
    if (missing) {
      return std::unexpected(diagnostic(ParseDiagnosticCode::missing_value,
                                        "an option requires a value", option->id,
                                        index));
    }
    result.emplace_back(token);
    result.emplace_back(arguments[index + 1]);
    ++index;
  }
  return result;
}

}  // namespace

auto ArgumentParser::parse(const ParserSchema& schema,
                           const std::span<const std::string_view> arguments,
                           const ParseLimits limits) const
    -> std::expected<ParseOutcome, ParseDiagnostic> {
  if (auto error = validate_limits(arguments, limits)) {
    return std::unexpected(std::move(*error));
  }
  if (auto error = validate_schema(schema)) {
    return std::unexpected(std::move(*error));
  }
  auto prepared_arguments = prepare_cli_arguments(schema, arguments);
  if (!prepared_arguments) {
    return std::unexpected(std::move(prepared_arguments.error()));
  }

  CLI::App app;
  std::unique_ptr<CommandBinding> root;
  try {
    root = build_command(schema.root, app, schema.controls);
    auto cli_arguments = std::move(*prepared_arguments);
    std::ranges::reverse(cli_arguments);
    app.parse(cli_arguments);
    return invocation(*root);
  } catch (const CLI::CallForHelp&) {
    return ParseOutcome{ControlRequest{ControlRequestKind::help,
                                       root ? command_path(*root)
                                            : std::vector<std::string>{}}};
  } catch (const CLI::CallForAllHelp&) {
    return ParseOutcome{ControlRequest{ControlRequestKind::help,
                                       root ? command_path(*root)
                                            : std::vector<std::string>{}}};
  } catch (const CLI::CallForVersion&) {
    return ParseOutcome{ControlRequest{ControlRequestKind::version,
                                       root ? command_path(*root)
                                            : std::vector<std::string>{}}};
  } catch (const CLI::ConstructionError&) {
    return std::unexpected(diagnostic(
        ParseDiagnosticCode::invalid_schema,
        "the parser rejected the declarative command schema"));
  } catch (const CLI::RequiredError&) {
    return std::unexpected(root ? missing_required(*root)
                                : diagnostic(ParseDiagnosticCode::adapter_failure,
                                             "the parser adapter failed safely"));
  } catch (const CLI::ArgumentMismatch&) {
    return std::unexpected(diagnostic(
        ParseDiagnosticCode::cardinality_violation,
        "an argument value count violates its declared cardinality"));
  } catch (const CLI::ConversionError&) {
    return std::unexpected(diagnostic(
        ParseDiagnosticCode::invalid_value,
        "an argument value does not match its declared type"));
  } catch (const CLI::ValidationError&) {
    return std::unexpected(diagnostic(
        ParseDiagnosticCode::invalid_value,
        "an argument value does not match its declared constraints"));
  } catch (const CLI::ExtrasError&) {
    return std::unexpected(root
                               ? classify_extra(*root, schema.controls, arguments)
                               : diagnostic(ParseDiagnosticCode::adapter_failure,
                                            "the parser adapter failed safely"));
  } catch (const CLI::InvalidError&) {
    return std::unexpected(diagnostic(
        ParseDiagnosticCode::invalid_schema,
        "the parser rejected the declarative command schema"));
  } catch (const CLI::ParseError&) {
    return std::unexpected(diagnostic(ParseDiagnosticCode::adapter_failure,
                                      "the parser adapter failed safely"));
  } catch (const std::exception&) {
    return std::unexpected(diagnostic(ParseDiagnosticCode::adapter_failure,
                                      "the parser adapter failed safely"));
  } catch (...) {
    return std::unexpected(diagnostic(ParseDiagnosticCode::adapter_failure,
                                      "the parser adapter failed safely"));
  }
}

}  // namespace aiforge::cli
