#include "../144chatsummary/fixture.hpp"
#include <aiforge/adapters/agent_transport.hpp>
#include <aiforge/adapters/process_context.hpp>
#include <aiforge/surfaces/context_control.hpp>
#include <array>
#include <atomic>
#include <sstream>

namespace {
using namespace chat_summary_test;
auto rolling(Fixture& f) -> void {
  auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(
      policy->policy.revision, domain::ConversationMode::rolling, {}));
}
auto preview(surfaces::ContextController& controller,
             const domain::ConversationSummaryCandidate& candidate)
    -> std::string {
  const auto reply = controller.execute(
      {1, surfaces::ContextPreview{version(candidate), {}, "draft"}});
  if (const auto* failed =
          std::get_if<surfaces::ContextFailure>(&reply.payload))
    INFO(failed->message);
  REQUIRE(std::holds_alternative<surfaces::ContextPreviewed>(reply.payload));
  return std::get<surfaces::ContextPreviewed>(reply.payload).handle;
}
} // namespace
TEST_CASE(
    "Context handles reject replay changed drafts and intervening mutations",
    "[context][review][failure]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  surfaces::ContextController controller{*f.chat, "first-instance"};
  const auto handle = preview(controller, candidate);
  SECTION("changed draft") {
    CHECK(std::holds_alternative<surfaces::ContextFailure>(
        controller.execute({2, surfaces::ContextApply{handle, "changed"}})
            .payload));
  }
  SECTION("different process") {
    surfaces::ContextController second{*f.chat, "second-instance"};
    preview(second, candidate);
    CHECK(std::holds_alternative<surfaces::ContextFailure>(
        second.execute({2, surfaces::ContextApply{handle, "draft"}}).payload));
    CHECK(std::holds_alternative<surfaces::ContextOk>(
        controller.execute({3, surfaces::ContextClose{}}).payload));
  }
  SECTION("attempted mutation") {
    CHECK(std::holds_alternative<surfaces::ContextFailure>(
        controller
            .execute({2,
                      surfaces::ContextPolicy{
                          0, domain::ConversationMode::rolling, {}}})
            .payload));
  }
  SECTION("superseding preview") {
    preview(controller, candidate);
  }
  const auto before = f.chat->event_log().events();
  CHECK(std::holds_alternative<surfaces::ContextFailure>(
      controller.execute({4, surfaces::ContextApply{handle, "draft"}})
          .payload));
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE("Context inspection keeps a review usable and apply remains explicit",
          "[context][review]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  surfaces::ContextController controller{*f.chat, "instance"};
  const auto before = f.chat->event_log().events();
  const auto handle = preview(controller, candidate);
  auto inspected = controller.execute({2, surfaces::ContextInspect{"draft"}});
  REQUIRE(
      std::holds_alternative<surfaces::ContextInspected>(inspected.payload));
  CHECK(f.chat->event_log().events() == before);
  CHECK(std::holds_alternative<domain::ConversationSummaryActivation>(
      controller.execute({3, surfaces::ContextApply{handle, "draft"}})
          .payload));
  const auto after = f.chat->event_log().events();
  CHECK(std::holds_alternative<surfaces::ContextFailure>(
      controller.execute({4, surfaces::ContextApply{handle, "draft"}})
          .payload));
  CHECK(f.chat->event_log().events() == after);
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE("Context preview pages retain one immutable proof across external "
          "binding changes",
          "[context][review][paging]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  surfaces::ContextController controller{*f.chat, "instance"};
  const auto first = controller.execute(
      {1, surfaces::ContextPreview{version(candidate), {}, "draft"}});
  REQUIRE(std::holds_alternative<surfaces::ContextPreviewed>(first.payload));
  const auto original = std::get<surfaces::ContextPreviewed>(first.payload);
  REQUIRE(f.chat->select_model(id<domain::ModelId>("another-model")));
  const auto before = f.chat->event_log().events();
  const auto second = controller.execute(
      {2, surfaces::ContextPreviewPage{original.handle, 16, 16}});
  REQUIRE(std::holds_alternative<surfaces::ContextPreviewed>(second.payload));
  const auto& page = std::get<surfaces::ContextPreviewed>(second.payload);
  CHECK(page.handle == original.handle);
  CHECK(page.context == original.context);
  CHECK(page.activation == original.activation);
  CHECK(page.review_sequence == original.review_sequence);
  CHECK(page.offset == 16);
  CHECK(f.chat->event_log().events() == before);
  CHECK(std::holds_alternative<surfaces::ContextFailure>(
      controller.execute({3, surfaces::ContextApply{original.handle, "draft"}})
          .payload));
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE("Context preview paging rejects changed durable session sequence",
          "[context][review][paging]") {
  Fixture f;
  const auto candidate = f.candidate();
  rolling(f);
  surfaces::ContextController controller{*f.chat, "instance"};
  const auto handle = preview(controller, candidate);
  const auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(policy->policy.revision,
                                          domain::ConversationMode::full, {}));
  const auto before = f.chat->event_log().events();
  CHECK(std::holds_alternative<surfaces::ContextFailure>(
      controller.execute({2, surfaces::ContextPreviewPage{handle, 0, 16}})
          .payload));
  CHECK(std::holds_alternative<surfaces::ContextFailure>(
      controller.execute({3, surfaces::ContextApply{handle, "draft"}})
          .payload));
  CHECK(f.chat->event_log().events() == before);
}
TEST_CASE(
    "Context polls only its own producer and never automatically publishes",
    "[context][generation]") {
  Fixture f;
  surfaces::ContextController controller{*f.chat, "instance"};
  REQUIRE(std::holds_alternative<surfaces::ContextGenerated>(
      controller.execute({9, f.request()}).payload));
  std::optional<surfaces::ContextReply> done;
  for (unsigned i{}; i < 1000 && !done; ++i) {
    auto reply = controller.poll();
    REQUIRE(reply);
    done = std::move(*reply);
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(done);
  CHECK(done->id == 9);
  REQUIRE(std::holds_alternative<surfaces::ContextGenerationFinished>(
      done->payload));
  const auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  CHECK(catalog->snapshot.candidates.empty());
  CHECK(catalog->snapshot.active.empty());
  CHECK(catalog->unpublished.size() == 1);
}
#ifndef _WIN32
#include <unistd.h>
namespace {
struct Pipe {
  std::array<int, 2> descriptors{-1, -1};
  Pipe() { REQUIRE(::pipe(descriptors.data()) == 0); }
  ~Pipe() {
    for (const auto fd : descriptors)
      if (fd >= 0) ::close(fd);
  }
  Pipe(const Pipe&) = delete;
  auto operator=(const Pipe&) -> Pipe& = delete;
  auto write(std::string_view text) -> void {
    REQUIRE(::write(descriptors[1], text.data(), text.size()) ==
            static_cast<ssize_t>(text.size()));
  }
  auto end() -> void {
    ::close(descriptors[1]);
    descriptors[1] = -1;
  }
};
} // namespace
TEST_CASE("Context transport bounds idle polling and requires complete lines",
          "[context][transport]") {
  Pipe input;
  Pipe output;
  std::stop_source stop;
  auto transport = adapters::AgentTransport::open(
      input.descriptors[0], output.descriptors[1], stop.get_token());
  REQUIRE(transport);
  const auto begin = std::chrono::steady_clock::now();
  auto idle = (*transport)->poll_line();
  REQUIRE(idle);
  CHECK(idle->state == adapters::TransportLineState::idle);
  CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds{1});
  input.write("one\ntwo\npartial");
  auto one = (*transport)->poll_line();
  REQUIRE(one);
  CHECK(one->text == "one");
  auto two = (*transport)->poll_line();
  REQUIRE(two);
  CHECK(two->text == "two");
  CHECK_FALSE((*transport)->read_request());
  SECTION("unterminated EOF") {
    input.end();
    CHECK_FALSE((*transport)->poll_line());
  }
  SECTION("cancelled open input") {
    stop.request_stop();
    auto cancelled = (*transport)->poll_line();
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == surfaces::AgentErrorCode::cancelled);
  }
  SECTION("complete EOF") {
    input.write("\n");
    input.end();
    auto line = (*transport)->poll_line();
    REQUIRE(line);
    CHECK(line->text == "partial");
    auto end = (*transport)->poll_line();
    REQUIRE(end);
    CHECK(end->state == adapters::TransportLineState::end);
  }
}
namespace {
struct Gate {
  std::atomic<bool> entered{};
  std::atomic<bool> cancelled{};
  std::atomic<unsigned> requests{};
};
class GatedStream final : public backend::BackendStream {
 public:
  explicit GatedStream(Gate& gate) : m_gate(gate) {}
  auto next(std::stop_token stop)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    switch (m_step++) {
      case 0:
        return backend::BackendEvent{backend::ResponseStarted{"response"}};
      case 1: {
        m_gate.entered.store(true);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!stop.stop_requested() &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        m_gate.cancelled.store(stop.stop_requested());
        return backend::BackendEvent{backend::UsageObserved{{7, 3, 0, 0}}};
      }
      case 2:
        return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
      default: return std::nullopt;
    }
  }

 private:
  Gate& m_gate;
  unsigned m_step{};
};
class GatedBackend final : public backend::Backend {
 public:
  Gate gate;
  auto start(backend::BackendRequest, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    ++gate.requests;
    return std::make_unique<GatedStream>(gate);
  }
};
} // namespace
TEST_CASE("Context EOF cancels a blocked producer and retains late accounting",
          "[context][transport][cancel]") {
  Fixture f;
  f.chat.reset();
  GatedBackend backend;
  auto opened = surfaces::ChatSession::open(
      {id<domain::ModelId>("model"), surfaces::ChatSessionOpen::Mode::resume,
       id<domain::SessionId>("session")},
      backend, f.models, &f.store, nullptr, {}, {1024U * 1024U, 256},
      f.dependencies);
  REQUIRE(opened);
  auto chat = std::move(*opened);
  Pipe input;
  Pipe output;
  input.write("{\"version\":1,\"id\":1,\"op\":\"summary.generate\",\"expected_"
              "sequence\":" +
              std::to_string(chat->event_log().last_sequence()) +
              ",\"runs\":[\"source-run\"]}\n");
  auto transport = adapters::AgentTransport::open(input.descriptors[0],
                                                  output.descriptors[1], {});
  REQUIRE(transport);
  std::atomic<bool> received{};
  std::jthread closer([&] {
    std::array<char, 4096> bytes{};
    const auto count =
        ::read(output.descriptors[0], bytes.data(), bytes.size());
    if (count > 0)
      received.store(
          std::string_view{bytes.data(), static_cast<std::size_t>(count)}.find(
              "generation.started") != std::string_view::npos);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!backend.gate.entered.load() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    input.end();
  });
  auto result = adapters::run_context_jsonl(*chat, **transport, {}, "instance");
  closer.join();
  INFO((result ? "closed" : result.error().message));
  REQUIRE(result);
  CHECK(received.load());
  CHECK(backend.gate.entered.load());
  CHECK(backend.gate.cancelled.load());
  CHECK(backend.gate.requests.load() == 1);
  CHECK_FALSE(chat->active());
  const auto& events = chat->event_log().events();
  CHECK(std::ranges::count_if(events, [](const auto& event) {
          return std::holds_alternative<domain::RunCancelled>(event.payload);
        }) == 1);
  CHECK(std::ranges::count_if(events, [](const auto& event) {
          return std::holds_alternative<domain::UsageRecorded>(event.payload);
        }) == 1);
  CHECK(std::ranges::none_of(events, [](const auto& event) {
    return std::holds_alternative<
               domain::ConversationSummaryCandidatePublished>(event.payload) ||
           std::holds_alternative<domain::ConversationSummaryActivated>(
               event.payload) ||
           std::holds_alternative<domain::ToolStarted>(event.payload);
  }));
}
TEST_CASE("Context loop refuses inherited active work without draining it",
          "[context][transport][recovery]") {
  Fixture f;
  REQUIRE(f.chat->generate_conversation_summary(f.request()));
  const auto before = f.chat->event_log().events();
  Pipe input;
  Pipe output;
  input.end();
  auto transport = adapters::AgentTransport::open(input.descriptors[0],
                                                  output.descriptors[1], {});
  REQUIRE(transport);
  CHECK_FALSE(
      adapters::run_context_jsonl(*f.chat, **transport, {}, "instance"));
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.chat->active());
  f.drain();
  CHECK(f.backend.requests().size() == 1);
}

