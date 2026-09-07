#include <aiforge/surfaces/agent.hpp>

#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/testing/application_launch_context.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace aiforge;

TEST_CASE("agent protocol rejects malformed or authority-bearing requests",
          "[agent][protocol][failure]") {
  const std::vector<std::string> invalid{
      "",
      "{",
      "[]",
      "{}",
      R"({"version":2,"operation":"replay","session_id":"session"})",
      R"({"version":1,"version":1,"operation":"replay","session_id":"session"})",
      R"({"version":1,"operation":"replay","session_id":"session","prompt":"bad"})",
      R"({"version":1,"operation":"submit","profile":"dev","tools":["ask_user"],"prompt":"x"})",
      R"({"version":1,"operation":"submit","profile":"dev","tools":["run_shell"],"prompt":"x"})",
      R"({"version":1,"operation":"submit","profile":"dev","tools":["run_process","run_process"],"prompt":"x"})",
      R"({"version":1,"operation":"submit","profile":"dev","tools":[],"prompt":"x"})",
      R"({"version":1,"operation":"submit","profile":"dev","tools":["run_process"],"prompt":"x","approval":"allow-all"})",
      R"({"version":1,"operation":"replay","session_id":"session"})"
      "\n{}",
      R"({"version":1,"operation":"replay","session_id":"session\ncontrol"})",
      std::string(1024U * 1024U + 1, ' '),
      std::string(100, '[') + "0" + std::string(100, ']')};
  for (const auto& input : invalid) {
    CAPTURE(input.substr(0, 160));
    CHECK_FALSE(surfaces::parse_agent_request(input));
  }
}

TEST_CASE("agent protocol preserves explicit selection and accepts EOF framing",
          "[agent][protocol]") {
  const auto request = surfaces::parse_agent_request(
      R"({"version":1,"operation":"submit","profile":"dev","tools":["read_repository_file","run_process"],"prompt":"Read and test","model":"model","session_id":"session"})"
      "\r\n");
  REQUIRE(request);
  CHECK(request->operation == surfaces::AgentOperation::submit);
  CHECK(request->prompt == "Read and test");
  CHECK(request->profile->value() == "dev");
  CHECK(request->tools ==
        std::vector<std::string>{"read_repository_file", "run_process"});
  CHECK(request->model->value() == "model");
  CHECK(request->session_id->value() == "session");
  const auto replay = surfaces::parse_agent_request(
      R"({"version":1,"operation":"replay","session_id":"session"})");
  REQUIRE(replay);
  CHECK(replay->operation == surfaces::AgentOperation::replay);
}

namespace {
using namespace std::chrono_literals;
template <class Id> auto id(std::string value) -> Id {
  return Id::from(value).value();
}
class MemoryStore final : public storage::SessionStore {
 public:
  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (sessions.contains(session.session_id)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::already_exists, "exists", false});
    }
    sessions.emplace(session.session_id,
                     storage::SessionInfo{session.session_id,
                                          session.created_at,
                                          session.created_at, 0});
    return {};
  }

  auto open_session(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    const auto found = sessions.find(session_id);
    if (found == sessions.end()) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    return found->second;
  }

  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    std::vector<storage::SessionInfo> result;
    for (const auto& [id, info] : sessions) {
      static_cast<void>(id);
      result.push_back(info);
    }
    return result;
  }

  auto append_events(const domain::SessionId& session_id,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (fail_appends) {
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "injected append failure", true});
    }
    const auto found = sessions.find(session_id);
    if (found == sessions.end()) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    auto& history = histories[session_id];
    if (!history.empty() && !events.empty() &&
        events.front().metadata.sequence <= history.back().metadata.sequence) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::conflict, "sequence", false});
    }
    history.insert(history.end(), events.begin(), events.end());
    if (!events.empty()) {
      found->second.last_sequence = events.back().metadata.sequence;
      found->second.last_activity_at = events.back().metadata.timestamp;
    }
    return {};
  }

  auto replay_events(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    if (!sessions.contains(session_id)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    return histories[session_id];
  }

  auto open_or_create_memory_journal(storage::MemoryJournalOpen request,
                                     std::stop_token stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    const auto existing =
        std::ranges::find(memory_journals, request.owner,
                          [](const auto& entry) -> const domain::MemoryOwner& {
                            return entry.first;
                          });
    if (existing != memory_journals.end()) {
      return sessions.at(existing->second);
    }
    if (auto created = create_session(
            {request.candidate_session_id, request.created_at}, stop_token);
        !created) {
      return std::unexpected(std::move(created.error()));
    }
    memory_journals.emplace_back(request.owner, request.candidate_session_id);
    return sessions.at(request.candidate_session_id);
  }

  bool fail_appends{};
  std::map<domain::SessionId, storage::SessionInfo> sessions;
  std::map<domain::SessionId, std::vector<domain::RunEvent>> histories;
  std::vector<std::pair<domain::MemoryOwner, domain::SessionId>>
      memory_journals;
};

