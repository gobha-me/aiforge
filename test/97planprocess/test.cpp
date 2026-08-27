#include <aiforge/adapters/process_plan.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aiforge;
using Json = nlohmann::json;

template <typename Id>
auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-plan-XXXXXX")
            .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto* created = ::mkdtemp(buffer.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

private:
  std::filesystem::path m_path;
};

class StateEnvironment final {
public:
  explicit StateEnvironment(const std::filesystem::path& path) {
    if (const auto* existing = std::getenv("XDG_STATE_HOME")) {
      m_previous = existing;
    }
    REQUIRE(::setenv("XDG_STATE_HOME", path.c_str(), 1) == 0);
  }

  ~StateEnvironment() {
    if (m_previous) {
      static_cast<void>(::setenv("XDG_STATE_HOME", m_previous->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv("XDG_STATE_HOME"));
    }
  }

private:
  std::optional<std::string> m_previous;
};

auto attributes() -> domain::RunStarted {
  return {id<domain::SurfaceId>("test"), id<domain::WorkspaceId>("workspace"),
          id<domain::PermissionProfileId>("observe"), std::nullopt};
}

auto revision() -> domain::PlanRevision {
  return {id<domain::PlanId>("plan"),
          id<domain::PlanRevisionId>("revision"),
          std::nullopt,
          "Inspect the pending plan",
          std::nullopt,
          {{id<domain::PlanTaskId>("task"),
            std::nullopt,
            {},
            "Implement the controller",
            {"The protocol is strict and bounded"},
            {domain::Effect::write},
            {{domain::Effect::write, "repository_path", "src"}}}},
          {}};
}

auto seed_session(const std::filesystem::path& state_root,
                  const domain::SessionId& session_id) -> void {
  auto store = adapters::SqliteSessionStore::open(state_root / "aiforge" /
                                                  "sessions.sqlite3");
  REQUIRE(store);
  testing::ScriptedBackend backend{{}};
  auto kernel = runtime::RunKernel::open_durable(
      {session_id, runtime::DurableSessionMode::create,
       domain::EventTimestamp{std::chrono::milliseconds{1}}},
      **store, backend);
  REQUIRE(kernel);
  REQUIRE((*kernel)->start_plan(
      {id<domain::RunId>("planning-run"), attributes(), revision()}));
}

auto output_lines(const std::string& output) -> std::vector<Json> {
  std::istringstream stream{output};
  std::vector<Json> result;
  std::string line;
  while (std::getline(stream, line))
    result.push_back(Json::parse(line));
  return result;
}

} // namespace

TEST_CASE("process plan emits one strict response per JSONL request",
          "[plan][jsonl][process][failure]") {
  TemporaryDirectory temporary;
  StateEnvironment environment_scope{temporary.path()};
  const auto session_id = id<domain::SessionId>("session");
  seed_session(temporary.path(), session_id);

  std::istringstream input{
      R"({"schema_version":1,"request_id":"inspect","operation":"inspect"})"
      "\n{bad\n"
      R"({"schema_version":1,"request_id":"inspect","operation":"inspect"})"
      "\n"};
  cli::CommandEnvironment environment{input, false, false, false, {}};
  std::ostringstream output;
  std::ostringstream error;
  adapters::ProcessPlanCommand command;
  const auto result =
      command.execute({cli::PlanCommand::SessionMode::resume, session_id},
                      environment, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == cli::CommandFailureKind::usage);
  REQUIRE(result.error().message.empty());
  REQUIRE(error.str().empty());

  const auto lines = output_lines(output.str());
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[0]["request_id"] == "inspect");
  REQUIRE(lines[0]["ok"] == true);
  REQUIRE(lines[0]["state"]["plan_state"] == "proposed");
  REQUIRE(lines[0]["state"]["pending_decision"]["run_id"] == "planning-run");
  REQUIRE(lines[0]["state"]["schedule"].size() == 1);
  REQUIRE(lines[1]["request_id"].is_null());
  REQUIRE(lines[1]["error"]["code"] == "malformed_json");
  REQUIRE(lines[2]["request_id"] == "inspect");
  REQUIRE(lines[2]["error"]["code"] == "duplicate_request_id");
}

TEST_CASE("process plan rejects terminal and empty protocol input",
          "[plan][jsonl][process][failure]") {
  TemporaryDirectory temporary;
  StateEnvironment environment_scope{temporary.path()};
  const auto session_id = id<domain::SessionId>("session");
  seed_session(temporary.path(), session_id);
  adapters::ProcessPlanCommand command;

  std::istringstream terminal_input;
  cli::CommandEnvironment terminal{terminal_input, true, true, true, {}};
  std::ostringstream output;
  std::ostringstream error;
  auto result =
      command.execute({cli::PlanCommand::SessionMode::resume, session_id},
                      terminal, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == cli::CommandFailureKind::usage);

  std::istringstream empty_input;
  cli::CommandEnvironment empty{empty_input, false, false, false, {}};
  result = command.execute({cli::PlanCommand::SessionMode::resume, session_id},
                           empty, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == cli::CommandFailureKind::usage);
  REQUIRE(result.error().message.find("no requests") != std::string::npos);
}

