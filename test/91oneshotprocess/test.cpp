#include <aiforge/adapters/process_one_shot.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <streambuf>
#include <string>

namespace {

class CancellingBuffer final : public std::streambuf {
 public:
  CancellingBuffer(std::stop_source& stop, const std::size_t size,
                   const bool throws)
      : m_stop(stop), m_size(size), m_throws(throws) {}

 protected:
  auto xsgetn(char* destination, const std::streamsize count)
      -> std::streamsize override {
    m_stop.request_stop();
    if (m_throws) throw std::runtime_error{"read failure"};
    const auto size = std::min(m_size, static_cast<std::size_t>(count));
    std::fill_n(destination, size, 'x');
    return static_cast<std::streamsize>(size);
  }

 private:
  std::stop_source& m_stop;
  std::size_t m_size;
  bool m_throws;
};

} // namespace

TEST_CASE("input cancellation wins over EOF, full buffers and read errors") {
  for (const auto size : {0U, 17U, 4096U}) {
    for (const auto throws : {false, true}) {
      for (const auto exceptions : {false, true}) {
        CAPTURE(size, throws, exceptions);
        std::stop_source stop;
        CancellingBuffer buffer{stop, size, throws};
        std::istream input{&buffer};
        if (exceptions) input.exceptions(std::ios::badbit);
        aiforge::cli::CommandEnvironment environment{input, false, false, false,
                                                     stop.get_token()};
        std::ostringstream output;
        std::ostringstream error;
        aiforge::adapters::ProcessOneShotCommand command;
        const auto result =
            command.execute({"explain"}, environment, output, error);
        REQUIRE_FALSE(result);
        CHECK(result.error().kind ==
              aiforge::cli::CommandFailureKind::cancelled);
        CHECK(output.str().empty());
        CHECK(error.str().empty());
      }
    }
  }
}

TEST_CASE("cancelled input never reaches process setup") {
  for (const auto terminal : {false, true}) {
    std::stop_source stop;
    stop.request_stop();
    std::istringstream input{"evidence"};
    aiforge::cli::CommandEnvironment environment{input, terminal, false, false,
                                                 stop.get_token()};
    std::ostringstream output;
    std::ostringstream error;
    aiforge::adapters::ProcessOneShotCommand command;
    const auto result =
        command.execute({"explain"}, environment, output, error);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == aiforge::cli::CommandFailureKind::cancelled);
    CHECK(input.tellg() == 0);
    CHECK(output.str().empty());
    CHECK(error.str().empty());
  }
}

TEST_CASE("portable input preserves bounded reads and read errors") {
  SECTION("overflow") {
    std::istringstream input{std::string(4097, 'x')};
    aiforge::cli::CommandEnvironment environment{
        input, false, false, false, {}};
    std::ostringstream output;
    std::ostringstream error;
    aiforge::adapters::ProcessOneShotCommand command{4096};
    const auto result =
        command.execute({"explain"}, environment, output, error);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == aiforge::cli::CommandFailureKind::usage);
    CHECK(result.error().message == "standard input exceeds 1 MiB");
    CHECK(error.str().empty());
  }
  SECTION("read error") {
    std::istringstream input;
    input.setstate(std::ios::badbit);
    aiforge::cli::CommandEnvironment environment{
        input, false, false, false, {}};
    std::ostringstream output;
    std::ostringstream error;
    aiforge::adapters::ProcessOneShotCommand command;
    const auto result =
        command.execute({"explain"}, environment, output, error);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == aiforge::cli::CommandFailureKind::runtime);
    CHECK(result.error().message == "standard input could not be read");
    CHECK(error.str().empty());
  }
}

#ifndef _WIN32
TEST_CASE("descriptor cancellation restores flags with the writer still open") {
  for (const auto size : {0U, 17U, 4096U}) {
    CAPTURE(size);
    int descriptors[2]{};
    REQUIRE(::pipe(descriptors) == 0);
    const auto flags = ::fcntl(descriptors[0], F_GETFL);
    const std::string bytes(size, 'x');
    REQUIRE(::write(descriptors[1], bytes.data(), bytes.size()) ==
            static_cast<ssize_t>(size));
    std::stop_source stop;
    std::istringstream input{"stream must not be read"};
    aiforge::cli::CommandEnvironment environment{input, false, false, false,
                                                 stop.get_token()};
    environment.input_descriptor = descriptors[0];
    std::ostringstream output;
    std::ostringstream error;
    aiforge::adapters::ProcessOneShotCommand command;
    std::jthread cancellation{[&] {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      stop.request_stop();
    }};
    const auto result =
        command.execute({"explain"}, environment, output, error);
    CHECK(::fcntl(descriptors[0], F_GETFL) == flags);
    CHECK(::close(descriptors[0]) == 0);
    CHECK(::close(descriptors[1]) == 0);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == aiforge::cli::CommandFailureKind::cancelled);
    CHECK(input.tellg() == 0);
    CHECK(output.str().empty());
    CHECK(error.str().empty());
  }
}

TEST_CASE("closed input descriptor fails before setup") {
  int descriptors[2]{};
  REQUIRE(::pipe(descriptors) == 0);
  REQUIRE(::close(descriptors[0]) == 0);
  REQUIRE(::close(descriptors[1]) == 0);
  std::istringstream input{"stream must not be read"};
  aiforge::cli::CommandEnvironment environment{input, false, false, false, {}};
  environment.input_descriptor = descriptors[0];
  std::ostringstream output;
  std::ostringstream error;
  aiforge::adapters::ProcessOneShotCommand command;
  const auto result = command.execute({"explain"}, environment, output, error);
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == aiforge::cli::CommandFailureKind::runtime);
  CHECK(result.error().message == "standard input could not be read");
  CHECK(input.tellg() == 0);
  CHECK(output.str().empty());
  CHECK(error.str().empty());
}
#endif