class Sink final : public surfaces::AgentRecordSink {
 public:
  std::vector<std::string> records;
  std::function<bool(std::string_view)> reject;
  auto write_record(std::string_view record)
      -> std::expected<void, surfaces::AgentError> override {
    if (reject && reject(record))
      return std::unexpected(surfaces::AgentError{
          surfaces::AgentErrorCode::output_failed, "injected output failure"});
    records.emplace_back(record);
    return {};
  }
};
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{};
  bool cancelled{};
  bool released{};
};
class Stream final : public backend::BackendStream {
 public:
  Stream(domain::MessageId message, bool calls_tool, std::shared_ptr<Gate> gate)
      : m_message(std::move(message)), m_calls_tool(calls_tool),
        m_gate(std::move(gate)) {}
  auto next(std::stop_token token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_gate && m_step == 1) {
      std::stop_callback cancelled(token, [this] {
        std::lock_guard guard(m_gate->mutex);
        m_gate->cancelled = true;
        m_gate->changed.notify_all();
      });
      std::unique_lock lock(m_gate->mutex);
      m_gate->entered = true;
      m_gate->changed.notify_all();
      m_gate->changed.wait(lock, [this] { return m_gate->released; });
    }
    switch (m_step++) {
      case 0:
        return backend::BackendEvent{
            backend::ResponseStarted{"agent-response"}};
      case 1:
        return backend::BackendEvent{backend::UsageObserved{{7, 3, 0, 0}}};
      case 2:
        if (token.stop_requested()) {
          m_step = 4;
          return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
        }
        if (m_calls_tool)
          return backend::BackendEvent{
              backend::ToolCallDelta{id<domain::InvocationId>("agent-read"),
                                     "read_repository_file", "{}"}};
        return backend::BackendEvent{
            backend::ContentDelta{m_message, domain::TextBlock{"answer"}}};
      case 3:
        return backend::BackendEvent{backend::ResponseFinished{
            m_calls_tool ? domain::FinishReason::tool_call
                         : domain::FinishReason::stop}};
      default: return std::optional<backend::BackendEvent>{};
    }
  }

 private:
  domain::MessageId m_message;
  bool m_calls_tool{};
  std::shared_ptr<Gate> m_gate;
  int m_step{};
};
class Backend final : public backend::Backend,
                      public backend::ModelContextProvider {
 public:
  bool capabilities{true};
  bool calls_tool{};
  std::shared_ptr<Gate> gate;
  std::vector<backend::BackendRequest> requests;
  auto lookup(const domain::ModelId& model, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{
        model, 100000, 4096, {}, {{"tools", capabilities}}};
  }
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    requests.push_back(request);
    return std::make_unique<Stream>(request.assistant_message_id,
                                    calls_tool && requests.size() == 1, gate);
  }
};
struct Fixture {
  Backend backend;
  MemoryStore store;
  surfaces::ChatSessionDependencies dependencies;
  std::shared_ptr<testing::ScriptedToolExecutor> executor;
  Fixture() {
    runtime::ToolRegistry registry;
    executor = std::make_shared<testing::ScriptedToolExecutor>(
        std::vector<testing::ScriptedToolExchange>{});
    REQUIRE(registry.register_tool(
        {"read_repository_file",
         "Read file",
         {"application/schema+json", R"({"type":"object"})"},
         {domain::Effect::read},
         {{domain::Effect::read, "filesystem.root", "/repo"}}},
        executor, {}, runtime::ToolExecutorContract{"test.agent.read", "1"},
        runtime::ToolCategory::repository));
    dependencies.tools = registry.snapshot().value();
    const auto permission =
        id<domain::PermissionProfileId>("agent-prompt-policy");
    auto policy = runtime::make_tool_launch_policy(
        dependencies.tools, {permission,
                             testing::available_application_launch_context(
                                 runtime::RestrictionLevel::medium),
                             {}});
    INFO((policy ? "policy ready" : policy.error().message));
    REQUIRE(policy);
    dependencies.tool_policy = *policy;
    dependencies.permission_profile_id = permission;
    dependencies.surface_kind = surfaces::ChatSurfaceKind::agent;
  }
  auto open(surfaces::ChatSessionOpen::Mode mode =
                surfaces::ChatSessionOpen::Mode::create,
            std::optional<domain::SessionId> session = {})
      -> std::unique_ptr<surfaces::ChatSession> {
    auto opened = surfaces::ChatSession::open(
        {id<domain::ModelId>("model"), mode, session,
         domain::RunProvenance{
             "test", "fake", {}, id<domain::ModelId>("model"), {}, {}, {}, {}}},
        backend, backend,
        mode == surfaces::ChatSessionOpen::Mode::ephemeral ? nullptr : &store,
        nullptr, {}, {}, dependencies);
    INFO((opened ? "opened" : opened.error().message));
    REQUIRE(opened);
    return std::move(*opened);
  }
};
auto request() -> surfaces::AgentRequest {
  return {surfaces::AgentOperation::submit,
          {},
          {},
          id<domain::ToolProfileId>("dev"),
          {"read_repository_file"},
          "Read source"};
}
template <class Payload>
auto count(const std::vector<domain::RunEvent>& events) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<Payload>(event.payload);
      }));
}
} // namespace