#endif
namespace {
class Command final : public cli::ContextCommand {
 public:
  std::optional<Request> received;
  auto execute(Request request, cli::CommandEnvironment&, std::ostream&,
               std::ostream&)
      -> std::expected<void, cli::CommandFailure> override {
    received = std::move(request);
    return {};
  }
};
} // namespace
TEST_CASE(
    "Context CLI requires exact launch identities without changing agent mode",
    "[context][cli]") {
  std::istringstream input;
  std::ostringstream output, error;
  Command command;
  cli::CommandEnvironment environment{input, false, false, false, {}};
  environment.context = &command;
  for (const auto& args : std::vector<std::vector<std::string_view>>{
           {"context"},
           {"context", "--jsonl"},
           {"context", "--jsonl", "--session", "s"},
           {"context", "--jsonl", "--model", "m"}}) {
    CHECK(cli::run_cli(args, environment, output, error) == 2);
    CHECK_FALSE(command.received);
  }
  CHECK(cli::run_cli(std::vector<std::string_view>{"context", "--jsonl",
                                                   "--session", "s", "--model",
                                                   "m"},
                     environment, output, error) == 0);
  REQUIRE(command.received);
  CHECK(command.received->session_id.value() == "s");
  CHECK(command.received->model_id.value() == "m");
}
