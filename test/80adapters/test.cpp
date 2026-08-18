#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <aiforge/adapters/termforge_run_bridge.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/testing/scripted_backend.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto context(domain::ContentBlock content = domain::TextBlock{"hello"})
    -> domain::ConstructedContext {
  return domain::ConstructedContext{
      {domain::ContextEntry{make_id<domain::ContextEntryId>("context"),
                            domain::ContextEntryKind::conversation,
                            std::nullopt,
                            domain::Message{make_id<domain::MessageId>("user"),
                                            domain::Role::user,
                                            {std::move(content)},
                                            std::nullopt},
                            {make_id<domain::ContextSourceId>("source"),
                             std::nullopt, std::nullopt},
                            0,
                            1,
                            1}},
      {{make_id<domain::ContextEntryId>("context"),
        domain::ContextDecision::admitted, std::nullopt}},
      {4096, 512, 0},
      1};
}

auto request(domain::ConstructedContext built = context())
    -> backend::BackendRequest {
  return backend::BackendRequest{make_id<domain::InferenceId>("inference"),
                                 make_id<domain::MessageId>("assistant"),
                                 make_id<domain::ModelId>("test-model"),
                                 std::move(built),
                                 {},
                                 {0.5, 64, 7, {}}};
}

class LocalServer final {
 public:
  LocalServer() {
    m_server.Post(
        "/api/v1/chat/completions",
        [this](const httplib::Request& request, httplib::Response& response) {
          {
            std::lock_guard lock(m_mutex);
            m_body = request.body;
            m_authorization = request.get_header_value("Authorization");
          }
          response.set_content(
              "data: "
              "{\"id\":\"response\",\"choices\":[{\"delta\":{\"role\":"
              "\"assistant\"}}]}\n\n"
              "data: "
              "{\"id\":\"response\",\"choices\":[{\"delta\":{\"content\":"
              "\"hello\"}}]}\n\n"
              "data: "
              "{\"id\":\"response\",\"choices\":[{\"delta\":{},\"finish_"
              "reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,\"completion_"
              "tokens\":1,\"total_tokens\":3}}\n\n"
              "data: [DONE]\n\n",
              "text/event-stream");
        });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    REQUIRE(m_port > 0);
    m_thread = std::jthread([this] { m_server.listen_after_bind(); });
  }

  ~LocalServer() {
    m_server.stop();
    if (m_thread.joinable()) m_thread.join();
  }

  auto base_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api/v1";
  }

  auto body() -> std::string {
    std::lock_guard lock(m_mutex);
    return m_body;
  }

  auto authorization() -> std::string {
    std::lock_guard lock(m_mutex);
    return m_authorization;
  }

 private:
  httplib::Server m_server;
  int m_port{};
  std::jthread m_thread;
  std::mutex m_mutex;
  std::string m_body;
  std::string m_authorization;
};

class TestApp final : public termforge::App {
 private:
  auto on_render(termforge::Screen&) -> void override {}
};

}  // namespace

TEST_CASE("Venice adapter rejects unsupported content without a request",
          "[adapter][venice][failure]") {
  adapters::VeniceBackend backend{
      {"secret", "http://127.0.0.1:1", 10ms, 10ms, 10ms, 4}};
  auto unsupported = context(domain::ArtifactReferenceBlock{
      make_id<domain::ArtifactId>("artifact"), std::nullopt});
  const auto started = backend.start(request(std::move(unsupported)), {});
  REQUIRE_FALSE(started);
  REQUIRE(started.error().kind == backend::BackendErrorKind::request_rejected);
  REQUIRE(started.error().redacted_message.find("secret") == std::string::npos);
}

TEST_CASE("Venice adapter maps structured SSE into neutral events",
          "[adapter][venice]") {
  LocalServer server;
  adapters::VeniceBackend backend{
      {"test-secret", server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  std::vector<backend::BackendEvent> events;
  for (;;) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
    events.push_back(std::move(**next));
  }

  REQUIRE(events.size() == 4);
  REQUIRE(std::holds_alternative<backend::ResponseStarted>(events[0]));
  REQUIRE(std::holds_alternative<backend::ContentDelta>(events[1]));
  REQUIRE(std::get<backend::ContentDelta>(events[1]).message_id ==
          make_id<domain::MessageId>("assistant"));
  REQUIRE(std::holds_alternative<backend::UsageObserved>(events[2]));
  REQUIRE(std::get<backend::UsageObserved>(events[2]).usage ==
          domain::Usage{2, 1, 0, 0});
  REQUIRE(std::holds_alternative<backend::ResponseFinished>(events[3]));

  const auto sent = nlohmann::json::parse(server.body());
  REQUIRE(sent.at("model") == "test-model");
  REQUIRE(sent.at("messages").at(0).at("content") == "hello");
  REQUIRE(server.authorization() == "Bearer test-secret");
  REQUIRE(server.body().find("test-secret") == std::string::npos);
}

TEST_CASE("TermForge bridge converts its marker to an owner-thread drain",
          "[adapter][termforge]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{
          {backend::ResponseStarted{"response"},
           backend::ResponseFinished{domain::FinishReason::stop},
           testing::EndOfStream{}}}}}};
  TestApp app;
  adapters::TermForgeRunBridge bridge{app};
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                            &bridge};
  const runtime::RunStart start{
      make_id<domain::RunId>("run"),
      {make_id<domain::SurfaceId>("tui"), make_id<domain::WorkspaceId>("chat"),
       make_id<domain::PermissionProfileId>("observe"), std::nullopt},
      {make_id<domain::MessageId>("user"),
       domain::Role::user,
       {domain::TextBlock{"hello"}},
       std::nullopt},
      backend_request};
  REQUIRE(kernel.start(start));

  const termforge::Event marker = termforge::ErrorEvent{
      termforge::Severity::Info, "aiforge.runtime", "events-ready"};
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    auto handled = bridge.handle(marker, kernel);
    REQUIRE(handled);
    std::this_thread::yield();
  }
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(kernel.projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::completed);
}
