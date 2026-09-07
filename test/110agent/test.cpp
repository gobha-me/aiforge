#include <aiforge/surfaces/agent.hpp>

#include <aiforge/runtime/automatic_approval_matcher.hpp>
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
    if (fail_appends || (reject_append && reject_append(events))) {
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
  std::function<bool(std::span<const domain::RunEvent>)> reject_append;
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
  int step{1};
};
class Stream final : public backend::BackendStream {
 public:
  Stream(domain::MessageId message, bool calls_tool, std::shared_ptr<Gate> gate,
         std::size_t request_number, std::string tool_name)
      : m_message(std::move(message)), m_calls_tool(calls_tool),
        m_gate(std::move(gate)), m_request_number(request_number),
        m_tool_name(std::move(tool_name)) {}
  auto next(std::stop_token token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_gate && m_step == m_gate->step) {
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
          return backend::BackendEvent{backend::ToolCallDelta{
              id<domain::InvocationId>("agent-read-" +
                                       std::to_string(m_request_number)),
              m_tool_name, "{}"}};
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
  std::size_t m_request_number{};
  std::string m_tool_name;
};
class Backend final : public backend::Backend,
                      public backend::ModelContextProvider {
 public:
  bool capabilities{true};
  bool calls_tool{};
  std::string tool_name{"read_repository_file"};
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
                                    calls_tool && requests.size() % 2 == 1,
                                    gate, requests.size(), tool_name);
  }
};
class ToolStream final : public runtime::ToolExecutionStream {
 public:
  explicit ToolStream(std::shared_ptr<Gate> gate) : m_gate(std::move(gate)) {}
  auto next(std::stop_token token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    if (m_finished) return std::optional<runtime::ToolExecutionEvent>{};
    std::stop_callback cancelled(token, [this] {
      std::lock_guard lock(m_gate->mutex);
      m_gate->cancelled = true;
      m_gate->changed.notify_all();
    });
    std::unique_lock lock(m_gate->mutex);
    m_gate->entered = true;
    m_gate->changed.notify_all();
    m_gate->changed.wait(lock, [this] { return m_gate->released; });
    m_finished = true;
    if (token.stop_requested())
      return std::unexpected(runtime::ToolExecutionError{
          runtime::ToolExecutionErrorCode::cancelled, "gated tool cancelled",
          false});
    return runtime::ToolExecutionEvent{
        runtime::ToolResult{{domain::TextBlock{"source contents"}}}};
  }

