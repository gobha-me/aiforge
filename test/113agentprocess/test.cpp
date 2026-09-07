#include <aiforge/adapters/agent_transport.hpp>
#include <aiforge/adapters/filesystem_artifact_store.hpp>
#include <aiforge/adapters/process_chat_assembly.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/surfaces/agent.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
using Json = nlohmann::json;

template <class Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}

class Directory final {
 public:
  Directory() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "aiforge-agent-process-XXXXXX")
                              .string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    path = std::filesystem::canonical(pattern);
  }
  ~Directory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;
};

class Pipe final {
 public:
  Pipe() { REQUIRE(::pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) == 0); }
  ~Pipe() {
    for (const int descriptor : ends)
      if (descriptor >= 0) static_cast<void>(::close(descriptor));
  }
  auto close_reader() -> void {
    REQUIRE(::close(ends[0]) == 0);
    ends[0] = -1;
  }
  std::array<int, 2> ends{};
};

// A thread-local mask prevents EPIPE from terminating Catch, without changing
// the application's signal disposition. Only test-generated pending SIGPIPE is
// consumed before restoring the original mask.
class SignalMask final {
 public:
  SignalMask() {
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGPIPE);
    ::sigaddset(&signals, SIGINT);
    REQUIRE(::pthread_sigmask(SIG_BLOCK, &signals, &previous) == 0);
    sigset_t pending;
    REQUIRE(::sigpending(&pending) == 0);
    previous_pipe_pending = ::sigismember(&pending, SIGPIPE) == 1;
  }
  ~SignalMask() {
    sigset_t pipe;
    ::sigemptyset(&pipe);
    ::sigaddset(&pipe, SIGPIPE);
    timespec immediate{};
    if (!previous_pipe_pending)
      while (::sigtimedwait(&pipe, nullptr, &immediate) == SIGPIPE) {
      }
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
  }
  sigset_t signals{};
  sigset_t previous{};
  bool previous_pipe_pending{};
};

class OwnedProcesses final {
 public:
  ~OwnedProcesses() {
    for (const int descriptor : descriptors) {
      if (descriptor < 0) continue;
      // pidfds pin identities; cleanup can never signal a reused numeric PID.
      static_cast<void>(
          ::syscall(SYS_pidfd_send_signal, descriptor, SIGKILL, nullptr, 0));
      static_cast<void>(::close(descriptor));
    }
  }
  auto await_marker(const std::filesystem::path& path) -> void {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::array<pid_t, 2> pids{};
    while (std::chrono::steady_clock::now() < deadline) {
      std::ifstream marker{path / "owned-pids"};
      if (marker >> pids[0] >> pids[1]) break;
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(pids[0] > 1);
    REQUIRE(pids[1] > 1);
    REQUIRE(pids[0] != pids[1]);
    REQUIRE(pids[0] != ::getpid());
    REQUIRE(pids[1] != ::getpid());
    for (std::size_t index{}; index < pids.size(); ++index) {
      descriptors[index] =
          static_cast<int>(::syscall(SYS_pidfd_open, pids[index], 0));
      REQUIRE(descriptors[index] >= 0);
    }
  }
  auto exited() const -> bool {
    for (const int descriptor : descriptors) {
      pollfd item{descriptor, POLLIN, 0};
      if (descriptor < 0 || ::poll(&item, 1, 1000) != 1 ||
          (item.revents & POLLIN) == 0)
        return false;
    }
    return true;
  }
  std::array<int, 2> descriptors{-1, -1};
};

class Stream final : public backend::BackendStream {
 public:
  Stream(domain::MessageId message, std::string arguments, const bool initial)
      : m_message(std::move(message)), m_arguments(std::move(arguments)),
        m_initial(initial) {}
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    switch (m_step++) {
      case 0:
        return backend::BackendEvent{backend::ResponseStarted{"fake-response"}};
      case 1:
        return backend::BackendEvent{backend::UsageObserved{{7, 3, 0, 0}}};
      case 2:
        if (m_initial)
          return backend::BackendEvent{
              backend::ToolCallDelta{id<domain::InvocationId>("owned-process"),
                                     "run_process", m_arguments}};
        return backend::BackendEvent{
            backend::ContentDelta{m_message, domain::TextBlock{"completed"}}};
      case 3:
        return backend::BackendEvent{backend::ResponseFinished{
            m_initial ? domain::FinishReason::tool_call
                      : domain::FinishReason::stop}};
      default: return std::optional<backend::BackendEvent>{};
    }
  }

