#include <aiforge/cli/command_registry.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sstream>
#include <stdexcept>

namespace {
using namespace aiforge::cli;
class Admin final : public AdminCommand {
 public:
  unsigned calls{};
  std::optional<Request> seen;
  std::optional<CommandFailure> failure;
  bool throws{}, fail_output{}, stopped{}, terminal_input{};
  auto execute(Request request, CommandEnvironment& environment,
               std::ostream& output, std::ostream&)
      -> std::expected<void, CommandFailure> override {
    ++calls;
    seen = std::move(request);
    stopped = environment.stop_token.stop_requested();
    terminal_input = environment.input_is_terminal;
    if (throws) throw std::runtime_error("private-adapter-exception-sentinel");
    if (fail_output) output.setstate(std::ios::badbit);
    if (failure) return std::unexpected(*failure);
    output << "recorded\n";
    return {};
  }
};
struct Fixture {
  std::istringstream input;
  std::ostringstream output, error;
  Admin admin;
  CommandEnvironment environment{input, true, true, true, {}};
  Fixture() { environment.admin = &admin; }
  auto run(const std::vector<std::string_view>& arguments) -> int {
    return run_cli(arguments, environment, output, error);
  }
};
} // namespace

TEST_CASE(
    "Admin grammar refuses missing and unsupported requests before dispatch",
    "[admin][cli]") {
  const auto arguments = GENERATE(
      std::vector<std::string_view>{"admin"},
      std::vector<std::string_view>{"admin", "unknown"},
      std::vector<std::string_view>{"admin", "service"},
      std::vector<std::string_view>{"admin", "service", "a.service",
                                    "b.service"},
      std::vector<std::string_view>{"admin", "health", "extra"},
      std::vector<std::string_view>{"admin", "health", "--target"},
      std::vector<std::string_view>{"admin", "health", "--target", "local",
                                    "--target", "other"},
      std::vector<std::string_view>{"admin", "services", "--json", "--json"},
      std::vector<std::string_view>{"admin", "targets", "--target", "local"},
      std::vector<std::string_view>{"admin", "health", "--jsonl"},
      std::vector<std::string_view>{"admin", "health", "--model", "model"},
      std::vector<std::string_view>{"--model", "model", "admin", "health"},
      std::vector<std::string_view>{"--resume", "session", "admin", "health"},
      std::vector<std::string_view>{"--repository", "/repo", "admin", "health"},
      std::vector<std::string_view>{"--target", "subdir", "admin", "health"});
  Fixture f;
  CHECK(f.run(arguments) == 2);
  CHECK(f.admin.calls == 0);
  CHECK(f.output.str().empty());
  CHECK_FALSE(f.error.str().empty());
}
TEST_CASE(
    "Admin target labels and service identities have closed bounded shapes",
    "[admin][cli]") {
  Fixture f;
  SECTION("invalid targets") {
    const auto target = GENERATE(
        std::string{}, std::string{"Other"}, std::string{"-other"},
        std::string{"other-"}, std::string{"../other"}, std::string{"a b"},
        std::string{"a;id"}, std::string(65, 'a'), std::string{"a\x1b"});
    CHECK(f.run({"admin", "health", "--target", target}) == 2);
  }
  SECTION("invalid services") {
    const auto unit = GENERATE(
        std::string{}, std::string{".service"}, std::string{"a.socket"},
        std::string{"a.service/other"}, std::string{"a.service;id"},
        std::string{"a b.service"}, std::string{"a\\x20.service"},
        std::string(248, 'a') + ".service", std::string{"\x1b.service"},
        std::string{"-a.service"});
    CHECK(f.run({"admin", "service", "--", unit}) == 2);
  }
  CHECK(f.admin.calls == 0);
  CHECK(f.output.str().empty());
}
TEST_CASE("Admin unavailable and command failure outcomes preserve exit codes",
          "[admin][cli]") {
  Fixture f;
  SECTION("unavailable build") {
    f.environment.admin = nullptr;
    CHECK(f.run({"admin", "health"}) == 1);
    CHECK(f.admin.calls == 0);
    CHECK(f.error.str().find("not available") != std::string::npos);
  }
  SECTION("usage") {
    f.admin.failure = CommandFailure{CommandFailureKind::usage, "fixed usage"};
    CHECK(f.run({"admin", "health"}) == 2);
  }
  SECTION("runtime") {
    f.admin.failure =
        CommandFailure{CommandFailureKind::runtime, "fixed failure"};
    CHECK(f.run({"admin", "health"}) == 1);
  }
  SECTION("cancelled") {
    f.admin.failure =
        CommandFailure{CommandFailureKind::cancelled, "cancelled"};
    CHECK(f.run({"admin", "health"}) == 130);
  }
  SECTION("exception") {
    f.admin.throws = true;
    CHECK(f.run({"admin", "health"}) == 1);
    CHECK(f.error.str().find("private-adapter-exception-sentinel") ==
          std::string::npos);
  }
  SECTION("output failure") {
    f.admin.fail_output = true;
    CHECK(f.run({"admin", "health"}) == 1);
  }
}
TEST_CASE("Admin schema IDs and help remain valid without service execution",
          "[admin][cli]") {
  REQUIRE(make_parser_schema(builtin_command_registry()));
  const auto arguments =
      GENERATE(std::vector<std::string_view>{"admin", "--help"},
               std::vector<std::string_view>{"admin", "targets", "--help"},
               std::vector<std::string_view>{"admin", "health", "--help"},
               std::vector<std::string_view>{"admin", "services", "--help"},
               std::vector<std::string_view>{"admin", "service", "--help"});
  Fixture f;
  CHECK(f.run(arguments) == 0);
  CHECK(f.admin.calls == 0);
  CHECK(f.output.str().find("admin") != std::string::npos);
  CHECK(f.error.str().empty());
}
TEST_CASE(
    "Admin leaves dispatch exact closed requests without provider services",
    "[admin][cli]") {
  Fixture f;
  const auto selected = GENERATE(
      std::pair{std::string_view{"targets"}, AdminCommand::Operation::targets},
      std::pair{std::string_view{"health"}, AdminCommand::Operation::health},
      std::pair{std::string_view{"services"},
                AdminCommand::Operation::services},
      std::pair{std::string_view{"service"}, AdminCommand::Operation::service});
  const bool json = GENERATE(false, true);
  std::vector<std::string_view> arguments{"admin", selected.first};
  if (selected.second == AdminCommand::Operation::service)
    arguments.push_back("a.service");
  if (json) arguments.push_back("--json");
  REQUIRE(f.run(arguments) == 0);
  REQUIRE(f.admin.seen);
  CHECK(f.admin.calls == 1);
  CHECK(f.admin.seen->operation == selected.second);
  CHECK(f.admin.seen->target == "local");
  CHECK(f.admin.seen->format == (json ? AdminCommand::OutputFormat::json
                                      : AdminCommand::OutputFormat::text));
  CHECK(f.admin.seen->unit ==
        (selected.second == AdminCommand::Operation::service
             ? std::optional<std::string>{"a.service"}
             : std::nullopt));
  CHECK(f.environment.one_shot == nullptr);
  CHECK(f.environment.interactive == nullptr);
  CHECK(f.environment.models == nullptr);
  CHECK(f.output.str() == "recorded\n");
  CHECK(f.error.str().empty());
}
TEST_CASE(
    "Admin preserves explicit target unit format and cancellation context",
    "[admin][cli]") {
  Fixture f;
  f.environment.input_is_terminal = false;
  std::stop_source stop;
  stop.request_stop();
  f.environment.stop_token = stop.get_token();
  const auto target =
      GENERATE(std::string{"other_node-1"}, std::string(64, 'a'));
  const auto unit = GENERATE(std::string{"App@instance:1.service"},
                             std::string(247, 'a') + ".service");
  REQUIRE(f.run({"admin", "service", unit, "--target", target, "--json"}) == 0);
  REQUIRE(f.admin.seen);
  CHECK(f.admin.seen->target == target);
  CHECK(f.admin.seen->unit == unit);
  CHECK(f.admin.seen->format == AdminCommand::OutputFormat::json);
  CHECK(f.admin.stopped);
  CHECK_FALSE(f.admin.terminal_input);
}