TEST_CASE("agent driver rejects unbound requests and non-durable sessions "
          "before inference",
          "[agent][failure]") {
  Fixture fixture;
  auto session = fixture.open();
  Sink sink;
  auto wrong_session = request();
  wrong_session.session_id = id<domain::SessionId>("other");
  CHECK_FALSE(surfaces::run_agent_session(*session, wrong_session, sink));
  auto wrong_model = request();
  wrong_model.model = id<domain::ModelId>("other");
  CHECK_FALSE(surfaces::run_agent_session(*session, wrong_model, sink));
  auto unavailable = request();
  unavailable.tools = {"run_process"};
  CHECK_FALSE(surfaces::run_agent_session(*session, unavailable, sink));
  auto ephemeral = fixture.open(surfaces::ChatSessionOpen::Mode::ephemeral);
  CHECK_FALSE(surfaces::run_agent_session(*ephemeral, request(), sink));
  CHECK(fixture.backend.requests.empty());
  CHECK(sink.records.empty());
}

TEST_CASE(
    "agent driver fails closed before backend startup on persistence failure",
    "[agent][storage][failure]") {
  Fixture fixture;
  auto session = fixture.open();
  fixture.store.fail_appends = true;
  Sink sink;
  CHECK_FALSE(surfaces::run_agent_session(*session, request(), sink));
  CHECK(fixture.backend.requests.empty());
  CHECK(sink.records.empty());
}

