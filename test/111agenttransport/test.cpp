#include <aiforge/adapters/agent_transport.hpp>
#include <aiforge/cli/command_registry.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace {
class AgentCommandFake final : public aiforge::cli::AgentCommand {
 public:
  std::optional<Request> received;
  std::optional<aiforge::cli::CommandFailure> failure;
  auto execute(Request request, aiforge::cli::CommandEnvironment&,
               std::ostream&, std::ostream&)
      -> std::expected<void, aiforge::cli::CommandFailure> override {
    received = std::move(request);
    if (failure) return std::unexpected(*failure);
    return {};
  }
};
} // namespace

TEST_CASE(
    "agent command requires JSONL input and preserves explicit launch options",
    "[agent][cli]") {
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  AgentCommandFake command;
  aiforge::cli::CommandEnvironment environment{input, false, false, false, {}};
  environment.agent = &command;
  CHECK(aiforge::cli::run_cli(std::vector<std::string_view>{"agent"},
                              environment, output, error) == 2);
  CHECK_FALSE(command.received);
  environment.input_is_terminal = true;
  CHECK(aiforge::cli::run_cli(std::vector<std::string_view>{"agent", "--jsonl"},
                              environment, output, error) == 2);
  CHECK_FALSE(command.received);
  environment.input_is_terminal = false;
  CHECK(aiforge::cli::run_cli(
            std::vector<std::string_view>{"agent", "--jsonl", "--repository",
                                          "/repo", "--tool-restriction", "none",
                                          "--tool-approval", "automatic"},
            environment, output, error) == 0);
  REQUIRE(command.received);
  CHECK(command.received->repository == "/repo");
  CHECK(command.received->tool_restriction == "none");
  CHECK(command.received->tool_approval == "automatic");
  CHECK(output.str().empty());
  command.failure = aiforge::cli::CommandFailure{
      aiforge::cli::CommandFailureKind::cancelled, "cancelled"};
  CHECK(aiforge::cli::run_cli(std::vector<std::string_view>{"agent", "--jsonl"},
                              environment, output, error) == 130);
}

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using namespace aiforge;
struct Pipe {
  std::array<int, 2> descriptors{-1, -1};
  Pipe() { REQUIRE(::pipe(descriptors.data()) == 0); }
  ~Pipe() {
    for (auto descriptor : descriptors)
      if (descriptor >= 0) ::close(descriptor);
  }
  Pipe(const Pipe&) = delete;
  auto operator=(const Pipe&) -> Pipe& = delete;
  void close_writer() {
    ::close(descriptors[1]);
    descriptors[1] = -1;
  }
  void close_reader() {
    ::close(descriptors[0]);
    descriptors[0] = -1;
  }
};
} // namespace

TEST_CASE("agent transport rejects absent closed and wrong-mode descriptors",
          "[agent][transport][failure]") {
  Pipe input;
  Pipe output;
  CHECK_FALSE(adapters::AgentTransport::open(-1, output.descriptors[1], {}));
  CHECK_FALSE(adapters::AgentTransport::open(input.descriptors[1],
                                             output.descriptors[1], {}));
  CHECK_FALSE(adapters::AgentTransport::open(input.descriptors[0],
                                             output.descriptors[0], {}));
  const auto closed = input.descriptors[0];
  input.close_reader();
  CHECK_FALSE(
      adapters::AgentTransport::open(closed, output.descriptors[1], {}));
}

