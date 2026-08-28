#include <aiforge/adapters/process_plan.hpp>

#include <algorithm>
#include <chrono>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <aiforge/adapters/process_repository.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/backend/backend.hpp>
#include <aiforge/runtime/plan_task_controller.hpp>
#include <nlohmann/json.hpp>

namespace aiforge::adapters {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_line_bytes = 1024U * 1024U;
constexpr std::size_t maximum_stream_bytes = 8U * 1024U * 1024U;
constexpr std::size_t maximum_requests = 4096;

class ControlBackend final : public backend::Backend {
 public:
  [[nodiscard]] auto start(backend::BackendRequest, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    return std::unexpected(backend::BackendError{
        backend::BackendErrorKind::unavailable,
        "plan control does not run inference", false, std::nullopt});
  }
};

enum class LineReadStatus {
  line,
  end,
  resource_exhausted,
  input_failure,
};

[[nodiscard]] auto read_bounded_line(std::istream& input, std::string& line,
                                     std::size_t& stream_bytes)
    -> LineReadStatus {
  line.clear();
  for (;;) {
    const auto value = input.get();
    if (value == std::char_traits<char>::eof()) {
      if (input.eof()) {
        return line.empty() ? LineReadStatus::end : LineReadStatus::line;
      }
      return LineReadStatus::input_failure;
    }
    if (stream_bytes == maximum_stream_bytes) {
      return LineReadStatus::resource_exhausted;
    }
    ++stream_bytes;
    if (value == '\n') return LineReadStatus::line;
    if (line.size() == maximum_line_bytes) {
      return LineReadStatus::resource_exhausted;
    }
    line.push_back(static_cast<char>(value));
  }
}

[[nodiscard]] auto failure(const cli::CommandFailureKind kind,
                           std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

template <typename Id>
[[nodiscard]] auto parse_id(const Json& value) -> std::optional<Id> {
  if (!value.is_string()) return std::nullopt;
  auto parsed = Id::from(value.get<std::string>());
  if (!parsed) return std::nullopt;
  return std::move(*parsed);
}

[[nodiscard]] auto effect_name(const domain::Effect effect)
    -> std::string_view {
  switch (effect) {
    case domain::Effect::read: return "read";
    case domain::Effect::write: return "write";
    case domain::Effect::remove: return "remove";
    case domain::Effect::execute: return "execute";
    case domain::Effect::network: return "network";
    case domain::Effect::communicate: return "communicate";
    case domain::Effect::spend: return "spend";
    case domain::Effect::change_infrastructure: return "change_infrastructure";
    case domain::Effect::change_privileges: return "change_privileges";
  }
  return "unknown";
}

[[nodiscard]] auto plan_state_name(const domain::PlanGraphState state)
    -> std::string_view {
  switch (state) {
    case domain::PlanGraphState::not_started: return "not_started";
    case domain::PlanGraphState::proposed: return "proposed";
    case domain::PlanGraphState::revision_requested:
      return "revision_requested";
    case domain::PlanGraphState::approved: return "approved";
    case domain::PlanGraphState::rejected: return "rejected";
    case domain::PlanGraphState::invalidated: return "invalidated";
  }
  return "unknown";
}

[[nodiscard]] auto session_task_state_name(
    const runtime::SessionTaskState state) -> std::string_view {
  switch (state) {
    case runtime::SessionTaskState::pending: return "pending";
    case runtime::SessionTaskState::dispatched: return "dispatched";
    case runtime::SessionTaskState::completed: return "completed";
    case runtime::SessionTaskState::failed: return "failed";
    case runtime::SessionTaskState::cancelled: return "cancelled";
    case runtime::SessionTaskState::timed_out: return "timed_out";
    case runtime::SessionTaskState::budget_exhausted: return "budget_exhausted";
    case runtime::SessionTaskState::unavailable: return "unavailable";
  }
  return "unknown";
}

[[nodiscard]] auto readiness_name(const domain::TaskReadinessState state)
    -> std::string_view {
  switch (state) {
    case domain::TaskReadinessState::ready: return "ready";
    case domain::TaskReadinessState::waiting_for_dependencies:
      return "waiting_for_dependencies";
    case domain::TaskReadinessState::blocked_by_dependency:
      return "blocked_by_dependency";
    case domain::TaskReadinessState::blocked_by_resource:
      return "blocked_by_resource";
    case domain::TaskReadinessState::waiting_for_capacity:
      return "waiting_for_capacity";
    case domain::TaskReadinessState::running: return "running";
    case domain::TaskReadinessState::completed: return "completed";
    case domain::TaskReadinessState::failed: return "failed";
  }
  return "unknown";
}

[[nodiscard]] auto source_name(
    const domain::ProjectBacklogDecisionSource source) -> std::string_view {
  return source == domain::ProjectBacklogDecisionSource::user ? "user"
                                                              : "policy";
}

[[nodiscard]] auto task_json(const domain::PlanTask& task) -> Json {
  Json dependencies = Json::array();
  for (const auto& dependency : task.dependency_task_ids) {
    dependencies.push_back(dependency.value());
  }
  Json effects = Json::array();
  for (const auto effect : task.intended_effects) {
    effects.push_back(effect_name(effect));
  }
  Json intents = Json::array();
  for (const auto& intent : task.resource_intents) {
    intents.push_back({{"effect", effect_name(intent.effect)},
                       {"kind", intent.kind},
                       {"value", intent.value}});
  }
  return {{"task_id", task.task_id.value()},
          {"parent_task_id", task.parent_task_id
                                 ? Json(task.parent_task_id->value())
                                 : Json(nullptr)},
          {"dependency_task_ids", std::move(dependencies)},
          {"title", task.title},
          {"acceptance_criteria", task.acceptance_criteria},
          {"intended_effects", std::move(effects)},
          {"resource_intents", std::move(intents)}};
}

[[nodiscard]] auto state_json(
    const runtime::PlanTaskState& state,
    const std::optional<domain::RepositoryId>& repository_id) -> Json {
  Json result{{"session_id", state.session_id.value()},
              {"repository_id",
               repository_id ? Json(repository_id->value()) : Json(nullptr)},
              {"plan_state", plan_state_name(state.plan_state)},
              {"pending_decision", nullptr},
              {"plan", nullptr},
              {"schedule", Json::array()},
              {"proposed_dispatch_task_ids", Json::array()},
              {"session_tasks", Json::array()},
              {"project_backlog", Json::array()}};
  if (state.pending_decision) {
    result["pending_decision"] = {
        {"run_id", state.pending_decision->run_id.value()},
        {"plan_id", state.pending_decision->plan_id.value()},
        {"revision_id", state.pending_decision->revision_id.value()}};
  }
  if (state.plan) {
    Json tasks = Json::array();
    for (const auto& task : state.plan->revision.tasks) {
      tasks.push_back(task_json(task));
    }
    Json evidence = Json::array();
    for (const auto& binding : state.plan->revision.evidence) {
      evidence.push_back({{"evidence_id", binding.evidence_id.value()},
                          {"digest",
                           {{"algorithm", binding.digest.algorithm},
                            {"value", binding.digest.value},
                            {"byte_size", binding.digest.byte_size}}}});
    }
    Json source = nullptr;
    if (state.plan->revision.source_snapshot) {
      const auto& snapshot = *state.plan->revision.source_snapshot;
      source = {{"repository_id", snapshot.repository_id.value()},
                {"fingerprint",
                 {{"algorithm", snapshot.fingerprint.algorithm},
                  {"value", snapshot.fingerprint.value},
                  {"byte_size", snapshot.fingerprint.byte_size}}}};
    }
    result["plan"] = {
        {"plan_id", state.plan->revision.plan_id.value()},
        {"revision_id", state.plan->revision.revision_id.value()},
        {"supersedes_revision_id",
         state.plan->revision.supersedes_revision_id
             ? Json(state.plan->revision.supersedes_revision_id->value())
             : Json(nullptr)},
        {"goal", state.plan->revision.goal},
        {"source_snapshot", std::move(source)},
        {"tasks", std::move(tasks)},
        {"evidence", std::move(evidence)}};
  }
  if (state.schedule) {
    for (const auto& task : state.schedule->tasks) {
      Json blockers = Json::array();
      for (const auto& blocker : task.blockers) {
        blockers.push_back(blocker.value());
      }
      result["schedule"].push_back({{"task_id", task.task_id.value()},
                                    {"state", readiness_name(task.state)},
                                    {"blockers", std::move(blockers)},
                                    {"next_attempt", task.next_attempt}});
    }
    for (const auto& task_id : state.schedule->dispatchable_task_ids) {
      result["proposed_dispatch_task_ids"].push_back(task_id.value());
    }
  }
  for (const auto& task : state.session_tasks) {
    result["session_tasks"].push_back(
        {{"plan_id", task.plan_id.value()},
         {"revision_id", task.revision_id.value()},
         {"task", task_json(task.task)},
         {"state", session_task_state_name(task.state)},
         {"child_run_id", task.child_run_id ? Json(task.child_run_id->value())
                                            : Json(nullptr)}});
  }
  for (const auto& item : state.project_backlog) {
    result["project_backlog"].push_back(
        {{"item_id", item.item.item_id.value()},
         {"origin_session_id", item.item.origin.session_id.value()},
         {"plan_id", item.item.origin.plan_id.value()},
         {"revision_id", item.item.origin.revision_id.value()},
         {"task", task_json(item.item.task)},
         {"status", item.status == domain::ProjectBacklogItemStatus::open
                        ? "open"
                        : "resolved"},
         {"status_event_id", item.status_event_id.value()},
         {"promotion_event_id", item.promotion_event_id.value()},
         {"source", source_name(item.item.source)},
         {"reason",
          item.status_reason ? Json(*item.status_reason) : Json(nullptr)}});
  }
  return result;
}

[[nodiscard]] auto parse_strict_json(const std::string& line)
    -> std::optional<Json> {
  try {
    std::vector<std::unordered_set<std::string>> keys;
    bool duplicate{};
    const auto callback = [&keys, &duplicate](const int,
                                              const Json::parse_event_t event,
                                              Json& value) {
      if (event == Json::parse_event_t::object_start) {
        keys.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        if (keys.empty() ||
            !keys.back().insert(value.get<std::string>()).second) {
          duplicate = true;
          return false;
        }
      } else if (event == Json::parse_event_t::object_end) {
        if (keys.empty()) return false;
        keys.pop_back();
      }
      return true;
    };
    auto parsed = Json::parse(line, callback, true, false);
    return parsed.is_discarded() || duplicate
               ? std::nullopt
               : std::optional<Json>{std::move(parsed)};
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] auto exact_keys(const Json& value,
                              const std::set<std::string_view>& allowed)
    -> bool {
  if (!value.is_object()) return false;
  return std::ranges::all_of(value.items(), [&](const auto& item) {
    return allowed.contains(item.key());
  });
}

[[nodiscard]] auto run_attributes() -> domain::RunStarted {
  return {*domain::SurfaceId::from("jsonl"), *domain::WorkspaceId::from("code"),
          *domain::PermissionProfileId::from("plan-control"), std::nullopt};
}

struct RepositoryObservation {
  std::optional<domain::RepositorySnapshot> snapshot;
  std::optional<std::string> error;
};

[[nodiscard]] auto observe_repository(const std::stop_token stop_token)
    -> RepositoryObservation {
  auto snapshot = observe_process_repository(stop_token);
  if (!snapshot) return {std::nullopt, std::move(snapshot.error())};
  return {std::move(*snapshot), std::nullopt};
}

[[nodiscard]] auto response_error(Json request_id, std::string code,
                                  std::string message,
                                  const bool retryable = false) -> Json {
  return {{"schema_version", 1},
          {"request_id", std::move(request_id)},
          {"ok", false},
          {"error",
           {{"code", std::move(code)},
            {"message", std::move(message)},
            {"retryable", retryable}}}};
}

auto write_response(std::ostream& output, const Json& response) -> bool {
  try {
    output << response.dump() << '\n';
    output.flush();
    return static_cast<bool>(output);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto controller_error_name(
    const runtime::PlanTaskControllerErrorCode code) -> std::string_view {
  switch (code) {
    case runtime::PlanTaskControllerErrorCode::invalid_request:
      return "invalid_request";
    case runtime::PlanTaskControllerErrorCode::plan_unavailable:
      return "plan_unavailable";
    case runtime::PlanTaskControllerErrorCode::repository_unavailable:
      return "repository_unavailable";
    case runtime::PlanTaskControllerErrorCode::stale_state:
      return "stale_state";
    case runtime::PlanTaskControllerErrorCode::storage_failure:
      return "storage_failure";
    case runtime::PlanTaskControllerErrorCode::runtime_failure:
      return "runtime_failure";
    case runtime::PlanTaskControllerErrorCode::resource_exhausted:
      return "resource_exhausted";
    case runtime::PlanTaskControllerErrorCode::internal_failure:
      return "internal_failure";
  }
  return "internal_failure";
}

[[nodiscard]] auto controller_response(
    const Json& request_id, const runtime::PlanTaskControllerError& error)
    -> Json {
  return response_error(request_id,
                        std::string{controller_error_name(error.code)},
                        error.message, error.retryable);
}

} // namespace

auto ProcessPlanCommand::execute(Request request,
                                 cli::CommandEnvironment& environment,
                                 std::ostream& output, std::ostream&)
    -> std::expected<void, cli::CommandFailure> {
  try {
    if (environment.input_is_terminal) {
      return failure(cli::CommandFailureKind::usage,
                     "plan --jsonl requires noninteractive standard input");
    }
    auto path = process_session_store_path();
    if (!path) {
      return failure(cli::CommandFailureKind::runtime, path.error().message);
    }
    auto store = SqliteSessionStore::open(*path);
    if (!store) {
      return failure(cli::CommandFailureKind::runtime, store.error().message);
    }
    auto session_id = request.session_id.value_or(
        *domain::SessionId::from("missing-session"));
    if (request.session_mode == SessionMode::continue_latest) {
      auto sessions = (*store)->list_sessions(1, environment.stop_token);
      if (!sessions || sessions->empty()) {
        return failure(cli::CommandFailureKind::runtime,
                       sessions ? "there is no durable session to continue"
                                : sessions.error().message);
      }
      session_id = sessions->front().session_id;
    }
    ControlBackend backend;
    auto kernel = runtime::RunKernel::open_durable(
        {session_id, runtime::DurableSessionMode::resume,
         std::chrono::floor<std::chrono::milliseconds>(
             std::chrono::system_clock::now())},
        **store, backend);
    if (!kernel) {
      return failure(cli::CommandFailureKind::runtime, kernel.error().message);
    }
    runtime::PlanTaskController controller{**kernel, store->get()};
    auto repository = observe_repository(environment.stop_token);
    auto repository_id =
        repository.snapshot
            ? std::optional{repository.snapshot->root.repository_id}
            : std::nullopt;

    std::set<std::string> request_ids;
    std::size_t request_count{};
    std::size_t stream_bytes{};
    bool protocol_failed{};
    std::string line;
    for (;;) {
      const auto line_status =
          read_bounded_line(environment.input, line, stream_bytes);
      if (line_status == LineReadStatus::end) break;
      if (line_status == LineReadStatus::input_failure) {
        return failure(cli::CommandFailureKind::runtime, "JSONL input failed");
      }
      if (line_status == LineReadStatus::resource_exhausted) {
        if (!write_response(
                output,
                response_error(nullptr, "resource_exhausted",
                               "JSONL input exceeds protocol limits"))) {
          return failure(cli::CommandFailureKind::runtime,
                         "JSONL output failed");
        }
        return failure(cli::CommandFailureKind::usage, {});
      }
      if (environment.stop_token.stop_requested()) {
        return failure(cli::CommandFailureKind::cancelled,
                       "plan control cancelled");
      }
      ++request_count;
      if (request_count > maximum_requests) {
        if (!write_response(
                output,
                response_error(nullptr, "resource_exhausted",
                               "JSONL input exceeds protocol limits"))) {
          return failure(cli::CommandFailureKind::runtime,
                         "JSONL output failed");
        }
        return failure(cli::CommandFailureKind::usage, {});
      }

      auto parsed = parse_strict_json(line);
      if (!parsed || !parsed->is_object()) {
        protocol_failed = true;
        if (!write_response(
                output, response_error(nullptr, "malformed_json",
                                       "request is not strict JSON object"))) {
          return failure(cli::CommandFailureKind::runtime,
                         "JSONL output failed");
        }
        continue;
      }
      Json request_id = parsed->contains("request_id") ? (*parsed)["request_id"]
                                                       : Json(nullptr);
      if (!parsed->contains("schema_version") ||
          (*parsed)["schema_version"] != 1 || !request_id.is_string() ||
          request_id.get_ref<const std::string&>().empty() ||
          request_id.get_ref<const std::string&>().size() > 128 ||
          !parsed->contains("operation") ||
          !(*parsed)["operation"].is_string()) {
        protocol_failed = true;
        if (!write_response(output,
                            response_error(request_id, "invalid_envelope",
                                           "request envelope is invalid"))) {
          return failure(cli::CommandFailureKind::runtime,
                         "JSONL output failed");
        }
        continue;
      }
      if (!request_ids.insert(request_id.get<std::string>()).second) {
        protocol_failed = true;
        if (!write_response(output,
                            response_error(request_id, "duplicate_request_id",
                                           "request ID is duplicated"))) {
          return failure(cli::CommandFailureKind::runtime,
                         "JSONL output failed");
        }
        continue;
      }

      const auto operation = (*parsed)["operation"].get<std::string>();
      std::optional<Json> operation_error;
      if (operation == "inspect") {
        if (!exact_keys(*parsed,
                        {"schema_version", "request_id", "operation"})) {
          operation_error = response_error(request_id, "invalid_request",
                                           "inspect has unknown fields");
        }
      } else if (operation == "decide") {
        if (!exact_keys(*parsed,
                        {"schema_version", "request_id", "operation", "plan_id",
                         "revision_id", "decision", "reason"}) ||
            !parsed->contains("plan_id") || !parsed->contains("revision_id") ||
            !parsed->contains("decision") ||
            !(*parsed)["decision"].is_string()) {
          operation_error = response_error(request_id, "invalid_request",
                                           "plan decision is malformed");
        } else {
          auto plan_id = parse_id<domain::PlanId>((*parsed)["plan_id"]);
          auto revision_id =
              parse_id<domain::PlanRevisionId>((*parsed)["revision_id"]);
          auto before = controller.inspect(repository_id);
          if (!plan_id || !revision_id || !before ||
              !before->pending_decision ||
              before->pending_decision->plan_id != *plan_id ||
              before->pending_decision->revision_id != *revision_id) {
            operation_error = response_error(
                request_id, "stale_state",
                "decision does not target the pending exact revision");
          } else {
            const auto decision_text = (*parsed)["decision"].get<std::string>();
            std::optional<domain::PlanDecision> decision;
            if (decision_text == "approved") {
              decision = domain::PlanDecision::approved;
            } else if (decision_text == "revision_requested") {
              decision = domain::PlanDecision::revision_requested;
            } else if (decision_text == "rejected") {
              decision = domain::PlanDecision::rejected;
            }
            std::optional<std::string> reason;
            if (parsed->contains("reason") && !(*parsed)["reason"].is_null()) {
              if (!(*parsed)["reason"].is_string()) {
                decision.reset();
              } else {
                reason = (*parsed)["reason"].get<std::string>();
              }
            }
            if (!decision) {
              operation_error = response_error(request_id, "invalid_request",
                                               "plan decision is invalid");
            } else {
              runtime::PlanApprovalEnvironment approval_environment;
              if (*decision == domain::PlanDecision::approved && before->plan) {
                repository = observe_repository(environment.stop_token);
                repository_id =
                    repository.snapshot
                        ? std::optional{repository.snapshot->root.repository_id}
                        : std::nullopt;
                if (before->plan->revision.source_snapshot) {
                  if (!repository.snapshot) {
                    operation_error = response_error(
                        request_id, "repository_unavailable",
                        repository.error.value_or(
                            "current repository snapshot is unavailable"));
                  } else {
                    approval_environment.source_snapshot =
                        domain::snapshot_identity(*repository.snapshot);
                  }
                }
                if (!before->plan->revision.evidence.empty()) {
                  operation_error =
                      response_error(request_id, "evidence_unavailable",
                                     "bound evidence cannot be re-established "
                                     "by this surface");
                }
              }
              if (!operation_error) {
                auto decided = controller.decide(
                    before->pending_decision->run_id,
                    {*plan_id, *revision_id, *decision,
                     domain::PlanDecisionSource::user, std::move(reason)},
                    std::move(approval_environment));
                if (!decided) {
                  operation_error =
                      controller_response(request_id, decided.error());
                }
              }
            }
          }
        }
      } else if (operation == "promote") {
        repository = observe_repository(environment.stop_token);
        repository_id =
            repository.snapshot
                ? std::optional{repository.snapshot->root.repository_id}
                : std::nullopt;
        const std::set<std::string_view> keys{
            "schema_version", "request_id", "operation",   "run_id", "item_id",
            "repository_id",  "plan_id",    "revision_id", "task_id"};
        auto run_id = parsed->contains("run_id")
                          ? parse_id<domain::RunId>((*parsed)["run_id"])
                          : std::nullopt;
        auto item_id =
            parsed->contains("item_id")
                ? parse_id<domain::ProjectBacklogItemId>((*parsed)["item_id"])
                : std::nullopt;
        auto requested_repository =
            parsed->contains("repository_id")
                ? parse_id<domain::RepositoryId>((*parsed)["repository_id"])
                : std::nullopt;
        auto plan_id = parsed->contains("plan_id")
                           ? parse_id<domain::PlanId>((*parsed)["plan_id"])
                           : std::nullopt;
        auto revision_id =
            parsed->contains("revision_id")
                ? parse_id<domain::PlanRevisionId>((*parsed)["revision_id"])
                : std::nullopt;
        auto task_id = parsed->contains("task_id")
                           ? parse_id<domain::PlanTaskId>((*parsed)["task_id"])
                           : std::nullopt;
        auto before = controller.inspect(repository_id);
        const runtime::ActiveSessionTask* task{};
        if (before && plan_id && revision_id && task_id) {
          const auto found = std::ranges::find_if(
              before->session_tasks, [&](const auto& candidate) {
                return candidate.plan_id == *plan_id &&
                       candidate.revision_id == *revision_id &&
                       candidate.task.task_id == *task_id;
              });
          if (found != before->session_tasks.end()) task = &*found;
        }
        if (!exact_keys(*parsed, keys) || !run_id || !item_id ||
            !requested_repository || !plan_id || !revision_id || !task_id ||
            !repository_id || *requested_repository != *repository_id ||
            task == nullptr) {
          operation_error =
              response_error(request_id, "invalid_request",
                             "promotion must target an exact unresolved task "
                             "in the current repository");
        } else {
          auto promoted = controller.promote(
              {*run_id,
               run_attributes(),
               {*item_id,
                *requested_repository,
                {session_id, *plan_id, *revision_id, *task_id},
                task->task,
                domain::ProjectBacklogDecisionSource::user}});
          if (!promoted) {
            operation_error = controller_response(request_id, promoted.error());
          }
        }
      } else if (operation == "set_backlog_status") {
        const std::set<std::string_view> keys{
            "schema_version", "request_id", "operation",
            "run_id",         "item_id",    "repository_id",
            "status",         "reason",     "expected_status_event_id"};
        auto run_id = parsed->contains("run_id")
                          ? parse_id<domain::RunId>((*parsed)["run_id"])
                          : std::nullopt;
        auto item_id =
            parsed->contains("item_id")
                ? parse_id<domain::ProjectBacklogItemId>((*parsed)["item_id"])
                : std::nullopt;
        auto requested_repository =
            parsed->contains("repository_id")
                ? parse_id<domain::RepositoryId>((*parsed)["repository_id"])
                : std::nullopt;
        auto expected = parsed->contains("expected_status_event_id")
                            ? parse_id<domain::EventId>(
                                  (*parsed)["expected_status_event_id"])
                            : std::nullopt;
        std::optional<std::string> reason;
        const auto reason_valid = !parsed->contains("reason") ||
                                  (*parsed)["reason"].is_null() ||
                                  (*parsed)["reason"].is_string();
        if (parsed->contains("reason") && (*parsed)["reason"].is_string()) {
          reason = (*parsed)["reason"].get<std::string>();
        }
        std::optional<domain::ProjectBacklogItemStatus> status;
        if (parsed->contains("status") && (*parsed)["status"].is_string()) {
          const auto text = (*parsed)["status"].get<std::string>();
          if (text == "open") status = domain::ProjectBacklogItemStatus::open;
          if (text == "resolved") {
            status = domain::ProjectBacklogItemStatus::resolved;
          }
        }
        if (!exact_keys(*parsed, keys) || !run_id || !item_id ||
            !requested_repository || !repository_id ||
            *requested_repository != *repository_id || !expected || !status ||
            !reason_valid) {
          operation_error =
              response_error(request_id, "invalid_request",
                             "project-backlog status request is malformed or "
                             "targets another repository");
        } else {
          auto changed = controller.set_backlog_status(
              {*run_id,
               run_attributes(),
               {*item_id, *requested_repository, *status,
                domain::ProjectBacklogDecisionSource::user, std::move(reason),
                *expected}});
          if (!changed) {
            operation_error = controller_response(request_id, changed.error());
          }
        }
      } else {
        operation_error = response_error(request_id, "unknown_operation",
                                         "JSONL operation is unknown");
      }

      if (operation_error) {
        protocol_failed = true;
        if (!write_response(output, *operation_error)) {
          return failure(cli::CommandFailureKind::runtime,
                         "JSONL output failed");
        }
        continue;
      }
      auto state = controller.inspect(repository_id);
      if (!state) {
        protocol_failed = true;
        if (!write_response(output,
                            controller_response(request_id, state.error()))) {
          return failure(cli::CommandFailureKind::runtime,
                         "JSONL output failed");
        }
        continue;
      }
      if (!write_response(output,
                          {{"schema_version", 1},
                           {"request_id", request_id},
                           {"ok", true},
                           {"state", state_json(*state, repository_id)}})) {
        return failure(cli::CommandFailureKind::runtime, "JSONL output failed");
      }
    }

    if (request_count == 0) {
      return failure(cli::CommandFailureKind::usage,
                     "plan --jsonl received no requests");
    }
    return protocol_failed ? failure(cli::CommandFailureKind::usage, {})
                           : std::expected<void, cli::CommandFailure>{};
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "plan JSONL control failed internally");
  }
}

} // namespace aiforge::adapters
