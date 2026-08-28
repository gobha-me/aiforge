#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <aiforge/cli/parser.hpp>

namespace {

using namespace aiforge::cli;

[[nodiscard]] auto limits() -> ParseLimits {
  return {64, 1024, 4096};
}

[[nodiscard]] auto controls() -> std::vector<ControlOptionSchema> {
  return {{ControlRequestKind::help, {"-h", "--help"}},
          {ControlRequestKind::version, {"--version"}}};
}

[[nodiscard]] auto option(std::string id, std::vector<std::string> names,
                          const ArgumentValueKind kind,
                          const std::size_t minimum = 0,
                          const std::size_t maximum = 1) -> OptionSchema {
  return {std::move(id), std::move(names), kind, minimum, maximum};
}

[[nodiscard]] auto positional(std::string id, std::string name,
                              const ArgumentValueKind kind,
                              const std::size_t minimum = 1,
                              const std::size_t maximum = 1)
    -> PositionalSchema {
  return {std::move(id), std::move(name), kind, minimum, maximum};
}

[[nodiscard]] auto base_schema() -> ParserSchema {
  ParserSchema schema;
  schema.root.id = "root";
  schema.controls = controls();
  return schema;
}

[[nodiscard]] auto command(std::string id, std::string name,
                           const bool subcommand_required = false)
    -> CommandSchema {
  CommandSchema result;
  result.id = std::move(id);
  result.name = std::move(name);
  result.subcommand_required = subcommand_required;
  return result;
}

[[nodiscard]] auto parse(const ParserSchema& schema,
                         std::vector<std::string_view> arguments,
                         const ParseLimits parse_limits = limits()) {
  return ArgumentParser{}.parse(schema, arguments, parse_limits);
}

[[nodiscard]] auto code(const ParserSchema& schema,
                        std::vector<std::string_view> arguments,
                        const ParseLimits parse_limits = limits())
    -> ParseDiagnosticCode {
  const auto result = parse(schema, std::move(arguments), parse_limits);
  REQUIRE_FALSE(result);
  return result.error().code;
}

[[nodiscard]] auto invocation(const ParseOutcome& outcome)
    -> const ParsedInvocation& {
  const auto* result = std::get_if<ParsedInvocation>(&outcome);
  REQUIRE(result != nullptr);
  return *result;
}

[[nodiscard]] auto argument(const ParsedInvocation& parsed,
                            const std::string_view id)
    -> const ParsedArgument& {
  const auto found =
      std::ranges::find(parsed.arguments, id, &ParsedArgument::id);
  REQUIRE(found != parsed.arguments.end());
  return *found;
}

class StreamCapture final {
 public:
  StreamCapture()
      : m_old_out(std::cout.rdbuf(m_out.rdbuf())),
        m_old_error(std::cerr.rdbuf(m_error.rdbuf())) {}

  StreamCapture(const StreamCapture&) = delete;
  auto operator=(const StreamCapture&) -> StreamCapture& = delete;

  ~StreamCapture() {
    std::cout.rdbuf(m_old_out);
    std::cerr.rdbuf(m_old_error);
  }

  [[nodiscard]] auto output() const -> std::string { return m_out.str(); }
  [[nodiscard]] auto error() const -> std::string { return m_error.str(); }

 private:
  std::ostringstream m_out;
  std::ostringstream m_error;
  std::streambuf* m_old_out;
  std::streambuf* m_old_error;
};

} // namespace

TEST_CASE("parser rejects invalid limits before schema construction",
          "[cli][failure]") {
  const auto schema = base_schema();
  REQUIRE(code(schema, {}, {0, 1, 1}) == ParseDiagnosticCode::invalid_limits);
  REQUIRE(code(schema, {"one", "two"}, {1, 16, 32}) ==
          ParseDiagnosticCode::too_many_arguments);
  REQUIRE(code(schema, {"oversized"}, {2, 4, 32}) ==
          ParseDiagnosticCode::argument_too_large);
  REQUIRE(code(schema, {"four", "five"}, {2, 8, 5}) ==
          ParseDiagnosticCode::arguments_too_large);
}