 private:
  std::shared_ptr<Gate> m_gate;
  bool m_finished{};
};
class Executor final : public runtime::ToolExecutor {
 public:
  std::shared_ptr<Gate> gate{std::make_shared<Gate>()};
  std::vector<runtime::ToolInvocation> invocations;
  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{arguments};
  }
  auto start(runtime::ToolInvocation invocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    invocations.push_back(std::move(invocation));
    return std::make_unique<ToolStream>(gate);
  }
};
auto release_after_cancel(const std::shared_ptr<Gate>& gate,
                          std::chrono::milliseconds delay = 20ms)
    -> std::jthread {
  return std::jthread([gate, delay] {
    std::unique_lock lock(gate->mutex);
    gate->changed.wait_for(lock, 1s, [&] { return gate->cancelled; });
    lock.unlock();
    std::this_thread::sleep_for(delay);
    lock.lock();
    gate->released = true;
    gate->changed.notify_all();
  });
}
auto await_entered(const std::shared_ptr<Gate>& gate) -> bool {
  std::unique_lock lock(gate->mutex);
  return gate->changed.wait_for(lock, 1s, [&] { return gate->entered; });
}
struct Fixture {
  Backend backend;
  MemoryStore store;
  surfaces::ChatSessionDependencies dependencies;
  std::shared_ptr<testing::ScriptedToolExecutor> executor;
  explicit Fixture(std::shared_ptr<Executor> executing = {}) {
    runtime::ToolRegistry registry;
    const std::string tool_name =
        executing ? "run_process" : "read_repository_file";
    backend.tool_name = tool_name;
    const auto effect =
        executing ? domain::Effect::execute : domain::Effect::read;
    executor = std::make_shared<testing::ScriptedToolExecutor>(
        std::vector<testing::ScriptedToolExchange>{});
    REQUIRE(registry.register_tool(
        {tool_name,
         "Hermetic test tool",
         {"application/schema+json", R"({"type":"object"})"},
         {effect},
         {{effect, executing ? "process.command" : "filesystem.root",
           executing ? "/usr/bin/test-agent" : "/repo"}}},
        executing ? std::static_pointer_cast<runtime::ToolExecutor>(executing)
                  : executor,
        {}, runtime::ToolExecutorContract{"test.agent.tool", "1"},
        executing ? runtime::ToolCategory::process
                  : runtime::ToolCategory::repository));
    dependencies.tools = registry.snapshot().value();
    const auto permission =
        id<domain::PermissionProfileId>("agent-prompt-policy");
    std::shared_ptr<runtime::AutomaticApprovalMatcher> matcher;
    if (executing) {
      auto compiled = runtime::compile_automatic_approval_matcher(
          {runtime::ExactToolArgumentsApprovalRule{
              tool_name,
              runtime::canonicalize_validated_tool_arguments(
                  {"application/json", "{}"})
                  .value(),
              {{runtime::RestrictionLevel::medium}, 1, {}, 0}}});
      REQUIRE(compiled);
      matcher = *compiled;
    }
    auto policy = runtime::make_tool_launch_policy(
        dependencies.tools,
        {permission,
         testing::available_application_launch_context(
             runtime::RestrictionLevel::medium,
             matcher ? runtime::ApprovalMode::automatic
                     : runtime::ApprovalMode::prompt,
             matcher ? std::optional{std::string{matcher->identity()}}
                     : std::nullopt),
         matcher});
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
auto request(std::string tool = "read_repository_file")
    -> surfaces::AgentRequest {
  return {surfaces::AgentOperation::submit,
          {},
          {},
          id<domain::ToolProfileId>("dev"),
          {std::move(tool)},
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

TEST_CASE(
    "agent rejects invalid and overflow-sized deadlines before submission",
    "[agent][deadline][failure]") {
  Fixture fixture;
  auto session = fixture.open();
  Sink sink;
  for (const auto limits : std::vector<surfaces::AgentRunLimits>{
           {0ms, 1s},
           {-1ms, 1s},
           {1s, 0ms},
           {std::chrono::milliseconds::max(), 1s},
           {1s, std::chrono::milliseconds::max()}}) {
    const auto outcome =
        surfaces::run_agent_session(*session, request(), sink, {}, limits);
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().code == surfaces::AgentErrorCode::invalid_request);
  }
  CHECK(fixture.backend.requests.empty());
  CHECK(sink.records.empty());
}

TEST_CASE("agent refuses recovered approval without draining or appending",
          "[agent][recovery][failure]") {
  Fixture fixture;
  fixture.backend.calls_tool = true;
  auto original = fixture.open();
  REQUIRE(original->select_tool_profile(id<domain::ToolProfileId>("dev")));
  REQUIRE(original->submit("pending read"));
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!original->pending_tool_approval() &&
         std::chrono::steady_clock::now() < deadline) {
    REQUIRE(original->drain());
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(original->pending_tool_approval());
  const auto session_id = original->session_id();
  original.reset();
  auto session =
      fixture.open(surfaces::ChatSessionOpen::Mode::resume, session_id);
  const auto before = fixture.store.histories.at(session_id);
  const auto requests = fixture.backend.requests.size();
  Sink sink;
  const auto outcome = surfaces::run_agent_session(*session, request(), sink);
  INFO((outcome ? outcome->reason : outcome.error().message));
  REQUIRE(outcome);
  CHECK(outcome->status == surfaces::AgentStatus::recovery_required);
  CHECK_FALSE(outcome->durable_terminal);
  CHECK(sink.records.size() == 1);
  CHECK(fixture.backend.requests.size() == requests);
  CHECK(fixture.executor->recorded_invocations().empty());
  CHECK(fixture.store.histories.at(session_id) == before);
}

TEST_CASE("agent reports mid-run persistence loss without claiming accounting",
          "[agent][storage][failure]") {
  Fixture fixture;
  auto session = fixture.open();
  Sink sink;
  sink.reject = [&](std::string_view record) {
    if (record.find("accepted") != std::string_view::npos)
      fixture.store.fail_appends = true;
    return false;
  };
  const auto outcome = surfaces::run_agent_session(*session, request(), sink);
  INFO((outcome ? outcome->reason : outcome.error().message));
  REQUIRE(outcome);
  CHECK(outcome->status == surfaces::AgentStatus::failed);
  CHECK_FALSE(outcome->durable_terminal);
  CHECK(outcome->reason.find("reopen") != std::string::npos);
  const auto& events = fixture.store.histories.at(session->session_id());
  CHECK(count<domain::RunCompleted>(events) == 0);
  CHECK(count<domain::UsageRecorded>(events) == 0);
  CHECK_FALSE(sink.records.empty());
  CHECK(sink.records.back().find("terminal") != std::string::npos);
}

TEST_CASE("agent drains terminal provider EOF after output failure or stop",
          "[agent][output][cancel][failure]") {
  Fixture fixture;
  fixture.backend.gate = std::make_shared<Gate>();
  fixture.backend.gate->step = 4;
  const auto gate = fixture.backend.gate;
  auto session = fixture.open();
  Sink sink;
  std::stop_source stop;
  bool reject_output = false;
  SECTION("output failed after durable completion") {
    reject_output = true;
  }
  SECTION("stop after durable completion") {
  }
  sink.reject = [&](std::string_view record) {
    if (record.find("run_completed") == std::string_view::npos) return false;
    REQUIRE(await_entered(gate));
    stop.request_stop();
    return reject_output;
  };
  // A terminal stream cannot be cancelled again. Release EOF independently,
  // after the sink has observed completion and requested cleanup.
  std::jthread release([&] {
    while (!stop.stop_requested())
      std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->changed.notify_all();
  });
  const auto outcome =
      surfaces::run_agent_session(*session, request(), sink, stop.get_token());
  if (reject_output) {
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().code == surfaces::AgentErrorCode::output_failed);
  } else {
    INFO((outcome ? outcome->reason : outcome.error().message));
    REQUIRE(outcome);
    CHECK(outcome->status == surfaces::AgentStatus::cancelled);
    CHECK(outcome->durable_terminal);
  }
  CHECK_FALSE(session->active());
  const auto& events = fixture.store.histories.at(session->session_id());
  CHECK(count<domain::RunCompleted>(events) == 1);
  CHECK(count<domain::RunCancelled>(events) == 0);
  CHECK(count<domain::UsageRecorded>(events) == 1);
}

TEST_CASE("agent work deadline includes accepted and final event writes",
          "[agent][deadline][failure]") {
  Fixture fixture;
  auto session = fixture.open();
  Sink sink;
  bool final_event = false;
  SECTION("accepted record consumes the work budget") {
  }
  SECTION("final durable event consumes the work budget") {
    final_event = true;
  }
  bool delayed = false;
  sink.reject = [&](std::string_view record) {
    const auto target = final_event ? "run_completed" : "accepted";
    if (!delayed && record.find(target) != std::string_view::npos) {
      delayed = true;
      std::this_thread::sleep_for(80ms);
    }
    return false;
  };
  const auto outcome =
      surfaces::run_agent_session(*session, request(), sink, {}, {40ms, 1s});
  INFO((outcome ? outcome->reason : outcome.error().message));
  REQUIRE(outcome);
  REQUIRE(delayed);
  CHECK(outcome->status == surfaces::AgentStatus::failed);
  CHECK(outcome->reason == "run deadline exceeded");
  CHECK(outcome->durable_terminal);
  CHECK_FALSE(session->active());
  CHECK(sink.records.back().find("terminal") != std::string::npos);
  if (final_event) {
    const auto& events = fixture.store.histories.at(session->session_id());
    CHECK(count<domain::RunCompleted>(events) == 1);
    CHECK(count<domain::RunCancelled>(events) == 0);
    CHECK(std::ranges::count_if(sink.records, [](const auto& record) {
            return record.find("run_completed") != std::string::npos;
          }) == 1);
  }
}

TEST_CASE("agent cancellation and output failure drain a real gated tool",
          "[agent][tools][cancel][failure]") {
  auto executor = std::make_shared<Executor>();
  Fixture fixture{executor};
  fixture.backend.calls_tool = true;
  auto session = fixture.open();
  Sink sink;
  std::stop_source stop;
  bool reject_output = false;
  SECTION("stop during tool execution") {
  }
  SECTION("output failure during tool execution") {
    reject_output = true;
  }
  sink.reject = [&](std::string_view record) {
    if (record.find("tool_started") == std::string_view::npos) return false;
    REQUIRE(await_entered(executor->gate));
    stop.request_stop();
    return reject_output;
  };
  auto release = release_after_cancel(executor->gate);
  const auto outcome = surfaces::run_agent_session(
      *session, request("run_process"), sink, stop.get_token());
  if (reject_output) {
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().code == surfaces::AgentErrorCode::output_failed);
  } else {
    INFO((outcome ? outcome->reason : outcome.error().message));
    REQUIRE(outcome);
    CHECK(outcome->status == surfaces::AgentStatus::cancelled);
    CHECK(outcome->durable_terminal);
  }
  CHECK_FALSE(session->active());
  REQUIRE(executor->invocations.size() == 1);
  CHECK(executor->gate->cancelled);
  const auto& events = fixture.store.histories.at(session->session_id());
  CHECK(count<domain::ToolStarted>(events) == 1);
  CHECK(count<domain::ToolErrored>(events) == 1);
  CHECK(count<domain::RunCancelled>(events) == 1);
  CHECK(count<domain::UsageRecorded>(events) == 1);
  CHECK(fixture.backend.requests.size() == 1);
}

TEST_CASE("agent cleanup deadline cannot claim a still-running tool is drained",
          "[agent][tools][deadline][failure]") {
  auto executor = std::make_shared<Executor>();
  Fixture fixture{executor};
  fixture.backend.calls_tool = true;
  auto session = fixture.open();
  Sink sink;
  std::stop_source stop;
  sink.reject = [&](std::string_view record) {
    if (record.find("tool_started") != std::string_view::npos) {
      REQUIRE(await_entered(executor->gate));
      stop.request_stop();
    }
    return false;
  };
  auto release = release_after_cancel(executor->gate, 100ms);
  const auto outcome = surfaces::run_agent_session(
      *session, request("run_process"), sink, stop.get_token(), {1s, 10ms});
  INFO((outcome ? outcome->reason : outcome.error().message));
  REQUIRE(outcome);
  CHECK(outcome->status == surfaces::AgentStatus::failed);
  CHECK_FALSE(outcome->durable_terminal);
  CHECK(outcome->reason.find("reopen") != std::string::npos);
  CHECK(session->active());
  CHECK(count<domain::RunCancelled>(
            fixture.store.histories.at(session->session_id())) == 1);
  release.join();
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (session->active() && std::chrono::steady_clock::now() < deadline) {
    REQUIRE(session->drain());
    std::this_thread::sleep_for(1ms);
  }
  CHECK_FALSE(session->active());
}

TEST_CASE("agent exact automatic rule executes once then refuses exhaustion",
          "[agent][tools][approval]") {
  auto executor = std::make_shared<Executor>();
  executor->gate->released = true;
  Fixture fixture{executor};
  fixture.backend.calls_tool = true;
  auto session = fixture.open();
  auto bound = request("run_process");
  bound.session_id = session->session_id();
  bound.model = session->model_id();
  Sink first;
  const auto completed = surfaces::run_agent_session(*session, bound, first);
  INFO((completed ? completed->reason : completed.error().message));
  REQUIRE(completed);
  CHECK(completed->status == surfaces::AgentStatus::completed);
  CHECK(completed->durable_terminal);
  REQUIRE(executor->invocations.size() == 1);
  CHECK(fixture.backend.requests.size() == 2);
  const auto& events = fixture.store.histories.at(session->session_id());
  CHECK(count<domain::ToolStarted>(events) == 1);
  CHECK(count<domain::ToolResultRecorded>(events) == 1);
  CHECK(count<domain::UsageRecorded>(events) == 2);
  Sink exhausted;
  const auto refused = surfaces::run_agent_session(*session, bound, exhausted);
  INFO((refused ? refused->reason : refused.error().message));
  REQUIRE(refused);
  CHECK(refused->status == surfaces::AgentStatus::interaction_required);
  CHECK(refused->durable_terminal);
  CHECK(executor->invocations.size() == 1);
  CHECK(fixture.backend.requests.size() == 3);
  CHECK(count<domain::ToolStarted>(events) == 1);
  CHECK(count<domain::RunCancelled>(events) == 1);
}

TEST_CASE("agent tool result persistence failure cannot report durable cleanup",
          "[agent][tools][storage][failure]") {
  auto executor = std::make_shared<Executor>();
  executor->gate->released = true;
  Fixture fixture{executor};
  fixture.backend.calls_tool = true;
  auto session = fixture.open();
  fixture.store.reject_append = [](std::span<const domain::RunEvent> events) {
    return std::ranges::any_of(events, [](const auto& event) {
      return std::holds_alternative<domain::ToolResultRecorded>(event.payload);
    });
  };
  Sink sink;
  const auto outcome =
      surfaces::run_agent_session(*session, request("run_process"), sink);
  INFO((outcome ? outcome->reason : outcome.error().message));
  REQUIRE(outcome);
  CHECK(outcome->status == surfaces::AgentStatus::failed);
  CHECK_FALSE(outcome->durable_terminal);
  CHECK(outcome->reason.find("reopen") != std::string::npos);
  CHECK(executor->invocations.size() == 1);
  CHECK(fixture.backend.requests.size() == 1);
  const auto& events = fixture.store.histories.at(session->session_id());
  CHECK(count<domain::ToolStarted>(events) == 1);
  CHECK(count<domain::ToolResultRecorded>(events) == 0);
  CHECK(count<domain::RunCompleted>(events) == 0);
  CHECK(count<domain::UsageRecorded>(events) == 1);
}