TEST_CASE(
    "agent transport cancels idle stdin and restores borrowed descriptor flags",
    "[agent][transport][cancel]") {
  Pipe input;
  Pipe output;
  const auto before_input = ::fcntl(input.descriptors[0], F_GETFL);
  const auto before_output = ::fcntl(output.descriptors[1], F_GETFL);
  std::stop_source stop;
  auto transport = adapters::AgentTransport::open(
      input.descriptors[0], output.descriptors[1], stop.get_token());
  REQUIRE(transport);
  std::jthread cancel([&] {
    std::this_thread::sleep_for(20ms);
    stop.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  const auto request = (*transport)->read_request();
  REQUIRE_FALSE(request);
  CHECK(request.error().code == surfaces::AgentErrorCode::cancelled);
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  transport->reset();
  CHECK(::fcntl(input.descriptors[0], F_GETFL) == before_input);
  CHECK(::fcntl(output.descriptors[1], F_GETFL) == before_output);
}

TEST_CASE("agent transport bounds stalled stdout and fails subsequent writes",
          "[agent][transport][failure]") {
  Pipe input;
  Pipe output;
  auto transport = adapters::AgentTransport::open(
      input.descriptors[0], output.descriptors[1], {}, 25ms);
  REQUIRE(transport);
  std::string record(1024U * 1024U, 'x');
  record.back() = '\n';
  const auto start = std::chrono::steady_clock::now();
  const auto written = (*transport)->write_record(record);
  REQUIRE_FALSE(written);
  CHECK(written.error().code == surfaces::AgentErrorCode::output_failed);
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  CHECK_FALSE((*transport)->write_record("{}\n"));
}

TEST_CASE("agent transport closed reader follows executable SIGPIPE policy",
          "[agent][transport][failure]") {
  Pipe input;
  Pipe output;
  auto transport = adapters::AgentTransport::open(input.descriptors[0],
                                                  output.descriptors[1], {});
  REQUIRE(transport);
  output.close_reader();
  const auto previous = std::signal(SIGPIPE, SIG_IGN);
  const auto result = (*transport)->write_record("{}\n");
  std::signal(SIGPIPE, previous);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == surfaces::AgentErrorCode::output_failed);
}

TEST_CASE(
    "agent transport collects through EOF once and writes complete JSON lines",
    "[agent][transport]") {
  Pipe input;
  Pipe output;
  const std::string request =
      R"({"version":1,"operation":"replay","session_id":"saved"})";
  REQUIRE(::write(input.descriptors[1], request.data(), request.size()) ==
          static_cast<ssize_t>(request.size()));
  input.close_writer();
  auto transport = adapters::AgentTransport::open(input.descriptors[0],
                                                  output.descriptors[1], {});
  REQUIRE(transport);
  const auto read = (*transport)->read_request();
  REQUIRE(read);
  CHECK(*read == request);
  CHECK_FALSE((*transport)->read_request());
  REQUIRE((*transport)->write_record("{}\n"));
  std::array<char, 3> bytes{};
  REQUIRE(::read(output.descriptors[0], bytes.data(), bytes.size()) == 3);
  CHECK(std::string(bytes.data(), bytes.size()) == "{}\n");
}

TEST_CASE("agent transport validates record framing and timeout bounds",
          "[agent][transport][failure]") {
  Pipe input;
  Pipe output;
  CHECK_FALSE(adapters::AgentTransport::open(input.descriptors[0],
                                             output.descriptors[1], {}, 0ms));
  CHECK_FALSE(adapters::AgentTransport::open(input.descriptors[0],
                                             output.descriptors[1], {},
                                             std::chrono::milliseconds::max()));
  for (const auto& record : std::vector<std::string>{
           "", "{}", "{}\n{}\n",
           std::string(surfaces::agent_maximum_record_bytes + 1, '\n')}) {
    auto transport = adapters::AgentTransport::open(input.descriptors[0],
                                                    output.descriptors[1], {});
    REQUIRE(transport);
    CHECK_FALSE((*transport)->write_record(record));
    CHECK_FALSE((*transport)->write_record("{}\n"));
  }
}

TEST_CASE("agent transport accepts regular file input at EOF",
          "[agent][transport]") {
  const auto close_file = [](std::FILE* file) { std::fclose(file); };
  std::unique_ptr<std::FILE, decltype(close_file)> file(std::tmpfile(),
                                                        close_file);
  REQUIRE(file);
  REQUIRE(std::fwrite("{}", 1, 2, file.get()) == 2);
  std::rewind(file.get());
  Pipe output;
  auto transport = adapters::AgentTransport::open(::fileno(file.get()),
                                                  output.descriptors[1], {});
  REQUIRE(transport);
  const auto read = (*transport)->read_request();
  REQUIRE(read);
  CHECK(*read == "{}");
}

TEST_CASE("agent transport stop wins while output is blocked",
          "[agent][transport][cancel]") {
  Pipe input;
  Pipe output;
  std::stop_source stop;
  auto transport = adapters::AgentTransport::open(
      input.descriptors[0], output.descriptors[1], stop.get_token());
  REQUIRE(transport);
  std::string record(1024U * 1024U, 'x');
  record.back() = '\n';
  std::jthread cancel([&] {
    std::this_thread::sleep_for(20ms);
    stop.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  const auto written = (*transport)->write_record(record);
  REQUIRE_FALSE(written);
  CHECK(written.error().code == surfaces::AgentErrorCode::cancelled);
  CHECK(std::chrono::steady_clock::now() - start < 1s);
}

TEST_CASE("agent transport does not write after a blocked record deadline",
          "[agent][transport][failure]") {
  Pipe input;
  Pipe output;
  auto transport = adapters::AgentTransport::open(
      input.descriptors[0], output.descriptors[1], {}, 1ms);
  REQUIRE(transport);
  std::array<char, 4096> buffer{};
  while (::write(output.descriptors[1], buffer.data(), buffer.size()) > 0) {
  }
  std::jthread reader([&] {
    std::this_thread::sleep_for(5ms);
    static_cast<void>(
        ::read(output.descriptors[0], buffer.data(), buffer.size()));
  });
  CHECK_FALSE((*transport)->write_record("{}\n"));
}
#endif
