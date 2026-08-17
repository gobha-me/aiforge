#pragma once

#include <cstddef>
#include <expected>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/cli/parser.hpp>

namespace aiforge::cli {

struct CommandContext {
  const ParsedInvocation& invocation;
  std::ostream& output;
  std::ostream& error;
};

using CommandHandler = auto (*)(CommandContext&) -> int;

struct CommandOptionSpec {
  OptionSchema parser;
  std::string value_name;
  std::string help;

  auto operator==(const CommandOptionSpec&) const -> bool = default;
};

struct CommandPositionalSpec {
  PositionalSchema parser;
  std::string help;

  auto operator==(const CommandPositionalSpec&) const -> bool = default;
};

struct CommandSpec {
  std::string id;
  // The root command has an empty name. Every nested command has one token name.
  std::string name;
  std::string help;
  bool subcommand_required{};
  std::vector<CommandOptionSpec> options;
  std::vector<CommandPositionalSpec> positionals;
  std::vector<CommandSpec> subcommands;
  CommandHandler handler{};
};

struct CommandRegistry {
  std::string program_name;
  std::string version;
  CommandSpec root;
  std::vector<ControlOptionSchema> controls;
};

enum class RegistryDiagnosticCode {
  invalid_program,
  invalid_command,
  duplicate_command_id,
  duplicate_command_name,
  missing_handler,
  unknown_command_path,
};

struct RegistryDiagnostic {
  RegistryDiagnosticCode code{RegistryDiagnosticCode::invalid_command};
  std::string message;
  std::string command_id;

  auto operator==(const RegistryDiagnostic&) const -> bool = default;
};

[[nodiscard]] auto make_parser_schema(const CommandRegistry& registry)
    -> std::expected<ParserSchema, RegistryDiagnostic>;

[[nodiscard]] auto render_help(const CommandRegistry& registry,
                               std::span<const std::string> command_path)
    -> std::expected<std::string, RegistryDiagnostic>;

class CommandDispatcher final {
 public:
  [[nodiscard]] auto dispatch(
      const CommandRegistry& registry,
      std::span<const std::string_view> arguments, std::ostream& output,
      std::ostream& error,
      ParseLimits limits = {256, 64U * 1024U, 1024U * 1024U}) const noexcept
      -> int;
};

[[nodiscard]] auto builtin_command_registry() -> const CommandRegistry&;

[[nodiscard]] auto run_cli(std::span<const std::string_view> arguments,
                           std::ostream& output,
                           std::ostream& error) noexcept -> int;

}  // namespace aiforge::cli
