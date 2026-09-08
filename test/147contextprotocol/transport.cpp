#include <aiforge/adapters/agent_transport.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
namespace {
using namespace aiforge;
struct Pipe {
  std::array<int, 2> descriptors{-1, -1};
  Pipe() { REQUIRE(::pipe(descriptors.data()) == 0); }
  ~Pipe() {
    for (const auto fd : descriptors)
      if (fd >= 0) ::close(fd);
  }
  Pipe(const Pipe&) = delete;
  auto operator=(const Pipe&) -> Pipe& = delete;
  auto send(std::string_view value) -> void {
    REQUIRE(::write(descriptors[1], value.data(), value.size()) ==
            static_cast<ssize_t>(value.size()));
  }
  auto end() -> void {
    ::close(descriptors[1]);
    descriptors[1] = -1;
  }
};
} // namespace
TEST_CASE("Context line polling does not wait for EOF or a partial record",
          "[context][transport]") {
  Pipe input;
  Pipe output;
  std::stop_source stop;
  auto transport = adapters::AgentTransport::open(
      input.descriptors[0], output.descriptors[1], stop.get_token());
  REQUIRE(transport);
  const auto start = std::chrono::steady_clock::now();
  auto idle = (*transport)->poll_line();
  REQUIRE(idle);
  CHECK(idle->state == adapters::TransportLineState::idle);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
  input.send("one\ntwo\npartial");
  auto one = (*transport)->poll_line();
  REQUIRE(one);
  CHECK(one->text == "one");
  auto two = (*transport)->poll_line();
  REQUIRE(two);
  CHECK(two->text == "two");
  CHECK_FALSE((*transport)->read_request());
  auto partial = (*transport)->poll_line();
  REQUIRE(partial);
  CHECK(partial->state == adapters::TransportLineState::idle);
  SECTION("unterminated EOF") {
    input.end();
    CHECK_FALSE((*transport)->poll_line());
    CHECK_FALSE((*transport)->poll_line());
  }
  SECTION("cancelled open input") {
    stop.request_stop();
    auto result = (*transport)->poll_line();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == surfaces::AgentErrorCode::cancelled);
  }
  SECTION("complete EOF") {
    input.send("\n");
    input.end();
    auto line = (*transport)->poll_line();
    REQUIRE(line);
    CHECK(line->text == "partial");
    auto end = (*transport)->poll_line();
    REQUIRE(end);
    CHECK(end->state == adapters::TransportLineState::end);
  }
}
TEST_CASE("Context oversized line fails without waiting for the input writer",
          "[context][transport][bounds]") {
  Pipe input;
  Pipe output;
  auto transport = adapters::AgentTransport::open(input.descriptors[0],
                                                  output.descriptors[1], {});
  REQUIRE(transport);
  std::string chunk(4096, 'x');
  std::expected<adapters::TransportLine, surfaces::AgentError> result =
      adapters::TransportLine{};
  for (std::size_t size{}; size <= surfaces::agent_maximum_input_bytes;
       size += chunk.size()) {
    input.send(chunk);
    result = (*transport)->poll_line();
    if (!result) break;
    CHECK(result->state == adapters::TransportLineState::idle);
  }
  REQUIRE_FALSE(result);
  CHECK(result.error().code == surfaces::AgentErrorCode::resource_exhausted);
  CHECK_FALSE((*transport)->poll_line());
}
TEST_CASE(
    "Legacy agent request mode remains single request and excludes line mode",
    "[context][transport][legacy]") {
  Pipe input;
  Pipe output;
  input.send("request");
  input.end();
  auto transport = adapters::AgentTransport::open(input.descriptors[0],
                                                  output.descriptors[1], {});
  REQUIRE(transport);
  auto request = (*transport)->read_request();
  REQUIRE(request);
  CHECK(*request == "request");
  CHECK_FALSE((*transport)->poll_line());
  CHECK_FALSE((*transport)->read_request());
}
#endif