TEST_CASE("agent driver cancels approval interaction without executing tools",
          "[agent][tools][failure]") {
  Fixture fixture;
  fixture.backend.calls_tool = true;
  auto session = fixture.open();
  Sink sink;
  const auto result = surfaces::run_agent_session(*session, request(), sink);
  INFO((result ? "finished" : result.error().message));
  REQUIRE(result);
  CHECK(result->status == surfaces::AgentStatus::interaction_required);
  CHECK(result->durable_terminal);
  CHECK_FALSE(session->active());
  CHECK(fixture.executor->recorded_invocations().empty());
  const auto& events = fixture.store.histories.at(session->session_id());
  CHECK(count<domain::RunCancelled>(events) == 1);
  CHECK(count<domain::ToolStarted>(events) == 0);
  CHECK(count<domain::UsageRecorded>(events) == 1);
}

TEST_CASE(
    "agent output failure drains late provider accounting after cancellation",
    "[agent][output][cancel][failure]") {
  Fixture fixture;
  fixture.backend.gate = std::make_shared<Gate>();
  auto gate = fixture.backend.gate;
  auto session = fixture.open();
  Sink sink;
  sink.reject = [&](std::string_view record) {
    if (record.find("accepted") == std::string_view::npos) return false;
    std::unique_lock lock(gate->mutex);
    return gate->changed.wait_for(lock, 1s, [&] { return gate->entered; });
  };
  std::jthread release([gate] {
    std::unique_lock lock(gate->mutex);
    gate->changed.wait_for(lock, 1s, [&] { return gate->cancelled; });
    lock.unlock();
    std::this_thread::sleep_for(20ms);
    lock.lock();
    gate->released = true;
    gate->changed.notify_all();
  });
  const auto result = surfaces::run_agent_session(*session, request(), sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == surfaces::AgentErrorCode::output_failed);
  CHECK_FALSE(session->active());
  const auto& events = fixture.store.histories.at(session->session_id());
  CHECK(count<domain::RunCancelled>(events) == 1);
  CHECK(count<domain::UsageRecorded>(events) == 1);
}

TEST_CASE("agent replay remains read only and cancellation aware",
          "[agent][replay][failure]") {
  Fixture fixture;
  auto session = fixture.open();
  Sink sink;
  const auto run = surfaces::run_agent_session(*session, request(), sink);
  INFO((run ? "finished" : run.error().message));
  REQUIRE(run);
  REQUIRE(run->status == surfaces::AgentStatus::completed);
  REQUIRE(run->durable_terminal);
  const auto calls = fixture.backend.requests.size();
  const auto events = fixture.store.histories.at(session->session_id());
  Sink replay;
  auto replayed =
      surfaces::replay_agent_session(session->session_id(), events, replay);
  REQUIRE(replayed);
  CHECK_FALSE(replayed->durable_terminal);
  CHECK(replay.records.size() == events.size() + 1);
  CHECK(fixture.backend.requests.size() == calls);
  std::stop_source stop;
  Sink cancelled;
  cancelled.reject = [&](std::string_view) {
    stop.request_stop();
    return false;
  };
  CHECK_FALSE(surfaces::replay_agent_session(session->session_id(), events,
                                             cancelled, stop.get_token()));
  CHECK(cancelled.records.size() == 1);
}

TEST_CASE("agent records bound escaped content before serialization",
          "[agent][protocol][failure]") {
  CHECK_FALSE(surfaces::agent_error_record(
      {surfaces::AgentErrorCode::run_failed,
       std::string(surfaces::agent_maximum_record_bytes, '\1')}));
  CHECK_FALSE(surfaces::agent_error_record(
      {surfaces::AgentErrorCode::run_failed, std::string{"\xFF"}}));
  const auto record = surfaces::agent_error_record(
      {surfaces::AgentErrorCode::run_failed, "quote\" and newline\n"});
  REQUIRE(record);
  CHECK(record->back() == '\n');
  CHECK(std::ranges::count(*record, '\n') == 1);
}
