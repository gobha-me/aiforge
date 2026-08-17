#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/cli/command_registry.hpp>

namespace {

using namespace aiforge::cli;

auto success_handler(CommandContext& context) -> int {
  context.output << "handled:" << context.invocation.command_path.back() << '\n';
  return 7;
}

auto throwing_handler(CommandContext&) -> int {
  throw std::runtime_error{"sensitive handler failure"};
}

auto typed_handler(CommandContext& context) -> int {
  const auto& value = context.invocation.arguments.front().values.front();
  context.output << "count:" << std::get<std::int64_t>(value) << '\n';
  return 0;
}

[[nodiscard]] auto root_command(CommandHandler handler = success_handler)
    -> CommandSpec {
  return {"root", "", "Test command registry.", false, {}, {}, {}, handler};
}

[[nodiscard]] auto registry_with(CommandSpec root) -> CommandRegistry {
  return {"forge-test",
          "1.2.3",
          std::move(root),
          {{ControlRequestKind::help, {"-h", "--help"}},
           {ControlRequestKind::version, {"--version"}}}};
}

[[nodiscard]] auto dispatch(const CommandRegistry& registry,
                            std::vector<std::string_view> arguments,
                            std::string& output, std::string& error) -> int {
  std::ostringstream output_stream;
  std::ostringstream error_stream;
  const auto result = CommandDispatcher{}.dispatch(
      registry, arguments, output_stream, error_stream, {32, 256, 1024});
  output = output_stream.str();
  error = error_stream.str();
  return result;
}

}  // namespace

TEST_CASE("invalid command registries fail before parser construction",
          "[commands][failure]") {
  auto root = root_command();
  root.handler = nullptr;
  auto result = make_parser_schema(registry_with(root));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == RegistryDiagnosticCode::missing_handler);

  root = root_command();
  root.subcommands = {
      {"duplicate", "one", "One.", false, {}, {}, {}, success_handler},
      {"duplicate", "two", "Two.", false, {}, {}, {}, success_handler}};
  result = make_parser_schema(registry_with(root));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          RegistryDiagnosticCode::duplicate_command_id);

  root = root_command();
  root.subcommands = {
      {"one", "same", "One.", false, {}, {}, {}, success_handler},
      {"two", "same", "Two.", false, {}, {}, {}, success_handler}};
  result = make_parser_schema(registry_with(root));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          RegistryDiagnosticCode::duplicate_command_name);
}

TEST_CASE("one command registration drives schema help and dispatch",
          "[commands]") {
  auto root = root_command();
  root.subcommands.push_back({
      "toy",
      "toy",
      "Run the toy command.",
      false,
      {{{"toy.count", {"--count"}, ArgumentValueKind::signed_integer, 1, 1},
        "number",
        "Number of runs."}},
      {},
      {},
      typed_handler,
  });
  const auto registry = registry_with(std::move(root));

  const auto schema = make_parser_schema(registry);
  REQUIRE(schema);
  REQUIRE(schema->root.subcommands.size() == 1);
  REQUIRE(schema->root.subcommands.front().id == "toy");

  const auto help = render_help(registry, std::vector<std::string>{"root"});
  REQUIRE(help);
  REQUIRE(help->find("toy") != std::string::npos);
  REQUIRE(help->find("Run the toy command.") != std::string::npos);

  std::string output;
  std::string error;
  REQUIRE(dispatch(registry, {"toy", "--count", "0"}, output, error) == 0);
  REQUIRE(output == "count:0\n");
  REQUIRE(error.empty());

  const auto toy_help =
      render_help(registry, std::vector<std::string>{"root", "toy"});
  REQUIRE(toy_help);
  REQUIRE(toy_help->find("--count <number>") != std::string::npos);
  REQUIRE(toy_help->find("Number of runs.") != std::string::npos);
}

TEST_CASE("command-line failures use stderr and usage exit code",
          "[commands][failure]") {
  auto root = root_command();
  root.subcommands.push_back(
      {"known", "known", "Known command.", false, {}, {}, {},
       success_handler});
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {"missing"}, output, error) == 2);
  REQUIRE(output.empty());
  REQUIRE(error.find("unknown command") != std::string::npos);

  REQUIRE(dispatch(registry, {"--secret-token"}, output, error) == 2);
  REQUIRE(output.empty());
  REQUIRE(error.find("--secret-token") == std::string::npos);
}

TEST_CASE("handler exceptions are contained and redacted",
          "[commands][failure]") {
  auto root = root_command(throwing_handler);
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("failed internally") != std::string::npos);
  REQUIRE(error.find("sensitive handler failure") == std::string::npos);
}

TEST_CASE("parser-schema defects are internal dispatch failures",
          "[commands][failure]") {
  auto root = root_command();
  root.options = {
      {{"duplicate.one", {"--same"}, ArgumentValueKind::flag, 0, 1}, "",
       "One."},
      {{"duplicate.two", {"--same"}, ArgumentValueKind::flag, 0, 1}, "",
       "Two."}};
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("internal command dispatch error") != std::string::npos);
}

TEST_CASE("help and version are generated without dispatch", "[commands]") {
  auto root = root_command(throwing_handler);
  root.subcommands.push_back(
      {"child", "child", "Child help.", false, {}, {}, {},
       throwing_handler});
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {"--help"}, output, error) == 0);
  REQUIRE(output.starts_with("Usage: forge-test"));
  REQUIRE(output.find("Child help.") != std::string::npos);
  REQUIRE(error.empty());

  REQUIRE(dispatch(registry, {"child", "--help"}, output, error) == 0);
  REQUIRE(output.starts_with("Usage: forge-test child"));
  REQUIRE(output.find("Child help.") != std::string::npos);
  REQUIRE(error.empty());

  REQUIRE(dispatch(registry, {"--version"}, output, error) == 0);
  REQUIRE(output == "forge-test 1.2.3\n");
  REQUIRE(error.empty());
}

TEST_CASE("builtin commands expose honest offline behavior", "[commands]") {
  const auto& registry = builtin_command_registry();
  const auto schema = make_parser_schema(registry);
  REQUIRE(schema);
  REQUIRE(schema->root.subcommands.size() == 4);

  std::string output;
  std::string error;
  REQUIRE(dispatch(registry, {"chat"}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("not available in this build") != std::string::npos);

  REQUIRE(dispatch(registry, {"version"}, output, error) == 0);
  REQUIRE(output.starts_with("aiforge "));
  REQUIRE(error.empty());
}