 private:
  domain::MessageId m_message;
  std::string m_arguments;
  bool m_initial{};
  unsigned m_step{};
};

class Backend final : public backend::Backend,
                      public backend::ModelContextProvider {
 public:
  std::string arguments;
  std::vector<backend::BackendRequest> requests;
  auto lookup(const domain::ModelId& model, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{
        model, 200000, 4096, {}, {{"tools", true}}};
  }
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    requests.push_back(request);
    return std::make_unique<Stream>(request.assistant_message_id, arguments,
                                    requests.size() == 1);
  }
};

struct Fixture {
  Directory directory;
  Backend backend;
  std::unique_ptr<adapters::FilesystemArtifactStore> artifacts;
  std::unique_ptr<adapters::SqliteSessionStore> store;
  std::unique_ptr<surfaces::ChatSession> session;
  explicit Fixture(const std::string& mode) {
    auto opened_artifacts =
        adapters::FilesystemArtifactStore::open(directory.path / "artifacts");
    REQUIRE(opened_artifacts);
    artifacts = std::move(*opened_artifacts);
    auto opened_store = adapters::SqliteSessionStore::open(
        directory.path / "state" / "sessions.sqlite3");
    REQUIRE(opened_store);
    store = std::move(*opened_store);
    config::ProcessConfigSettings settings;
    settings.executable_allowlist = {
        std::filesystem::canonical(AGENT_PROCESS_FIXTURE).string()};
    settings.readable_roots = {directory.path.string()};
    settings.writable_roots = settings.readable_roots;
    settings.unrestricted_network = true;
    settings.allowlist_automatic_approval_maximum_matches = 1;
    auto matcher = runtime::compile_automatic_approval_matcher(
        adapters::configured_process_automatic_approval_rules(settings));
    REQUIRE(matcher);
    runtime::ToolRegistry registry;
    auto assembly = adapters::assemble_process_chat_tool(
        registry,
        {.durable_session = true,
         .settings = settings,
         .restriction = runtime::RestrictionLevel::none,
         .approval = runtime::ApprovalMode::automatic,
         .matcher_policy_identity = std::string{(*matcher)->identity()},
         .artifact_store = artifacts.get(),
         .environment_lookup = {},
         .establish_launcher = adapters::establish_linux_process_launcher});
    INFO((assembly ? "assembled" : assembly.error().message));
    REQUIRE(assembly);
    REQUIRE(assembly->process_registered);
    auto tools = registry.snapshot();
    REQUIRE(tools);
    const auto permission =
        id<domain::PermissionProfileId>("agent-owned-process");
    auto policy = runtime::make_tool_launch_policy(
        *tools, {permission, assembly->launch_context, *matcher});
    REQUIRE(policy);
    surfaces::ChatSessionDependencies dependencies;
    dependencies.tools = *tools;
    dependencies.tool_policy = *policy;
    dependencies.permission_profile_id = permission;
    dependencies.surface_kind = surfaces::ChatSurfaceKind::agent;
    backend.arguments =
        Json{{"arguments", {mode}},
             {"environment", Json::array()},
             {"executable", settings.executable_allowlist.front()},
             {"output_bytes", 4096},
             {"readable_roots", settings.readable_roots},
             {"writable_roots", settings.writable_roots},
             {"stdin", "closed"},
             {"timeout_ms", 5000},
             {"working_directory", directory.path.string()}}
            .dump();
    auto opened = surfaces::ChatSession::open(
        {id<domain::ModelId>("fake-model"),
         surfaces::ChatSessionOpen::Mode::create,
         {},
         domain::RunProvenance{"test",
                               "fake",
                               {},
                               id<domain::ModelId>("fake-model"),
                               {},
                               {},
                               {},
                               {}}},
        backend, backend, store.get(), nullptr, {}, {}, dependencies);
    INFO((opened ? "opened" : opened.error().message));
    REQUIRE(opened);
    session = std::move(*opened);
  }
  auto request() const -> surfaces::AgentRequest {
    return {surfaces::AgentOperation::submit,
            {},
            {},
            id<domain::ToolProfileId>("process"),
            {"run_process"},
            "Run the explicitly configured harmless fixture"};
  }
};