TEST_CASE("process plan bounds a line before JSON parsing",
          "[plan][jsonl][process][limits][failure]") {
  TemporaryDirectory temporary;
  StateEnvironment environment_scope{temporary.path()};
  const auto session_id = id<domain::SessionId>("session");
  seed_session(temporary.path(), session_id);
  adapters::ProcessPlanCommand command;

  std::istringstream input{std::string(1024U * 1024U + 1U, 'x')};
  cli::CommandEnvironment environment{input, false, false, false, {}};
  std::ostringstream output;
  std::ostringstream error;
  const auto result =
      command.execute({cli::PlanCommand::SessionMode::resume, session_id},
                      environment, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == cli::CommandFailureKind::usage);
  const auto lines = output_lines(output.str());
  REQUIRE(lines.size() == 1);
  REQUIRE(lines.front()["error"]["code"] == "resource_exhausted");
  REQUIRE(error.str().empty());
}

TEST_CASE("process plan durably approves promotes and resolves an exact task",
          "[plan][jsonl][process][mutation]") {
  TemporaryDirectory temporary;
  StateEnvironment environment_scope{temporary.path()};
  const auto session_id = id<domain::SessionId>("session");
  seed_session(temporary.path(), session_id);
  adapters::ProcessPlanCommand command;

  std::istringstream approval_input{
      R"({"schema_version":1,"request_id":"approve","operation":"decide","plan_id":"plan","revision_id":"revision","decision":"approved"})"
      "\n"};
  cli::CommandEnvironment approval_environment{approval_input, false, false,
                                                false, {}};
  std::ostringstream approval_output;
  std::ostringstream error;
  auto result = command.execute(
      {cli::PlanCommand::SessionMode::resume, session_id}, approval_environment,
      approval_output, error);
  REQUIRE(result);
  auto lines = output_lines(approval_output.str());
  REQUIRE(lines.size() == 1);
  REQUIRE(lines.front()["ok"] == true);
  REQUIRE(lines.front()["state"]["plan_state"] == "approved");
  REQUIRE(lines.front()["state"]["session_tasks"].size() == 1);
  const auto repository_id =
      lines.front()["state"]["repository_id"].get<std::string>();

  const Json promotion{{"schema_version", 1},
                       {"request_id", "promote"},
                       {"operation", "promote"},
                       {"run_id", "promotion-run"},
                       {"item_id", "backlog-item"},
                       {"repository_id", repository_id},
                       {"plan_id", "plan"},
                       {"revision_id", "revision"},
                       {"task_id", "task"}};
  std::istringstream promotion_input{promotion.dump() + '\n'};
  cli::CommandEnvironment promotion_environment{promotion_input, false, false,
                                                 false, {}};
  std::ostringstream promotion_output;
  result = command.execute(
      {cli::PlanCommand::SessionMode::resume, session_id},
      promotion_environment, promotion_output, error);
  REQUIRE(result);
  lines = output_lines(promotion_output.str());
  REQUIRE(lines.size() == 1);
  REQUIRE(lines.front()["state"]["project_backlog"].size() == 1);
  REQUIRE(lines.front()["state"]["project_backlog"][0]["status"] == "open");
  const auto status_event_id = lines.front()["state"]["project_backlog"][0]
                                     ["status_event_id"]
                                         .get<std::string>();

  const Json resolution{{"schema_version", 1},
                        {"request_id", "resolve"},
                        {"operation", "set_backlog_status"},
                        {"run_id", "status-run"},
                        {"item_id", "backlog-item"},
                        {"repository_id", repository_id},
                        {"status", "resolved"},
                        {"reason", "implemented"},
                        {"expected_status_event_id", status_event_id}};
  std::istringstream resolution_input{resolution.dump() + '\n'};
  cli::CommandEnvironment resolution_environment{resolution_input, false,
                                                  false, false, {}};
  std::ostringstream resolution_output;
  result = command.execute(
      {cli::PlanCommand::SessionMode::resume, session_id},
      resolution_environment, resolution_output, error);
  REQUIRE(result);
  lines = output_lines(resolution_output.str());
  REQUIRE(lines.size() == 1);
  REQUIRE(lines.front()["state"]["project_backlog"].size() == 1);
  REQUIRE(lines.front()["state"]["project_backlog"][0]["status"] ==
          "resolved");
  REQUIRE(lines.front()["state"]["project_backlog"][0]["reason"] ==
          "implemented");
  REQUIRE(error.str().empty());
}