TEST_CASE("malformed schemas fail without leaking construction exceptions",
          "[cli][failure]") {
  auto schema = base_schema();
  schema.root.name = "not-root";
  REQUIRE(code(schema, {}) == ParseDiagnosticCode::invalid_schema);

  schema = base_schema();
  schema.root.options = {option("first", {"--same"}, ArgumentValueKind::text),
                         option("second", {"--same"}, ArgumentValueKind::text)};
  REQUIRE(code(schema, {}) == ParseDiagnosticCode::invalid_schema);

  schema = base_schema();
  schema.root.options = {
      option("bad-count", {"--count"}, ArgumentValueKind::text, 2, 1)};
  REQUIRE(code(schema, {}) == ParseDiagnosticCode::invalid_schema);

  schema = base_schema();
  schema.root.positionals = {
      positional("many", "many", ArgumentValueKind::text, 0, 3),
      positional("last", "last", ArgumentValueKind::text)};
  REQUIRE(code(schema, {}) == ParseDiagnosticCode::invalid_schema);

  schema = base_schema();
  schema.root.options = {
      option("cli11-construction", {"-invalid"}, ArgumentValueKind::text)};
  REQUIRE(code(schema, {}) == ParseDiagnosticCode::invalid_schema);
}

TEST_CASE("empty arguments select the default command", "[cli]") {
  auto schema = base_schema();
  schema.root.positionals = {
      positional("prompt", "prompt", ArgumentValueKind::text, 0, 1)};
  auto result = parse(schema, {});
  REQUIRE(result);
  auto& parsed = invocation(*result);
  REQUIRE(parsed.command_path == std::vector<std::string>{"root"});
  REQUIRE(parsed.arguments.empty());

  result = parse(schema, {"hello"});
  REQUIRE(result);
  REQUIRE(argument(invocation(*result), "prompt").values ==
          std::vector<ParsedValue>{std::string{"hello"}});
}

TEST_CASE("typed options preserve explicit default-like and repeated values",
          "[cli]") {
  auto schema = base_schema();
  schema.root.options = {
      option("count", {"-c", "--count"}, ArgumentValueKind::signed_integer),
      option("enabled", {"--enabled"}, ArgumentValueKind::boolean),
      option("label", {"--label"}, ArgumentValueKind::text),
      option("tag", {"--tag"}, ArgumentValueKind::text, 0, 3),
      option("verbose", {"-v", "--verbose"}, ArgumentValueKind::flag)};

  const auto repeated = parse(schema, {"--tag", "one", "--tag", "two"});
  INFO("repeated diagnostic code: "
       << (repeated ? -1 : static_cast<int>(repeated.error().code)));
  REQUIRE(repeated);

  const auto result =
      parse(schema, {"--count", "0", "--enabled", "false", "--label=", "--tag",
                     "one", "--tag", "two", "-v"});
  INFO("diagnostic code: " << (result ? -1
                                      : static_cast<int>(result.error().code)));
  REQUIRE(result);
  const auto& parsed = invocation(*result);
  REQUIRE(argument(parsed, "count").values ==
          std::vector<ParsedValue>{std::int64_t{0}});
  REQUIRE(argument(parsed, "enabled").values ==
          std::vector<ParsedValue>{false});
  REQUIRE(argument(parsed, "label").values ==
          std::vector<ParsedValue>{std::string{}});
  REQUIRE(argument(parsed, "tag").values ==
          std::vector<ParsedValue>{std::string{"one"}, std::string{"two"}});
  REQUIRE(argument(parsed, "verbose").values == std::vector<ParsedValue>{true});
}

TEST_CASE("named and nested commands return stable command paths", "[cli]") {
  auto schema = base_schema();
  auto chat = command("chat", "chat");
  chat.positionals = {positional("prompt", "prompt", ArgumentValueKind::text)};
  auto show = command("config-show", "show");
  show.options = {
      option("limit", {"--limit"}, ArgumentValueKind::unsigned_integer)};
  auto config = command("config", "config", true);
  config.subcommands.push_back(show);
  schema.root.subcommands = {chat, config};

  auto result = parse(schema, {"config", "show", "--limit", "0"});
  REQUIRE(result);
  REQUIRE(invocation(*result).command_path ==
          std::vector<std::string>{"root", "config", "config-show"});
  REQUIRE(argument(invocation(*result), "limit").values ==
          std::vector<ParsedValue>{std::uint64_t{0}});

  result = parse(schema, {"chat", "--", "--literal-prompt"});
  REQUIRE(result);
  REQUIRE(argument(invocation(*result), "prompt").values ==
          std::vector<ParsedValue>{std::string{"--literal-prompt"}});
}

TEST_CASE("repeatable options cannot greedily consume a positional", "[cli]") {
  auto schema = base_schema();
  auto chat = command("chat", "chat");
  chat.options = {option("tag", {"--tag"}, ArgumentValueKind::text, 0, 4)};
  chat.positionals = {positional("prompt", "prompt", ArgumentValueKind::text)};
  schema.root.subcommands.push_back(chat);

  const auto result =
      parse(schema, {"chat", "--tag", "one", "--tag", "two", "prompt"});
  REQUIRE(result);
  const auto& parsed = invocation(*result);
  REQUIRE(argument(parsed, "tag").values ==
          std::vector<ParsedValue>{std::string{"one"}, std::string{"two"}});
  REQUIRE(argument(parsed, "prompt").values ==
          std::vector<ParsedValue>{std::string{"prompt"}});
}