enum class Disruption { blocked, broken, interrupt };

class DisruptingSink final : public surfaces::AgentRecordSink {
 public:
  adapters::AgentTransport& transport;
  Pipe& pipe;
  Fixture& fixture;
  OwnedProcesses& processes;
  Disruption disruption;
  pthread_t signal_thread{};
  bool triggered{};
  std::jthread consumer;
  DisruptingSink(adapters::AgentTransport& selected_transport, Pipe& output,
                 Fixture& selected_fixture, OwnedProcesses& owned,
                 Disruption selected)
      : transport(selected_transport), pipe(output), fixture(selected_fixture),
        processes(owned), disruption(selected),
        consumer([this](std::stop_token token) {
          std::array<char, 4096> bytes{};
          while (!token.stop_requested()) {
            if (::read(pipe.ends[0], bytes.data(), bytes.size()) <= 0)
              std::this_thread::sleep_for(1ms);
          }
        }) {}
  auto write_record(std::string_view record)
      -> std::expected<void, surfaces::AgentError> override {
    if (!triggered &&
        record.find("\"kind\":\"tool_started\"") != std::string_view::npos) {
      triggered = true;
      processes.await_marker(fixture.directory.path);
      if (disruption != Disruption::interrupt) {
        consumer.request_stop();
        consumer.join();
      }
      if (disruption == Disruption::broken)
        pipe.close_reader();
      else if (disruption == Disruption::interrupt)
        REQUIRE(::pthread_kill(signal_thread, SIGINT) == 0);
      else {
        const std::array<char, 4096> filler{};
        while (::write(pipe.ends[1], filler.data(), filler.size()) > 0) {
        }
        REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
      }
    }
    return transport.write_record(record);
  }
};

template <class Payload>
auto count(const std::vector<domain::RunEvent>& events) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<Payload>(event.payload);
      }));
}
} // namespace

