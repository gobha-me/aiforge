#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aiforge::cli {

enum class ArgumentValueKind {
  flag,
  // Boolean values accept true/false, 1/0, on/off, and yes/no without case.
  boolean,
  // Integer values are complete base-10 strings in the corresponding 64-bit range.
  signed_integer,
  unsigned_integer,
  // Text values are preserved exactly, including an explicitly supplied empty value.
  text,
};

struct OptionSchema {
  std::string id;
  std::vector<std::string> names;
  ArgumentValueKind value_kind{ArgumentValueKind::text};
  std::size_t minimum_occurrences{};
  std::size_t maximum_occurrences{1};

  auto operator==(const OptionSchema&) const -> bool = default;
};

struct PositionalSchema {
  std::string id;
  std::string name;
  ArgumentValueKind value_kind{ArgumentValueKind::text};
  std::size_t minimum_values{};
  std::size_t maximum_values{1};

  auto operator==(const PositionalSchema&) const -> bool = default;
};

enum class ControlRequestKind {
  help,
  version,
};

struct ControlOptionSchema {
  ControlRequestKind kind{ControlRequestKind::help};
  std::vector<std::string> names;

  auto operator==(const ControlOptionSchema&) const -> bool = default;
};

struct CommandSchema {
  std::string id;
  // The root command has an empty name. Every nested command has one token name.
  std::string name;
  bool subcommand_required{};
  std::vector<OptionSchema> options;
  std::vector<PositionalSchema> positionals;
  std::vector<CommandSchema> subcommands;

  auto operator==(const CommandSchema&) const -> bool = default;
};

struct ParserSchema {
  CommandSchema root;
  // Controls are recognized at every command level and never render or dispatch.
  std::vector<ControlOptionSchema> controls;

  auto operator==(const ParserSchema&) const -> bool = default;
};

struct ParseLimits {
  std::size_t maximum_argument_count{};
  std::size_t maximum_argument_bytes{};
  std::size_t maximum_total_bytes{};

  auto operator==(const ParseLimits&) const -> bool = default;
};

using ParsedValue =
    std::variant<bool, std::int64_t, std::uint64_t, std::string>;

struct ParsedArgument {
  std::string id;
  // Repeated values retain command-line order. An absent argument has no entry.
  std::vector<ParsedValue> values;

  auto operator==(const ParsedArgument&) const -> bool = default;
};

struct ParsedInvocation {
  // Stable command IDs from the root through the selected command.
  std::vector<std::string> command_path;
  std::vector<ParsedArgument> arguments;

  auto operator==(const ParsedInvocation&) const -> bool = default;
};

struct ControlRequest {
  ControlRequestKind kind{ControlRequestKind::help};
  std::vector<std::string> command_path;

  auto operator==(const ControlRequest&) const -> bool = default;
};

using ParseOutcome = std::variant<ParsedInvocation, ControlRequest>;

enum class ParseDiagnosticCode {
  invalid_limits,
  too_many_arguments,
  argument_too_large,
  arguments_too_large,
  invalid_schema,
  unknown_command,
  ambiguous_command,
  unknown_option,
  ambiguous_option,
  missing_command,
  missing_value,
  invalid_value,
  missing_required_argument,
  unexpected_argument,
  cardinality_violation,
  adapter_failure,
};

struct ParseDiagnostic {
  ParseDiagnosticCode code{ParseDiagnosticCode::adapter_failure};
  std::optional<std::size_t> token_index;
  std::optional<std::string> schema_id;
  // Messages are adapter-authored and never include raw argument values.
  std::string message;

  auto operator==(const ParseDiagnostic&) const -> bool = default;
};

class ArgumentParser final {
 public:
  // Arguments exclude argv[0]. Parsing is synchronous and retains no views.
  [[nodiscard]] auto parse(const ParserSchema& schema,
                           std::span<const std::string_view> arguments,
                           ParseLimits limits) const
      -> std::expected<ParseOutcome, ParseDiagnostic>;
};

}  // namespace aiforge::cli