TEST_CASE("option aliases remain scoped to the selected command", "[cli]") {
  auto schema = base_schema();
  schema.root.options = {
      option("root-mode", {"--mode"}, ArgumentValueKind::text)};
  auto child = command("child", "child");
  child.options = {option("child-mode", {"--mode"}, ArgumentValueKind::flag)};
  schema.root.subcommands.push_back(child);

  auto result = parse(schema, {"--mode="});
  REQUIRE(result);
  REQUIRE(argument(invocation(*result), "root-mode").values ==
          std::vector<ParsedValue>{std::string{}});

  result = parse(schema, {"child", "--mode"});
  REQUIRE(result);
  REQUIRE(argument(invocation(*result), "child-mode").values ==
          std::vector<ParsedValue>{true});
}

TEST_CASE("unknown and ambiguous commands and options are typed failures",
          "[cli][failure]") {
  auto schema = base_schema();
  schema.root.options = {
      option("verbose", {"--verbose"}, ArgumentValueKind::flag)};
  schema.root.subcommands = {command("config", "config"),
                             command("configure", "configure")};

  REQUIRE(code(schema, {"missing"}) == ParseDiagnosticCode::unknown_command);
  REQUIRE(code(schema, {"conf"}) == ParseDiagnosticCode::ambiguous_command);
  REQUIRE(code(schema, {"--missing"}) == ParseDiagnosticCode::unknown_option);
  REQUIRE(code(schema, {"--ver"}) == ParseDiagnosticCode::ambiguous_option);
  REQUIRE(code(schema, {"configu"}) == ParseDiagnosticCode::unknown_command);
}

TEST_CASE("missing invalid and excessive values retain distinct failures",
          "[cli][failure]") {
  auto schema = base_schema();
  schema.root.options = {
      option("count", {"--count"}, ArgumentValueKind::signed_integer),
      option("required", {"--required"}, ArgumentValueKind::text, 1, 1),
      option("tag", {"--tag"}, ArgumentValueKind::text, 0, 2)};

  REQUIRE(code(schema, {"--count"}) == ParseDiagnosticCode::missing_value);
  REQUIRE(code(schema, {"--count", "--required", "yes"}) ==
          ParseDiagnosticCode::missing_value);
  REQUIRE(code(schema, {"--count", "not-a-number", "--required", "yes"}) ==
          ParseDiagnosticCode::invalid_value);
  const auto secret =
      parse(schema, {"--count", "secret-value", "--required", "yes"});
  REQUIRE_FALSE(secret);
  REQUIRE(secret.error().message.find("secret-value") == std::string::npos);
  REQUIRE(code(schema, {"--count", "999999999999999999999999", "--required",
                        "yes"}) == ParseDiagnosticCode::invalid_value);
  REQUIRE(code(schema, {}) == ParseDiagnosticCode::missing_required_argument);
  REQUIRE(code(schema, {"--required", "yes", "--tag", "one", "--tag", "two",
                        "--tag", "three"}) ==
          ParseDiagnosticCode::cardinality_violation);
}

TEST_CASE("required nested commands fail explicitly", "[cli][failure]") {
  auto schema = base_schema();
  auto config = command("config", "config", true);
  config.subcommands.push_back(command("config-show", "show"));
  schema.root.subcommands.push_back(config);
  REQUIRE(code(schema, {"config"}) == ParseDiagnosticCode::missing_command);
}

TEST_CASE("help and version are silent successful control requests", "[cli]") {
  auto schema = base_schema();
  auto config = command("config", "config");
  schema.root.subcommands.push_back(config);

  std::expected<ParseOutcome, ParseDiagnostic> help;
  std::expected<ParseOutcome, ParseDiagnostic> version;
  std::expected<ParseOutcome, ParseDiagnostic> failure;
  std::string output;
  std::string error;
  {
    StreamCapture capture;
    help = parse(schema, {"config", "--help"});
    version = parse(schema, {"--version"});
    failure = parse(schema, {"--unknown"});
    output = capture.output();
    error = capture.error();
  }

  REQUIRE(help);
  REQUIRE(version);
  REQUIRE_FALSE(failure);
  REQUIRE(output.empty());
  REQUIRE(error.empty());
  REQUIRE(std::get<ControlRequest>(*help) ==
          ControlRequest{ControlRequestKind::help, {"root", "config"}});
  REQUIRE(std::get<ControlRequest>(*version) ==
          ControlRequest{ControlRequestKind::version, {"root"}});
}