TEST_CASE("agent transport failures cancel only the running owned process tree",
          "[agent][process][transport][failure]") {
  SignalMask mask;
  auto disruption = Disruption::blocked;
  SECTION("blocked consumer") {
    disruption = Disruption::blocked;
  }
  SECTION("broken consumer") {
    disruption = Disruption::broken;
  }
  SECTION("SIGINT during process execution") {
    disruption = Disruption::interrupt;
  }
  Fixture fixture{"wait"};
  Pipe input;
  Pipe output;
  std::stop_source stop;
  std::atomic<bool> signal_received{};
  std::jthread signal_waiter([&](std::stop_token token) {
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    while (!token.stop_requested()) {
      const timespec deadline{0, 10'000'000};
      if (::sigtimedwait(&signals, nullptr, &deadline) == SIGINT) {
        signal_received = true;
        stop.request_stop();
        return;
      }
    }
  });
  auto transport = adapters::AgentTransport::open(input.ends[0], output.ends[1],
                                                  stop.get_token(), 100ms);
  REQUIRE(transport);
  OwnedProcesses owned;
  DisruptingSink sink{**transport, output, fixture, owned, disruption};
  sink.signal_thread = signal_waiter.native_handle();
  const auto began = std::chrono::steady_clock::now();
  const auto result = surfaces::run_agent_session(
      *fixture.session, fixture.request(), sink, stop.get_token(), {4s, 2s});
  INFO((result ? "settled" : result.error().message));
  CHECK(std::chrono::steady_clock::now() - began < 4s);
  REQUIRE(sink.triggered);
  CHECK(owned.exited());
  const auto events =
      fixture.store->replay_events(fixture.session->session_id());
  REQUIRE(events);
  CHECK(count<domain::ToolStarted>(*events) == 1);
  CHECK(count<domain::UsageRecorded>(*events) == 1);
  CHECK(count<domain::ToolErrored>(*events) +
            count<domain::ToolResultRecorded>(*events) ==
        1);
  CHECK(count<domain::RunCancelled>(*events) == 1);
  CHECK(fixture.backend.requests.size() == 1);
  CHECK_FALSE(fixture.session->active());
  if (disruption == Disruption::interrupt) {
    CHECK(signal_received);
    REQUIRE(result);
    CHECK(result->status == surfaces::AgentStatus::cancelled);
    CHECK(result->durable_terminal);
  } else {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == surfaces::AgentErrorCode::output_failed);
  }
}

TEST_CASE(
    "configured automatic process run preserves output result and accounting",
    "[agent][process][smoke]") {
  SignalMask mask;
  Fixture fixture{"complete"};
  Pipe input;
  Pipe output;
  auto transport =
      adapters::AgentTransport::open(input.ends[0], output.ends[1], {});
  REQUIRE(transport);
  std::string captured;
  std::jthread consumer([&](std::stop_token token) {
    std::array<char, 4096> bytes{};
    while (!token.stop_requested()) {
      const auto size = ::read(output.ends[0], bytes.data(), bytes.size());
      if (size > 0)
        captured.append(bytes.data(), static_cast<std::size_t>(size));
      else
        std::this_thread::sleep_for(1ms);
    }
  });
  const auto result = surfaces::run_agent_session(
      *fixture.session, fixture.request(), **transport, {}, {4s, 2s});
  consumer.request_stop();
  consumer.join();
  std::array<char, 4096> remaining{};
  for (auto size = ::read(output.ends[0], remaining.data(), remaining.size());
       size > 0;
       size = ::read(output.ends[0], remaining.data(), remaining.size()))
    captured.append(remaining.data(), static_cast<std::size_t>(size));
  INFO((result ? "completed" : result.error().message));
  REQUIRE(result);
  CHECK(result->status == surfaces::AgentStatus::completed);
  CHECK(result->durable_terminal);
  const auto events =
      fixture.store->replay_events(fixture.session->session_id());
  REQUIRE(events);
  CHECK(count<domain::ToolStarted>(*events) == 1);
  CHECK(count<domain::ToolResultRecorded>(*events) == 1);
  CHECK(count<domain::ToolErrored>(*events) == 0);
  CHECK(count<domain::UsageRecorded>(*events) == 2);
  CHECK(count<domain::RunCompleted>(*events) == 1);
  CHECK(count<domain::ToolApprovalRequested>(*events) == 0);
  const auto allowed = std::ranges::find_if(*events, [](const auto& event) {
    const auto* decision =
        std::get_if<domain::ToolPolicyDecided>(&event.payload);
    return decision != nullptr &&
           decision->decision == domain::PolicyDecision::allow &&
           decision->source ==
               domain::PolicyDecisionSource::automatic_matcher &&
           decision->automatic_approval.has_value();
  });
  CHECK(allowed != events->end());
  CHECK(fixture.backend.requests.size() == 2);
  CHECK(captured.find("owned process stdout") != std::string::npos);
  CHECK(captured.find("owned process stderr") != std::string::npos);
}
