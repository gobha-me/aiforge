#include <aiforge/runtime/run_kernel.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace aiforge::runtime {
namespace {

struct BackendFailure {
  backend::BackendError error;
};
struct BackendEnded {};
struct ToolUpdate {
  domain::InvocationId invocation_id;
  ToolExecutionEvent event;
};
struct ToolFailure {
  domain::InvocationId invocation_id;
  ToolExecutionError error;
};
struct ToolEnded {
  domain::InvocationId invocation_id;
};
struct ToolDeadlineExpired {
  domain::InvocationId invocation_id;
};

using WorkerUpdate =
    std::variant<backend::BackendEvent, BackendFailure, BackendEnded,
                 ToolUpdate, ToolFailure, ToolEnded, ToolDeadlineExpired>;

[[nodiscard]] auto default_timestamp() -> domain::EventTimestamp {
  return std::chrono::floor<std::chrono::milliseconds>(
      std::chrono::system_clock::now());
}

[[nodiscard]] auto kernel_error(const RunKernelErrorCode code,
                                std::string message,
                                const bool retryable = false)
    -> RunKernelError {
  return RunKernelError{code, std::move(message), retryable};
}

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto effects_are_unique(
    const std::vector<domain::Effect>& effects) -> bool {
  std::set<domain::Effect> unique;
  return std::ranges::all_of(
      effects, [&](const auto effect) { return unique.insert(effect).second; });
}

[[nodiscard]] auto effects_are_declared(
    const std::vector<domain::Effect>& requested,
    const std::vector<domain::Effect>& declared) -> bool {
  if (requested.empty()) return declared.empty();
  return effects_are_unique(requested) &&
         std::ranges::all_of(requested, [&](const auto effect) {
           return std::ranges::find(declared, effect) != declared.end();
         });
}

[[nodiscard]] auto backend_domain_error(const backend::BackendError& error)
    -> domain::DomainError {
  switch (error.kind) {
    case backend::BackendErrorKind::cancelled:
      return {domain::ErrorCode::cancelled, "backend request cancelled", false};
    case backend::BackendErrorKind::unavailable:
    case backend::BackendErrorKind::network:
    case backend::BackendErrorKind::rate_limited:
      return {domain::ErrorCode::unavailable, "backend unavailable",
              error.retryable};
    case backend::BackendErrorKind::authentication:
      return {domain::ErrorCode::backend, "backend authentication failed",
              false};
    case backend::BackendErrorKind::credential_unavailable:
      return {domain::ErrorCode::backend,
              "backend credential is not configured", false};
    case backend::BackendErrorKind::request_rejected:
      return {domain::ErrorCode::backend, "backend rejected the request",
              false};
    case backend::BackendErrorKind::protocol:
    case backend::BackendErrorKind::script_mismatch:
    case backend::BackendErrorKind::script_exhausted:
      return {domain::ErrorCode::backend, "backend protocol failure",
              error.retryable};
  }
  return {domain::ErrorCode::backend, "backend failure", false};
}

[[nodiscard]] auto tool_domain_error(const ToolExecutionError& error)
    -> domain::DomainError {
  switch (error.code) {
    case ToolExecutionErrorCode::invalid_arguments:
      return {domain::ErrorCode::invalid_event, "tool arguments are invalid",
              false};
    case ToolExecutionErrorCode::unavailable:
      return {domain::ErrorCode::unavailable, "tool executor unavailable",
              error.retryable};
    case ToolExecutionErrorCode::cancelled:
      return {domain::ErrorCode::cancelled, "tool execution cancelled", false};
    case ToolExecutionErrorCode::timed_out:
      return {domain::ErrorCode::cancelled, "tool execution timed out", false};
    case ToolExecutionErrorCode::output_limit:
      return {domain::ErrorCode::invalid_state, "tool output limit exceeded",
              false};
    case ToolExecutionErrorCode::protocol_failure:
      return {domain::ErrorCode::invalid_state,
              "tool executor protocol failure", false};
    case ToolExecutionErrorCode::internal_failure:
      return {domain::ErrorCode::unavailable,
              "tool execution failed internally", false};
  }
  return {domain::ErrorCode::unavailable, "tool execution failed", false};
}

[[nodiscard]] auto protocol_domain_error() -> domain::DomainError {
  return {domain::ErrorCode::invalid_state, "backend stream protocol failure",
          false};
}

[[nodiscard]] auto policy_denied_error() -> domain::DomainError {
  return {domain::ErrorCode::policy, "tool invocation denied", false};
}

[[nodiscard]] auto approval_cancelled_error() -> domain::DomainError {
  return {domain::ErrorCode::cancelled, "tool approval cancelled", false};
}

[[nodiscard]] auto policy_failure_error(const ToolPolicyError& error)
    -> domain::DomainError {
  return {error.code == ToolPolicyErrorCode::scope_widening
              ? domain::ErrorCode::policy
              : domain::ErrorCode::unavailable,
          error.code == ToolPolicyErrorCode::persistence_failure
              ? "tool policy persistence failed"
              : "tool policy evaluation failed",
          error.retryable};
}

[[nodiscard]] auto checked_add(std::size_t& total, const std::size_t value)
    -> bool {
  if (value > std::numeric_limits<std::size_t>::max() - total) return false;
  total += value;
  return true;
}

[[nodiscard]] auto content_bytes(
    const std::vector<domain::ContentBlock>& content)
    -> std::optional<std::size_t> {
  std::size_t total{};
  for (const auto& block : content) {
    const auto size = std::visit(
        [](const auto& value) -> std::size_t {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<Value, domain::TextBlock>) {
            return value.text.size();
          } else if constexpr (std::same_as<Value,
                                            domain::StructuredDataBlock>) {
            return value.media_type.size() + value.data.size();
          } else if constexpr (std::same_as<Value, domain::CitationBlock>) {
            return value.uri.size() + (value.title ? value.title->size() : 0U);
          } else if constexpr (std::same_as<Value,
                                            domain::ArtifactReferenceBlock>) {
            return value.artifact_id.value().size() +
                   (value.label ? value.label->size() : 0U);
          } else {
            return value.type_name.size();
          }
        },
        block);
    if (!checked_add(total, size)) return std::nullopt;
  }
  return total;
}

[[nodiscard]] auto valid_created_artifacts(
    const std::vector<domain::ArtifactMetadata>& artifacts,
    const std::vector<domain::ContentBlock>& content,
    const domain::InvocationId& invocation_id,
    const domain::SessionEventLog& event_log) -> bool {
  constexpr std::size_t maximum_artifacts{16};
  if (artifacts.size() > maximum_artifacts) return false;

  std::set<domain::ArtifactId> existing;
  for (const auto& event : event_log.events()) {
    if (const auto* created =
            std::get_if<domain::ArtifactCreated>(&event.payload)) {
      existing.insert(created->artifact.artifact_id);
    }
  }

  std::set<domain::ArtifactId> created_ids;
  for (const auto& artifact : artifacts) {
    const bool dimensions_match = artifact.width.has_value() ==
                                  artifact.height.has_value();
    if (!artifact.producing_invocation_id ||
        *artifact.producing_invocation_id != invocation_id ||
        artifact.media_type.empty() || artifact.media_type.size() > 255 ||
        has_control_character(artifact.media_type) || artifact.byte_size == 0 ||
        artifact.digest.empty() || artifact.digest.size() > 512 ||
        has_control_character(artifact.digest) || !dimensions_match ||
        (artifact.width && (*artifact.width == 0 || *artifact.height == 0)) ||
        existing.contains(artifact.artifact_id) ||
        !created_ids.insert(artifact.artifact_id).second) {
      return false;
    }
  }

  std::set<domain::ArtifactId> referenced;
  for (const auto& block : content) {
    if (const auto* reference =
            std::get_if<domain::ArtifactReferenceBlock>(&block)) {
      referenced.insert(reference->artifact_id);
    }
  }
  return std::ranges::all_of(created_ids, [&](const auto& id) {
    return referenced.contains(id);
  });
}

[[nodiscard]] auto valid_question_definitions(
    const std::vector<domain::QuestionDefinition>& questions) -> bool {
  if (questions.empty() || questions.size() > 3) return false;
  std::set<domain::QuestionId> ids;
  for (const auto& question : questions) {
    const auto available =
        question.options.size() + (question.other ? 1U : 0U);
    if (!ids.insert(question.question_id).second || question.prompt.empty() ||
        question.prompt.size() > 1024 ||
        has_control_character(question.prompt) || question.options.empty() ||
        question.options.size() > 8 ||
        question.minimum_selections > available ||
        !question.maximum_selections ||
        *question.maximum_selections < question.minimum_selections ||
        *question.maximum_selections > available ||
        (question.required && question.minimum_selections == 0) ||
        (!question.required && question.minimum_selections != 0) ||
        (question.selection == domain::QuestionSelection::one &&
         *question.maximum_selections != 1)) {
      return false;
    }
    std::set<std::string> option_ids;
    std::size_t recommended{};
    for (const auto& option : question.options) {
      if (option.option_id.empty() || option.option_id.size() > 128 ||
          has_control_character(option.option_id) || option.label.empty() ||
          option.label.size() > 256 || has_control_character(option.label) ||
          (option.description &&
           (option.description->size() > 1024 ||
            has_control_character(*option.description))) ||
          !option_ids.insert(option.option_id).second) {
        return false;
      }
      if (option.recommended) ++recommended;
    }
    if (recommended > *question.maximum_selections) return false;
    if (question.other &&
        (question.other->label.empty() ||
         question.other->label.size() > 256 ||
         has_control_character(question.other->label) ||
         question.other->maximum_bytes == 0 ||
         (question.other->placeholder &&
          (question.other->placeholder->size() > 1024 ||
           has_control_character(*question.other->placeholder))))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto valid_question_answers(
    const std::vector<domain::QuestionDefinition>& questions,
    const std::vector<domain::QuestionAnswer>& answers) -> bool {
  if (answers.size() != questions.size()) return false;
  std::set<domain::QuestionId> answered_ids;
  for (const auto& answer : answers) {
    if (!answered_ids.insert(answer.question_id).second) return false;
    const auto found = std::ranges::find(
        questions, answer.question_id,
        &domain::QuestionDefinition::question_id);
    if (found == questions.end()) return false;
    if (answer.free_form &&
        (answer.free_form->empty() || !found->other ||
         answer.free_form->size() > found->other->maximum_bytes ||
         has_control_character(*answer.free_form))) {
      return false;
    }
    std::set<std::string> selected;
    for (const auto& id : answer.selected_option_ids) {
      if (!selected.insert(id).second ||
          std::ranges::none_of(found->options, [&](const auto& option) {
            return option.option_id == id;
          })) {
        return false;
      }
    }
    const auto count = answer.selected_option_ids.size() +
                       static_cast<std::size_t>(answer.free_form.has_value());
    if (found->required && count == 0) return false;
    if (!found->required && count == 0) continue;
    if (count < found->minimum_selections ||
        (found->maximum_selections && count > *found->maximum_selections) ||
        (found->selection == domain::QuestionSelection::one && count > 1)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto answered_content(
    const std::vector<domain::QuestionAnswer>& answers)
    -> std::vector<domain::ContentBlock> {
  auto values = nlohmann::json::array();
  for (const auto& answer : answers) {
    values.push_back(
        {{"question_id", std::string{answer.question_id.value()}},
         {"selected_option_ids", answer.selected_option_ids},
         {"other", answer.free_form ? nlohmann::json(*answer.free_form)
                                     : nlohmann::json(nullptr)}});
  }
  return {domain::StructuredDataBlock{
      "application/json",
      nlohmann::json{{"status", "answered"}, {"answers", std::move(values)}}
          .dump()}};
}

[[nodiscard]] auto cancelled_content()
    -> std::vector<domain::ContentBlock> {
  return {domain::StructuredDataBlock{
      "application/json", nlohmann::json{{"status", "cancelled"}}.dump()}};
}

[[nodiscard]] auto scopes_are_unique(
    const std::vector<domain::CapabilityScope>& scopes) -> bool {
  for (auto current = scopes.begin(); current != scopes.end(); ++current) {
    if (std::find(std::next(current), scopes.end(), *current) != scopes.end()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto valid_policy_decision(
    const domain::PolicyDecision decision) -> bool {
  switch (decision) {
    case domain::PolicyDecision::allow:
    case domain::PolicyDecision::deny:
    case domain::PolicyDecision::require_approval: return true;
  }
  return false;
}

[[nodiscard]] auto valid_approval_decision(
    const domain::ApprovalDecision decision) -> bool {
  switch (decision) {
    case domain::ApprovalDecision::approved:
    case domain::ApprovalDecision::denied:
    case domain::ApprovalDecision::cancelled: return true;
  }
  return false;
}

[[nodiscard]] auto valid_approval_lifetime(
    const domain::ApprovalGrantLifetime lifetime) -> bool {
  switch (lifetime) {
    case domain::ApprovalGrantLifetime::invocation:
    case domain::ApprovalGrantLifetime::session:
    case domain::ApprovalGrantLifetime::saved: return true;
  }
  return false;
}

[[nodiscard]] auto tool_invocation_id(
    const domain::RunEventPayload& payload)
    -> std::optional<domain::InvocationId> {
  return std::visit(
      [](const auto& value) -> std::optional<domain::InvocationId> {
        if constexpr (requires { value.invocation_id; }) {
          return value.invocation_id;
        }
        return std::nullopt;
      },
      payload);
}

}  // namespace

struct RunKernel::Impl {
  struct ToolAssembly {
    std::string name;
    std::string arguments;
  };

  enum class InvocationState {
    proposed,
    awaiting_approval,
    allowed,
    running,
    awaiting_input,
    terminal,
  };

  struct PendingInvocation {
    domain::InvocationId invocation_id;
    std::optional<domain::InvocationId> parent_invocation_id;
    backend::ToolDeclaration declaration;
    ValidatedToolArguments arguments;
    ToolExecutionLimits limits;
    std::shared_ptr<ToolExecutor> executor;
    domain::MessageId result_message_id;
    std::vector<domain::Effect> requested_effects;
    std::vector<domain::CapabilityScope> requested_scopes;
    std::vector<domain::CapabilityScope> granted_scopes;
    std::optional<ToolPolicyRequest> policy_request;
    InvocationState state{InvocationState::proposed};
    std::size_t output_bytes{};
    std::size_t progress_events{};
    bool terminal_event_seen{};
    std::vector<domain::QuestionDefinition> questions;
  };

  struct ActiveRun {
    domain::RunId run_id;
    domain::PermissionProfileId permission_profile_id;
    std::optional<domain::InferenceId> inference_id;
    std::optional<domain::MessageId> assistant_message_id;
    std::map<domain::InvocationId, ToolAssembly> tool_calls;
    std::vector<domain::InvocationId> tool_call_order;
    std::map<domain::InvocationId, PendingInvocation> invocations;
    std::vector<domain::InvocationId> invocation_order;
    std::optional<domain::InvocationId> active_tool_id;
    bool response_started{};
    bool backend_terminal_seen{};
    bool tool_call_finish{};
    bool cancellation_ack_expected{};
    bool cancel_requested{};
    bool run_terminal{};
    std::optional<std::string> cancel_reason;
  };

  struct Transaction {
    domain::SessionEventLog event_log;
    std::map<domain::RunId, domain::RunProjection> projections;
    std::set<domain::InvocationId> used_invocation_ids;
    std::optional<ActiveRun> active;
    std::vector<domain::RunEvent> events;
  };

  Impl(domain::SessionId session_id, backend::Backend& backend,
       RunWakeSink* wake_sink, TimestampSource timestamp_source,
       RunKernelLimits limits, ToolRegistrySnapshot tool_snapshot,
       std::shared_ptr<ToolPolicy> tool_policy)
      : event_log(std::move(session_id)), backend_port(backend),
        wake(wake_sink),
        timestamp(timestamp_source ? std::move(timestamp_source)
                                   : TimestampSource{default_timestamp}),
        limits(limits), tools(std::move(tool_snapshot)),
        policy(tool_policy ? std::move(tool_policy) : default_tool_policy()) {}

  ~Impl() {
    {
      std::lock_guard lock(queue_mutex);
      queue_closed = true;
    }
    queue_space.notify_all();
    backend_worker.request_stop();
    if (operation_stop) operation_stop->request_stop();
    deadline_worker.request_stop();
    tool_worker.request_stop();
    if (backend_worker.joinable()) backend_worker.join();
    if (deadline_worker.joinable()) deadline_worker.join();
    if (tool_worker.joinable()) tool_worker.join();
  }

  [[nodiscard]] auto transaction() const -> Transaction {
    return {event_log, projections, used_invocation_ids, active, {}};
  }

  [[nodiscard]] auto valid_limits() const noexcept -> bool {
    return limits.pending_updates != 0 && limits.tool_argument_bytes != 0;
  }

  auto stop_workers() -> void {
    backend_worker.request_stop();
    if (operation_stop) operation_stop->request_stop();
    deadline_worker.request_stop();
    tool_worker.request_stop();
  }

  [[nodiscard]] auto commit(Transaction transaction)
      -> std::expected<void, RunKernelError> {
    if (session_store != nullptr && !transaction.events.empty()) {
      auto appended = session_store->append_events(
          transaction.event_log.session_id(), transaction.events);
      if (!appended) {
        {
          std::lock_guard lock(queue_mutex);
          queue_closed = true;
        }
        queue_space.notify_all();
        stop_workers();
        active.reset();
        unusable = true;
        return std::unexpected(kernel_error(RunKernelErrorCode::storage_failure,
                                            "session event persistence failed",
                                            appended.error().retryable));
      }
    }
    event_log = std::move(transaction.event_log);
    projections = std::move(transaction.projections);
    used_invocation_ids = std::move(transaction.used_invocation_ids);
    active = std::move(transaction.active);
    return {};
  }

  auto push(WorkerUpdate update) -> bool {
    bool should_wake{};
    {
      std::unique_lock lock(queue_mutex);
      queue_space.wait(lock, [&] {
        return queue_closed || queue.size() < limits.pending_updates;
      });
      if (queue_closed) return false;
      should_wake = queue.empty();
      queue.push_back(std::move(update));
    }
    if (should_wake && wake != nullptr) wake->wake();
    return true;
  }

  auto run_backend(backend::BackendRequest request,
                   const std::stop_token stop_token) -> void {
    auto started = backend_port.start(std::move(request), stop_token);
    if (!started) {
      static_cast<void>(push(BackendFailure{std::move(started.error())}));
      static_cast<void>(push(BackendEnded{}));
      return;
    }
    for (;;) {
      auto next = (*started)->next(stop_token);
      if (!next) {
        static_cast<void>(push(BackendFailure{std::move(next.error())}));
        static_cast<void>(push(BackendEnded{}));
        return;
      }
      if (!*next) {
        static_cast<void>(push(BackendEnded{}));
        return;
      }
      if (!push(std::move(**next))) return;
    }
  }

  auto run_tool(const PendingInvocation invocation,
                const std::stop_token stop_token) -> void {
    auto started = invocation.executor->start(
        ToolInvocation{invocation.invocation_id,
                       invocation.parent_invocation_id,
                       invocation.declaration.name, invocation.arguments,
                       invocation.granted_scopes, invocation.limits},
        stop_token);
    if (!started) {
      static_cast<void>(push(
          ToolFailure{invocation.invocation_id, std::move(started.error())}));
      static_cast<void>(push(ToolEnded{invocation.invocation_id}));
      return;
    }
    for (;;) {
      auto next = (*started)->next(stop_token);
      if (!next) {
        static_cast<void>(push(
            ToolFailure{invocation.invocation_id, std::move(next.error())}));
        static_cast<void>(push(ToolEnded{invocation.invocation_id}));
        return;
      }
      if (!*next) {
        static_cast<void>(push(ToolEnded{invocation.invocation_id}));
        return;
      }
      if (!push(ToolUpdate{invocation.invocation_id, std::move(**next)})) {
        return;
      }
    }
  }

  [[nodiscard]] auto launch_backend(backend::BackendRequest request)
      -> std::expected<void, RunKernelError> {
    if (backend_worker.joinable()) backend_worker.join();
    try {
      backend_worker =
          std::jthread([impl = this, request = std::move(request)](
                           const std::stop_token stop_token) mutable {
            try {
              impl->run_backend(std::move(request), stop_token);
            } catch (...) {
              try {
                static_cast<void>(
                    impl->push(BackendFailure{backend::BackendError{
                        backend::BackendErrorKind::unavailable,
                        "backend worker failed internally", false,
                        std::nullopt}}));
                static_cast<void>(impl->push(BackendEnded{}));
              } catch (...) {
              }
            }
          });
      return {};
    } catch (...) {
      return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                          "could not start backend worker"));
    }
  }

  [[nodiscard]] auto make_event(
      const Transaction& transaction, const domain::RunId& run_id,
      domain::RunEventPayload payload,
      std::optional<domain::InvocationId> invocation_id = std::nullopt)
      -> std::expected<domain::RunEvent, RunKernelError> {
    if (transaction.event_log.last_sequence() ==
        std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::event_sequence_overflow,
                       "session event sequence overflow"));
    }
    const auto sequence = transaction.event_log.last_sequence() + 1;
    auto event_id = domain::EventId::from("event-" + std::to_string(sequence));
    if (!event_id) {
      return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                          "could not create an event ID"));
    }
    return domain::RunEvent{domain::EventMetadata{std::move(*event_id), run_id,
                                                  sequence, 1, timestamp(),
                                                  std::nullopt, std::nullopt,
                                                  std::move(invocation_id)},
                            std::move(payload)};
  }

  [[nodiscard]] auto make_result_message_id(const Transaction& transaction)
      -> std::expected<domain::MessageId, RunKernelError> {
    if (transaction.event_log.last_sequence() ==
        std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::event_sequence_overflow,
                       "session event sequence overflow"));
    }
    auto id = domain::MessageId::from(
        "tool-message-" +
        std::to_string(transaction.event_log.last_sequence() + 1));
    if (!id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::internal_failure,
                       "could not create a tool result message ID"));
    }
    return *id;
  }

  [[nodiscard]] auto record(
      const domain::RunId& run_id, domain::RunEventPayload payload,
      Transaction& transaction,
      std::optional<domain::InvocationId> invocation_id = std::nullopt)
      -> std::expected<void, RunKernelError> {
    auto event = make_event(transaction, run_id, std::move(payload),
                            std::move(invocation_id));
    if (!event) return std::unexpected(std::move(event.error()));

    auto projection_candidate = transaction.projections.contains(run_id)
                                    ? transaction.projections.at(run_id)
                                    : domain::RunProjection{};
    if (!projection_candidate.apply(*event)) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::projection_rejected,
                       "run projection rejected a generated event"));
    }
    if (!transaction.event_log.append(*event)) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::event_log_rejected,
                       "session event log rejected a generated event"));
    }
    transaction.projections.insert_or_assign(run_id,
                                             std::move(projection_candidate));
    transaction.events.push_back(std::move(*event));
    return {};
  }

  [[nodiscard]] auto fail_live_run(Transaction& transaction,
                                   domain::DomainError error)
      -> std::expected<void, RunKernelError> {
    if (!transaction.active || transaction.active->run_terminal) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "event followed a terminal run event"));
    }
    const auto run_id = transaction.active->run_id;
    const auto projection =
        transaction.projections.find(transaction.active->run_id);
    if (transaction.active->inference_id &&
        projection != transaction.projections.end() &&
        projection->second.active_inference_id() ==
            transaction.active->inference_id) {
      if (auto result = record(
              run_id,
              domain::InferenceFailed{*transaction.active->inference_id, error},
              transaction);
          !result) {
        return result;
      }
    }
    if (auto result =
            record(run_id, domain::RunFailed{std::move(error)}, transaction);
        !result) {
      return result;
    }
    transaction.active->backend_terminal_seen = true;
    transaction.active->cancellation_ack_expected = true;
    transaction.active->run_terminal = true;
    if (transaction.active->active_tool_id) {
      auto& invocation = transaction.active->invocations.at(
          *transaction.active->active_tool_id);
      invocation.state = InvocationState::terminal;
      invocation.terminal_event_seen = true;
    }
    stop_workers();
    return {};
  }

  [[nodiscard]] auto record_tool_error(Transaction& transaction,
                                       PendingInvocation& invocation,
                                       domain::DomainError error)
      -> std::expected<void, RunKernelError> {
    if (invocation.terminal_event_seen) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "tool invocation is already terminal"));
    }
    if (auto result = record(transaction.active->run_id,
                             domain::ToolErrored{invocation.invocation_id,
                                                 std::move(error),
                                                 invocation.result_message_id},
                             transaction, invocation.invocation_id);
        !result) {
      return result;
    }
    invocation.state = InvocationState::terminal;
    invocation.terminal_event_seen = true;
    return {};
  }

  [[nodiscard]] auto process_backend_event(backend::BackendEvent event,
                                           Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active || !active->inference_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::no_active_run,
                       "backend event has no active inference"));
    }
    if (active->run_terminal) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "backend event followed a terminal run event"));
    }
    if (active->backend_terminal_seen) {
      if (active->cancellation_ack_expected &&
          std::holds_alternative<backend::ResponseCancelled>(event)) {
        active->cancellation_ack_expected = false;
        return {};
      }
      return fail_live_run(transaction, protocol_domain_error());
    }

    const auto record_or_fail =
        [&](domain::RunEventPayload payload,
            std::optional<domain::InvocationId> invocation =
                std::nullopt) -> std::expected<void, RunKernelError> {
      auto recorded = record(active->run_id, std::move(payload), transaction,
                             std::move(invocation));
      if (recorded) return {};
      return fail_live_run(transaction, protocol_domain_error());
    };

    return std::visit(
        [&](auto&& value) -> std::expected<void, RunKernelError> {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<Value, backend::ResponseStarted>) {
            if (active->response_started || value.response_id.size() > 4096 ||
                has_control_character(value.response_id)) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            active->response_started = true;
            return record_or_fail(domain::AssistantContentStarted{
                *active->assistant_message_id, *active->inference_id});
          } else if constexpr (std::same_as<Value, backend::ContentDelta>) {
            if (!active->response_started ||
                value.message_id != *active->assistant_message_id) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(domain::AssistantContentDeltaAdded{
                value.message_id, *active->inference_id,
                std::move(value.delta)});
          } else if constexpr (std::same_as<Value, backend::ReasoningDelta>) {
            if (!active->response_started) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(domain::ReasoningMetadataAdded{
                *active->inference_id, std::move(value.text),
                std::move(value.metadata)});
          } else if constexpr (std::same_as<Value, backend::CitationObserved>) {
            if (!active->response_started) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(domain::AssistantContentDeltaAdded{
                *active->assistant_message_id, *active->inference_id,
                std::move(value.citation)});
          } else if constexpr (std::same_as<Value, backend::UsageObserved>) {
            if (!active->response_started) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(
                domain::UsageRecorded{*active->inference_id, value.usage});
          } else if constexpr (std::same_as<Value, backend::ToolCallDelta>) {
            if (!active->response_started || value.tool_name.size() > 256 ||
                has_control_character(value.tool_name)) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            auto found = active->tool_calls.find(value.invocation_id);
            if (found == active->tool_calls.end()) {
              if (value.tool_name.empty() ||
                  tools.find(value.tool_name) == nullptr ||
                  transaction.used_invocation_ids.contains(
                      value.invocation_id)) {
                return fail_live_run(transaction, protocol_domain_error());
              }
              found = active->tool_calls
                          .emplace(value.invocation_id,
                                   ToolAssembly{value.tool_name, {}})
                          .first;
              active->tool_call_order.push_back(value.invocation_id);
            } else if (!value.tool_name.empty() &&
                       found->second.name != value.tool_name) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            if (found->second.arguments.size() > limits.tool_argument_bytes ||
                value.arguments_fragment.size() >
                    limits.tool_argument_bytes -
                        found->second.arguments.size()) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            found->second.arguments.append(value.arguments_fragment);
            return {};
          } else if constexpr (std::same_as<Value, backend::ResponseFinished>) {
            const bool has_tools = !active->tool_calls.empty();
            if (!active->response_started ||
                (value.reason == domain::FinishReason::tool_call) !=
                    has_tools) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            if (has_tools) {
              for (const auto& invocation_id : active->tool_call_order) {
                auto& assembly = active->tool_calls.at(invocation_id);
                const auto* registration = tools.find(assembly.name);
                if (registration == nullptr ||
                    !transaction.used_invocation_ids.insert(invocation_id)
                         .second) {
                  return fail_live_run(transaction, protocol_domain_error());
                }
                auto message_id = make_result_message_id(transaction);
                if (!message_id) return std::unexpected(message_id.error());
                const domain::StructuredDataBlock raw_arguments{
                    "application/json", assembly.arguments};
                std::expected<ValidatedToolArguments, ToolExecutionError>
                    validated = std::unexpected(ToolExecutionError{
                        ToolExecutionErrorCode::internal_failure,
                        "tool validation failed internally", false});
                try {
                  validated =
                      registration->executor->validate(raw_arguments);
                } catch (...) {
                }
                if (validated &&
                    (validated->value.media_type != "application/json" ||
                     validated->value.data.empty() ||
                     validated->value.data.size() >
                         limits.tool_argument_bytes ||
                     (!validated->required_effects.empty() &&
                      !effects_are_declared(
                          validated->required_effects,
                          registration->declaration.effects)) ||
                     (!validated->required_scopes.empty() &&
                      std::ranges::any_of(
                          validated->required_scopes, [&](const auto& scope) {
                            return std::ranges::none_of(
                                registration->declaration.capability_scopes,
                                [&](const auto& declared) {
                                  return capability_scope_covers(declared,
                                                                 scope);
                                });
                          })) ||
                     (!validated->required_scopes.empty() &&
                      std::ranges::any_of(
                          validated->required_scopes, [&](const auto& scope) {
                            const auto& effects =
                                validated->required_effects.empty()
                                    ? registration->declaration.effects
                                    : validated->required_effects;
                            return std::ranges::find(effects, scope.effect) ==
                                   effects.end();
                          })))) {
                  validated = std::unexpected(ToolExecutionError{
                      ToolExecutionErrorCode::protocol_failure,
                      "tool validator returned invalid normalized arguments",
                      false});
                }
                auto required_scopes =
                    validated
                        ? (validated->required_scopes.empty()
                               ? registration->declaration.capability_scopes
                               : validated->required_scopes)
                        : std::vector<domain::CapabilityScope>{};
                auto required_effects =
                    validated
                        ? (validated->required_effects.empty()
                               ? registration->declaration.effects
                               : validated->required_effects)
                        : std::vector<domain::Effect>{};
                if (auto result = record_or_fail(
                        domain::ToolProposed{
                            invocation_id, assembly.name, raw_arguments,
                            required_effects, std::nullopt,
                            validated && validated->value == raw_arguments,
                            validated
                                ? validated->required_scopes
                                : std::vector<domain::CapabilityScope>{},
                            required_scopes, *message_id},
                        invocation_id);
                    !result) {
                  return result;
                }
                if (!validated) {
                  PendingInvocation failed{
                      invocation_id,
                      std::nullopt,
                      registration->declaration,
                      ValidatedToolArguments{raw_arguments},
                      registration->limits,
                      registration->executor,
                      *message_id,
                      {},
                      {},
                      {},
                      std::nullopt,
                      InvocationState::proposed,
                      0,
                      0,
                      false,
                      {}};
                  if (auto result = record_tool_error(
                          transaction, failed,
                          tool_domain_error(validated.error()));
                      !result) {
                    return result;
                  }
                  active->invocations.emplace(invocation_id, std::move(failed));
                } else {
                  active->invocations.emplace(
                      invocation_id,
                      PendingInvocation{invocation_id,
                                        std::nullopt,
                                        registration->declaration,
                                        std::move(*validated),
                                        registration->limits,
                                        registration->executor,
                                        *message_id,
                                        std::move(required_effects),
                                        std::move(required_scopes),
                                        {},
                                        std::nullopt,
                                        InvocationState::proposed,
                                        0,
                                        0,
                                        false,
                                        {}});
                }
                active->invocation_order.push_back(invocation_id);
              }
            }
            if (auto result = record_or_fail(domain::AssistantContentFinished{
                    *active->assistant_message_id, *active->inference_id});
                !result) {
              return result;
            }
            if (auto result = record_or_fail(domain::InferenceFinished{
                    *active->inference_id, value.reason});
                !result) {
              return result;
            }
            active->backend_terminal_seen = true;
            active->tool_call_finish = has_tools;
            if (!has_tools) {
              if (auto result = record_or_fail(domain::RunCompleted{});
                  !result) {
                return result;
              }
              active->run_terminal = true;
            }
            return {};
          } else if constexpr (std::same_as<Value,
                                            backend::ResponseCancelled>) {
            auto reason = active->cancel_requested ? active->cancel_reason
                                                   : std::move(value.reason);
            if (auto result = record_or_fail(
                    domain::InferenceCancelled{*active->inference_id, reason});
                !result) {
              return result;
            }
            if (auto result = record_or_fail(domain::RunCancelled{reason});
                !result) {
              return result;
            }
            active->backend_terminal_seen = true;
            active->run_terminal = true;
            return {};
          }
        },
        std::move(event));
  }

  [[nodiscard]] auto process_backend_failure(backend::BackendError error,
                                             Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active || !active->inference_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "backend failure followed inference termination"));
    }
    if (active->backend_terminal_seen) {
      if (active->cancellation_ack_expected &&
          error.kind == backend::BackendErrorKind::cancelled) {
        active->cancellation_ack_expected = false;
        return {};
      }
      return fail_live_run(transaction, protocol_domain_error());
    }
    if (error.kind == backend::BackendErrorKind::cancelled &&
        active->cancel_requested) {
      const auto reason = active->cancel_reason;
      if (auto result =
              record(active->run_id,
                     domain::InferenceCancelled{*active->inference_id, reason},
                     transaction);
          !result) {
        return result;
      }
      if (auto result =
              record(active->run_id, domain::RunCancelled{reason}, transaction);
          !result) {
        return result;
      }
      active->backend_terminal_seen = true;
      active->run_terminal = true;
      return {};
    }
    return fail_live_run(transaction, backend_domain_error(error));
  }

  [[nodiscard]] auto process_backend_end(Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active || !active->inference_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::no_active_run,
                       "backend stream ended without an active inference"));
    }
    if (!active->backend_terminal_seen) {
      if (auto failed = fail_live_run(transaction, protocol_domain_error());
          !failed) {
        return failed;
      }
    }
    if (active->run_terminal) {
      active.reset();
    } else {
      active->inference_id.reset();
      active->assistant_message_id.reset();
      active->response_started = false;
      active->backend_terminal_seen = false;
      active->tool_call_finish = false;
      active->cancellation_ack_expected = false;
      active->tool_calls.clear();
      active->tool_call_order.clear();
      if (auto evaluated = evaluate_pending_policies(transaction); !evaluated) {
        return evaluated;
      }
    }
    if (backend_worker.joinable()) backend_worker.join();
    return {};
  }

  [[nodiscard]] auto process_tool_update(ToolUpdate update,
                                         Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active || active->active_tool_id != update.invocation_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_invocation,
                       "tool update targets no active invocation"));
    }
    auto& invocation = active->invocations.at(update.invocation_id);
    if (active->run_terminal && invocation.terminal_event_seen) {
      return {};
    }
    if (invocation.terminal_event_seen) {
      return fail_live_run(transaction,
                           {domain::ErrorCode::invalid_state,
                            "tool executor protocol failure", false});
    }
    return std::visit(
        [&](auto&& event) -> std::expected<void, RunKernelError> {
          using Event = std::remove_cvref_t<decltype(event)>;
          if constexpr (std::same_as<Event, ToolInputRequested>) {
            if (invocation.state != InvocationState::running ||
                invocation.declaration.name != "ask_user" ||
                !valid_question_definitions(event.questions)) {
              return fail_live_run(transaction,
                                   {domain::ErrorCode::invalid_state,
                                    "tool input request is invalid", false});
            }
            invocation.questions = std::move(event.questions);
            for (const auto& question : invocation.questions) {
              if (auto result = record(
                      active->run_id, domain::QuestionRequested{question},
                      transaction, invocation.invocation_id);
                  !result) {
                return result;
              }
            }
            if (auto result = record(
                    active->run_id,
                    domain::RunAwaitingInput{
                        invocation.questions.front().question_id},
                    transaction, invocation.invocation_id);
                !result) {
              return result;
            }
            invocation.state = InvocationState::awaiting_input;
            return {};
          } else {
            const auto bytes = content_bytes(event.content);
            if (!bytes ||
                invocation.output_bytes > invocation.limits.output_bytes ||
                *bytes > invocation.limits.output_bytes -
                             invocation.output_bytes) {
              if (operation_stop) operation_stop->request_stop();
              return record_tool_error(
                  transaction, invocation,
                  tool_domain_error(ToolExecutionError{
                      ToolExecutionErrorCode::output_limit,
                      "tool output exceeded its budget", false}));
            }
            if constexpr (std::same_as<Event, ToolProgress>) {
              if (invocation.progress_events >=
                  invocation.limits.progress_events) {
                if (operation_stop) operation_stop->request_stop();
                return record_tool_error(
                    transaction, invocation,
                    tool_domain_error(ToolExecutionError{
                        ToolExecutionErrorCode::output_limit,
                        "tool progress exceeded its budget", false}));
              }
              ++invocation.progress_events;
              invocation.output_bytes += *bytes;
              return record(active->run_id,
                            domain::ToolProgressed{invocation.invocation_id,
                                                   std::move(event.content)},
                            transaction, invocation.invocation_id);
            } else {
              invocation.output_bytes += *bytes;
              if (!valid_created_artifacts(
                      event.created_artifacts, event.content,
                      invocation.invocation_id, transaction.event_log)) {
                return fail_live_run(
                    transaction,
                    tool_domain_error(ToolExecutionError{
                        ToolExecutionErrorCode::protocol_failure,
                        "tool executor returned invalid artifact metadata",
                        false}));
              }
              for (auto& artifact : event.created_artifacts) {
                const auto artifact_id = artifact.artifact_id;
                if (auto created = record(
                        active->run_id,
                        domain::ArtifactCreated{std::move(artifact)},
                        transaction, invocation.invocation_id);
                    !created) {
                  return created;
                }
                if (auto referenced = record(
                        active->run_id,
                        domain::ArtifactReferenced{artifact_id, std::nullopt},
                        transaction, invocation.invocation_id);
                    !referenced) {
                  return referenced;
                }
              }
              auto result = record(
                  active->run_id,
                  domain::ToolResultRecorded{invocation.invocation_id,
                                             std::move(event.content),
                                             invocation.result_message_id},
                  transaction, invocation.invocation_id);
              if (result) {
                invocation.state = InvocationState::terminal;
                invocation.terminal_event_seen = true;
              }
              return result;
            }
          }
        },
        std::move(update.event));
  }

  [[nodiscard]] auto process_tool_failure(ToolFailure failure,
                                          Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active || active->active_tool_id != failure.invocation_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_invocation,
                       "tool failure targets no active invocation"));
    }
    auto& invocation = active->invocations.at(failure.invocation_id);
    if (invocation.terminal_event_seen) return {};
    return record_tool_error(transaction, invocation,
                             tool_domain_error(failure.error));
  }

  [[nodiscard]] auto process_tool_deadline(const ToolDeadlineExpired& expired,
                                           Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active || active->active_tool_id != expired.invocation_id) return {};
    auto& invocation = active->invocations.at(expired.invocation_id);
    if (invocation.terminal_event_seen) return {};
    if (operation_stop) operation_stop->request_stop();
    return record_tool_error(
        transaction, invocation,
        tool_domain_error(
            ToolExecutionError{ToolExecutionErrorCode::timed_out,
                               "tool execution exceeded its deadline", false}));
  }

  [[nodiscard]] auto process_tool_end(const ToolEnded& ended,
                                      Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active || active->active_tool_id != ended.invocation_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_invocation,
                       "tool stream ended for no active invocation"));
    }
    auto& invocation = active->invocations.at(ended.invocation_id);
    if (!invocation.terminal_event_seen &&
        invocation.state != InvocationState::awaiting_input) {
      if (auto result = record_tool_error(
              transaction, invocation,
              tool_domain_error(ToolExecutionError{
                  ToolExecutionErrorCode::protocol_failure,
                  "tool stream ended without a terminal result", false}));
          !result) {
        return result;
      }
    }
    active->active_tool_id.reset();
    if (active->run_terminal) active.reset();
    if (operation_stop) operation_stop->request_stop();
    deadline_worker.request_stop();
    if (deadline_worker.joinable()) deadline_worker.join();
    if (tool_worker.joinable()) tool_worker.join();
    operation_stop.reset();
    return {};
  }

  [[nodiscard]] auto evaluate_pending_policies(Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    if (!transaction.active || transaction.active->inference_id) return {};
    for (const auto& invocation_id : transaction.active->invocation_order) {
      auto& invocation = transaction.active->invocations.at(invocation_id);
      if (invocation.state != InvocationState::proposed ||
          invocation.terminal_event_seen) {
        continue;
      }
      invocation.policy_request = ToolPolicyRequest{
          transaction.event_log.session_id(), transaction.active->run_id,
          invocation.invocation_id,
          transaction.active->permission_profile_id,
          invocation.declaration.name, invocation.requested_effects,
          invocation.requested_scopes};

      std::expected<ToolPolicyResolution, ToolPolicyError> resolution =
          std::unexpected(ToolPolicyError{
              ToolPolicyErrorCode::internal_failure,
              "tool policy evaluation failed internally", false});
      try {
        resolution = policy->evaluate(*invocation.policy_request);
      } catch (...) {
      }
      if (!resolution) {
        const auto domain_error = policy_failure_error(resolution.error());
        if (auto recorded = record(
                transaction.active->run_id,
                domain::ToolPolicyFailed{invocation.invocation_id,
                                         domain_error},
                transaction, invocation.invocation_id);
            !recorded) {
          return recorded;
        }
        if (auto failed = record_tool_error(transaction, invocation,
                                            domain_error);
            !failed) {
          return failed;
        }
        continue;
      }
      if (!valid_policy_decision(resolution->decision) ||
          (resolution->redacted_reason &&
           (resolution->redacted_reason->size() > 4096 ||
            has_control_character(*resolution->redacted_reason))) ||
          !scopes_are_unique(resolution->scopes)) {
        return fail_live_run(transaction, protocol_domain_error());
      }
      if (resolution->decision != domain::PolicyDecision::deny) {
        const auto forward = intersect_capability_scopes(
            resolution->scopes, invocation.requested_scopes);
        const auto reverse = intersect_capability_scopes(
            invocation.requested_scopes, resolution->scopes);
        if (!forward || !reverse) {
          return fail_live_run(transaction, policy_denied_error());
        }
      } else if (!resolution->scopes.empty()) {
        return fail_live_run(transaction, protocol_domain_error());
      }
      if (auto recorded = record(
              transaction.active->run_id,
              domain::ToolPolicyDecided{
                  invocation.invocation_id, resolution->decision,
                  resolution->scopes,
                  std::move(resolution->redacted_reason), resolution->source},
              transaction, invocation.invocation_id);
          !recorded) {
        return recorded;
      }
      switch (resolution->decision) {
        case domain::PolicyDecision::allow:
          invocation.granted_scopes = std::move(resolution->scopes);
          invocation.state = InvocationState::allowed;
          break;
        case domain::PolicyDecision::deny:
          if (auto failed = record_tool_error(transaction, invocation,
                                              policy_denied_error());
              !failed) {
            return failed;
          }
          break;
        case domain::PolicyDecision::require_approval:
          if (auto requested = record(
                  transaction.active->run_id,
                  domain::ToolApprovalRequested{
                      invocation.invocation_id, resolution->scopes,
                      std::string{"Approval is required by runtime policy"}},
                  transaction, invocation.invocation_id);
              !requested) {
            return requested;
          }
          invocation.state = InvocationState::awaiting_approval;
          break;
      }
    }
    return {};
  }

  [[nodiscard]] auto launch_next_tool() -> std::expected<void, RunKernelError> {
    if (!active || active->run_terminal || active->inference_id ||
        active->active_tool_id) {
      return {};
    }
    auto ready = active->invocation_order.end();
    for (auto current = active->invocation_order.begin();
         current != active->invocation_order.end(); ++current) {
      const auto state = active->invocations.at(*current).state;
      if (state == InvocationState::terminal) continue;
      if (state != InvocationState::allowed) return {};
      ready = current;
      break;
    }
    if (ready == active->invocation_order.end()) return {};

    auto transaction = this->transaction();
    auto& invocation = transaction.active->invocations.at(*ready);
    if (auto result = record(transaction.active->run_id,
                             domain::ToolStarted{invocation.invocation_id},
                             transaction, invocation.invocation_id);
        !result) {
      return result;
    }
    invocation.state = InvocationState::running;
    transaction.active->active_tool_id = invocation.invocation_id;
    const auto invocation_copy = invocation;
    if (auto committed = commit(std::move(transaction)); !committed) {
      return committed;
    }

    try {
      operation_stop.emplace();
      auto stop_source = *operation_stop;
      const auto invocation_id = invocation_copy.invocation_id;
      deadline_worker =
          std::jthread([impl = this, stop_source, invocation_id,
                        timeout = invocation_copy.limits.timeout](
                           const std::stop_token stop_token) mutable {
            std::mutex mutex;
            std::unique_lock lock(mutex);
            std::condition_variable_any changed;
            static_cast<void>(changed.wait_for(lock, stop_token, timeout,
                                               [] { return false; }));
            if (!stop_token.stop_requested()) {
              static_cast<void>(impl->push(ToolDeadlineExpired{invocation_id}));
              stop_source.request_stop();
            }
          });
      tool_worker = std::jthread([impl = this, invocation_copy,
                                  stop_source](std::stop_token) mutable {
        try {
          impl->run_tool(invocation_copy, stop_source.get_token());
        } catch (...) {
          try {
            static_cast<void>(impl->push(ToolFailure{
                invocation_copy.invocation_id,
                ToolExecutionError{ToolExecutionErrorCode::internal_failure,
                                   "tool worker failed internally", false}}));
            static_cast<void>(
                impl->push(ToolEnded{invocation_copy.invocation_id}));
          } catch (...) {
          }
        }
      });
      return {};
    } catch (...) {
      if (operation_stop) operation_stop->request_stop();
      deadline_worker.request_stop();
      if (deadline_worker.joinable()) deadline_worker.join();
      auto failure = this->transaction();
      auto& failed =
          failure.active->invocations.at(invocation_copy.invocation_id);
      failure.active->active_tool_id.reset();
      if (auto result =
              record_tool_error(failure, failed,
                                tool_domain_error(ToolExecutionError{
                                    ToolExecutionErrorCode::internal_failure,
                                    "could not start tool worker", false}));
          !result) {
        return result;
      }
      operation_stop.reset();
      return commit(std::move(failure));
    }
  }

  domain::SessionEventLog event_log;
  std::map<domain::RunId, domain::RunProjection> projections;
  std::set<domain::InvocationId> used_invocation_ids;
  backend::Backend& backend_port;
  RunWakeSink* wake;
  TimestampSource timestamp;
  RunKernelLimits limits;
  ToolRegistrySnapshot tools;
  std::shared_ptr<ToolPolicy> policy;
  storage::SessionStore* session_store{};
  std::optional<ActiveRun> active;
  bool unusable{};

  std::jthread backend_worker;
  std::jthread tool_worker;
  std::jthread deadline_worker;
  std::optional<std::stop_source> operation_stop;

  std::mutex queue_mutex;
  std::condition_variable queue_space;
  std::deque<WorkerUpdate> queue;
  bool queue_closed{};
};

RunKernel::RunKernel(domain::SessionId session_id, backend::Backend& backend,
                     RunWakeSink* wake_sink, TimestampSource timestamp_source,
                     RunKernelLimits limits, ToolRegistrySnapshot tools,
                     std::shared_ptr<ToolPolicy> policy)
    : m_impl(std::make_unique<Impl>(std::move(session_id), backend, wake_sink,
                                    std::move(timestamp_source), limits,
                                    std::move(tools), std::move(policy))) {
}

auto RunKernel::open_durable(DurableSessionOpen session,
                             storage::SessionStore& store,
                             backend::Backend& backend, RunWakeSink* wake_sink,
                             TimestampSource timestamp_source,
                             RunKernelLimits limits, ToolRegistrySnapshot tools,
                             std::shared_ptr<ToolPolicy> policy)
    -> std::expected<std::unique_ptr<RunKernel>, RunKernelError> {
  try {
    auto kernel = std::unique_ptr<RunKernel>{
        new RunKernel(session.session_id, backend, wake_sink,
                      std::move(timestamp_source), limits, std::move(tools),
                      std::move(policy))};
    if (!kernel->m_impl->valid_limits()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_limits,
                       "run-kernel limits must be positive"));
    }
    if (session.mode == DurableSessionMode::create) {
      auto created =
          store.create_session({session.session_id, session.created_at});
      if (!created) {
        return std::unexpected(kernel_error(RunKernelErrorCode::storage_failure,
                                            "durable session creation failed",
                                            created.error().retryable));
      }
    } else {
      auto info = store.open_session(session.session_id);
      if (!info) {
        return std::unexpected(kernel_error(
            RunKernelErrorCode::storage_failure,
            "durable session could not be opened", info.error().retryable));
      }
      auto events = store.replay_events(session.session_id);
      if (!events) {
        return std::unexpected(kernel_error(RunKernelErrorCode::storage_failure,
                                            "durable session replay failed",
                                            events.error().retryable));
      }

      domain::SessionEventLog event_log{session.session_id};
      std::map<domain::RunId, domain::RunProjection> projections;
      std::set<domain::InvocationId> invocation_ids;
      for (const auto& event : *events) {
        auto projection = projections.contains(event.metadata.run_id)
                              ? projections.at(event.metadata.run_id)
                              : domain::RunProjection{};
        if (!projection.apply(event) || !event_log.append(event)) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::replay_rejected,
              "durable session events could not rebuild projections"));
        }
        const auto payload_invocation = tool_invocation_id(event.payload);
        if (payload_invocation &&
            (!event.metadata.invocation_id ||
             *event.metadata.invocation_id != *payload_invocation)) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::replay_rejected,
              "durable session contains mismatched invocation identity"));
        }
        if (const auto* proposed =
                std::get_if<domain::ToolProposed>(&event.payload);
            proposed != nullptr &&
            !invocation_ids.insert(proposed->invocation_id).second) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::replay_rejected,
              "durable session contains invalid invocation identity"));
        }
        projections.insert_or_assign(event.metadata.run_id,
                                     std::move(projection));
      }
      if (event_log.last_sequence() != info->last_sequence) {
        return std::unexpected(kernel_error(
            RunKernelErrorCode::replay_rejected,
            "durable session catalog disagrees with replayed history"));
      }

      std::optional<domain::RunId> awaiting_run;
      for (const auto& [run_id, projection] : projections) {
        if (projection.status() != domain::RunStatus::awaiting_input) continue;
        if (awaiting_run) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::replay_rejected,
              "durable session contains multiple awaiting runs"));
        }
        awaiting_run = run_id;
      }
      if (awaiting_run) {
        std::optional<domain::InvocationId> invocation_id;
        std::optional<domain::RunStarted> started;
        std::vector<domain::InvocationId> current_batch;
        std::map<domain::InvocationId, domain::ToolProposed> proposals;
        std::map<domain::InvocationId, domain::PolicyDecision>
            policy_decisions;
        std::map<domain::InvocationId,
                 std::vector<domain::CapabilityScope>>
            policy_scopes;
        std::map<domain::InvocationId, domain::ApprovalDecision>
            approval_decisions;
        std::map<domain::InvocationId,
                 std::vector<domain::CapabilityScope>>
            approval_scopes;
        std::map<domain::InvocationId,
                 std::vector<domain::QuestionDefinition>>
            questions;
        std::map<domain::InvocationId, domain::MessageId>
            terminal_message_ids;
        std::set<domain::InvocationId> tool_started;
        std::set<domain::InvocationId> tool_terminal;
        for (const auto& event : *events) {
          if (event.metadata.run_id != *awaiting_run) continue;
          if (const auto* value =
                  std::get_if<domain::RunStarted>(&event.payload)) {
            started = *value;
          } else if (std::holds_alternative<domain::InferenceStarted>(
                         event.payload)) {
            current_batch.clear();
            proposals.clear();
            policy_decisions.clear();
            policy_scopes.clear();
            approval_decisions.clear();
            approval_scopes.clear();
            questions.clear();
            terminal_message_ids.clear();
            tool_started.clear();
            tool_terminal.clear();
          } else if (const auto* value =
                         std::get_if<domain::ToolProposed>(&event.payload)) {
            current_batch.push_back(value->invocation_id);
            proposals.insert_or_assign(value->invocation_id, *value);
          } else if (const auto* value =
                         std::get_if<domain::ToolPolicyDecided>(
                             &event.payload)) {
            policy_decisions.insert_or_assign(value->invocation_id,
                                               value->decision);
            policy_scopes.insert_or_assign(value->invocation_id,
                                           value->scopes);
          } else if (const auto* value =
                         std::get_if<domain::ToolApprovalDecided>(
                             &event.payload)) {
            approval_decisions.insert_or_assign(value->invocation_id,
                                                 value->decision);
            approval_scopes.insert_or_assign(value->invocation_id,
                                             value->granted_scopes);
          } else if (const auto* value =
                         std::get_if<domain::ToolStarted>(&event.payload)) {
            tool_started.insert(value->invocation_id);
          } else if (const auto* value =
                         std::get_if<domain::QuestionRequested>(
                             &event.payload)) {
            if (event.metadata.invocation_id) {
              questions[*event.metadata.invocation_id].push_back(
                  value->question);
            }
          } else if (const auto* value =
                         std::get_if<domain::ToolResultRecorded>(
                             &event.payload)) {
            tool_terminal.insert(value->invocation_id);
            if (value->result_message_id) {
              terminal_message_ids.insert_or_assign(
                  value->invocation_id, *value->result_message_id);
            }
          } else if (const auto* value =
                         std::get_if<domain::ToolErrored>(&event.payload)) {
            tool_terminal.insert(value->invocation_id);
            if (value->result_message_id) {
              terminal_message_ids.insert_or_assign(
                  value->invocation_id, *value->result_message_id);
            }
          } else if (std::holds_alternative<domain::RunAwaitingInput>(
                         event.payload)) {
            invocation_id = event.metadata.invocation_id;
          }
        }
        if (!invocation_id || !started) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::replay_rejected,
              "awaiting question history lacks runtime identity"));
        }
        if (!proposals.contains(*invocation_id) ||
            !tool_started.contains(*invocation_id) ||
            tool_terminal.contains(*invocation_id) ||
            !valid_question_definitions(questions[*invocation_id])) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::replay_rejected,
              "awaiting question history is incomplete"));
        }
        std::map<domain::InvocationId, Impl::PendingInvocation> invocations;
        for (const auto& current_id : current_batch) {
          const auto proposed = proposals.find(current_id);
          if (proposed == proposals.end()) {
            return std::unexpected(kernel_error(
                RunKernelErrorCode::replay_rejected,
                "queued tool history lacks its proposal"));
          }
          const auto* registration =
              kernel->m_impl->tools.find(proposed->second.tool_name);
          if (registration == nullptr) {
            return std::unexpected(kernel_error(
                current_id == *invocation_id
                    ? RunKernelErrorCode::interactive_input_unavailable
                    : RunKernelErrorCode::replay_rejected,
                current_id == *invocation_id
                    ? "pending ask_user input is unavailable on this surface"
                    : "queued tool is unavailable during replay"));
          }
          const auto scopes_are_declared = [&](const auto& scopes) {
            return std::ranges::all_of(scopes, [&](const auto& requested) {
              return std::ranges::any_of(
                  registration->declaration.capability_scopes,
                  [&](const auto& declared) {
                    return capability_scope_covers(declared, requested);
                  });
            });
          };
          if (!effects_are_declared(
                  proposed->second.declared_effects,
                  registration->declaration.effects) ||
              !scopes_are_unique(
                  proposed->second.validated_required_scopes) ||
              !scopes_are_unique(proposed->second.requested_scopes) ||
              !scopes_are_declared(
                  proposed->second.validated_required_scopes) ||
              !scopes_are_declared(proposed->second.requested_scopes) ||
              std::ranges::any_of(
                  proposed->second.requested_scopes, [&](const auto& scope) {
                    return std::ranges::find(
                               proposed->second.declared_effects,
                               scope.effect) ==
                           proposed->second.declared_effects.end();
                  })) {
            return std::unexpected(kernel_error(
                RunKernelErrorCode::replay_rejected,
                "queued tool declaration changed during replay"));
          }

          auto state = Impl::InvocationState::proposed;
          std::vector<domain::CapabilityScope> granted_scopes;
          std::optional<ToolPolicyRequest> policy_request;
          if (tool_terminal.contains(current_id)) {
            state = Impl::InvocationState::terminal;
          } else if (current_id == *invocation_id) {
            if (proposed->second.tool_name != "ask_user" ||
                !policy_decisions.contains(current_id) ||
                policy_decisions.at(current_id) !=
                    domain::PolicyDecision::allow) {
              return std::unexpected(kernel_error(
                  RunKernelErrorCode::replay_rejected,
                  "pending input lacks an allowed ask_user invocation"));
            }
            state = Impl::InvocationState::awaiting_input;
            granted_scopes = policy_scopes.at(current_id);
          } else if (tool_started.contains(current_id) ||
                     (approval_decisions.contains(current_id) &&
                      approval_decisions.at(current_id) !=
                          domain::ApprovalDecision::approved)) {
            return std::unexpected(kernel_error(
                RunKernelErrorCode::replay_rejected,
                "queued tool history has an unterminated execution"));
          } else if (approval_decisions.contains(current_id) &&
                     approval_decisions.at(current_id) ==
                         domain::ApprovalDecision::approved) {
            state = Impl::InvocationState::allowed;
            granted_scopes = approval_scopes.at(current_id);
          } else if (policy_decisions.contains(current_id) &&
                     policy_decisions.at(current_id) ==
                         domain::PolicyDecision::allow) {
            state = Impl::InvocationState::allowed;
            granted_scopes = policy_scopes.at(current_id);
          } else if (policy_decisions.contains(current_id) &&
                     policy_decisions.at(current_id) ==
                         domain::PolicyDecision::require_approval) {
            state = Impl::InvocationState::awaiting_approval;
            policy_request = ToolPolicyRequest{
                event_log.session_id(), *awaiting_run, current_id,
                started->permission_profile_id,
                proposed->second.tool_name,
                proposed->second.declared_effects,
                proposed->second.requested_scopes};
          } else {
            return std::unexpected(kernel_error(
                RunKernelErrorCode::replay_rejected,
                "queued tool history has no resumable state"));
          }

          auto result_message_id = proposed->second.result_message_id;
          if (!result_message_id &&
              terminal_message_ids.contains(current_id)) {
            result_message_id = terminal_message_ids.at(current_id);
          }
          if (!result_message_id) {
            return std::unexpected(kernel_error(
                RunKernelErrorCode::replay_rejected,
                "queued tool history lacks a result message identity"));
          }
          if (state != Impl::InvocationState::terminal &&
              !proposed->second.arguments_replayable) {
            return std::unexpected(kernel_error(
                RunKernelErrorCode::replay_rejected,
                "queued tool history lacks normalized arguments"));
          }
          auto invocation_questions = questions.contains(current_id)
                                          ? questions.at(current_id)
                                          : std::vector<
                                                domain::QuestionDefinition>{};
          invocations.emplace(
              current_id,
              Impl::PendingInvocation{
                  current_id, proposed->second.parent_invocation_id,
                  registration->declaration,
                  ValidatedToolArguments{
                      proposed->second.arguments,
                      proposed->second.validated_required_scopes,
                      proposed->second.declared_effects},
                  registration->limits, registration->executor,
                  *result_message_id, proposed->second.declared_effects,
                  proposed->second.requested_scopes,
                  std::move(granted_scopes), std::move(policy_request), state,
                  0, 0, tool_terminal.contains(current_id),
                  std::move(invocation_questions)});
        }
        kernel->m_impl->active = Impl::ActiveRun{
            *awaiting_run,
            started->permission_profile_id,
            std::nullopt,
            std::nullopt,
            {},
            {},
            std::move(invocations),
            std::move(current_batch),
            std::nullopt,
            false,
            false,
            false,
            false,
            false,
            false,
            std::nullopt};
      }
      kernel->m_impl->event_log = std::move(event_log);
      kernel->m_impl->projections = std::move(projections);
      kernel->m_impl->used_invocation_ids = std::move(invocation_ids);
    }
    kernel->m_impl->session_store = &store;
    return kernel;
  } catch (...) {
    return std::unexpected(
        kernel_error(RunKernelErrorCode::internal_failure,
                     "durable session open failed internally"));
  }
}

RunKernel::~RunKernel() = default;

auto RunKernel::start(RunStart start) -> std::expected<void, RunKernelError> {
  try {
    if (m_impl->unusable) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::storage_failure,
          "run kernel is unavailable after a persistence failure"));
    }
    if (!m_impl->valid_limits()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_limits,
                       "run-kernel limits must be positive"));
    }
    if (m_impl->active) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::run_already_active, "another run is active"));
    }
    if (start.user_message.role != domain::Role::user ||
        start.request.assistant_message_id == start.user_message.message_id ||
        start.request.tools != m_impl->tools.declarations()) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::invalid_start,
          "run start contains invalid identity or tool declarations"));
    }
    if (m_impl->projections.contains(start.run_id)) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_start,
                       "run ID is already present in the session"));
    }
    const auto persona_entries = std::ranges::count_if(
        start.request.context.entries, [](const domain::ContextEntry& entry) {
          return entry.kind == domain::ContextEntryKind::instruction &&
                 entry.instruction_layer == domain::InstructionLayer::persona;
        });
    if (!start.persona_selection) {
      if (start.attributes.persona_id || persona_entries != 0) {
        return std::unexpected(kernel_error(
            RunKernelErrorCode::invalid_start,
            "run persona metadata is incomplete"));
      }
    } else {
      if (!domain::validate_persona_selection(*start.persona_selection)) {
        return std::unexpected(kernel_error(
            RunKernelErrorCode::invalid_start,
            "run persona selection is invalid"));
      }
      if (start.persona_selection->action ==
          domain::PersonaSelectionAction::disabled) {
        if (start.attributes.persona_id || persona_entries != 0) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::invalid_start,
              "disabled persona selection entered the backend context"));
        }
      } else {
        const auto& persona = *start.persona_selection->persona;
        const auto found = std::ranges::find_if(
            start.request.context.entries,
            [](const domain::ContextEntry& entry) {
              return entry.kind == domain::ContextEntryKind::instruction &&
                     entry.instruction_layer == domain::InstructionLayer::persona;
            });
        const auto expected_digest = persona.content_digest.algorithm + ":" +
                                     persona.content_digest.value;
        if (start.attributes.persona_id != persona.persona_id ||
            persona_entries != 1 ||
            found == start.request.context.entries.end() ||
            found->provenance.source_location != persona.source_location ||
            found->provenance.digest != expected_digest) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::invalid_start,
              "run persona selection does not match constructed context"));
        }
      }
    }
    if (start.provenance) {
      if (!start.provenance->tools.empty()) {
        return std::unexpected(
            kernel_error(RunKernelErrorCode::invalid_start,
                         "run provenance tool identity is kernel-owned"));
      }
      for (const auto& declaration : m_impl->tools.declarations()) {
        start.provenance->tools.push_back({declaration.name,
                                           declaration.effects,
                                           declaration.capability_scopes});
      }
      // Validated before anything is recorded so an ephemeral run cannot carry
      // a sensitive value into the in-memory log either.
      if (auto valid = domain::validate_run_provenance(*start.provenance);
          !valid) {
        return std::unexpected(kernel_error(
            RunKernelErrorCode::invalid_start,
            "run provenance is invalid: " + valid.error().message));
      }
    }

    Impl::ActiveRun active{start.run_id,
                           start.attributes.permission_profile_id,
                           start.request.inference_id,
                           start.request.assistant_message_id,
                           {},
                           {},
                           {},
                           {},
                           std::nullopt,
                           false,
                           false,
                           false,
                           false,
                           false,
                           false,
                           std::nullopt};
    auto transaction = m_impl->transaction();
    if (auto result = m_impl->record(start.run_id, std::move(start.attributes),
                                     transaction);
        !result) {
      return result;
    }
    if (start.provenance) {
      if (auto result = m_impl->record(
              start.run_id,
              domain::RunProvenanceRecorded{std::move(*start.provenance)},
              transaction);
          !result) {
        return result;
      }
    }
    if (start.persona_selection) {
      if (auto result = m_impl->record(
              start.run_id,
              domain::PersonaSelectionRecorded{
                  std::move(*start.persona_selection)},
              transaction);
          !result) {
        return result;
      }
    }
    if (auto result = m_impl->record(
            start.run_id,
            domain::UserContentAdded{std::move(start.user_message)},
            transaction);
        !result) {
      return result;
    }
    if (auto result = m_impl->record(
            start.run_id, domain::RunCompletionRequested{}, transaction);
        !result) {
      return result;
    }
    if (auto result =
            m_impl->record(start.run_id,
                           domain::InferenceStarted{start.request.inference_id,
                                                    start.request.model_id},
                           transaction);
        !result) {
      return result;
    }
    transaction.active = std::move(active);
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    auto launched = m_impl->launch_backend(std::move(start.request));
    if (launched) return {};
    auto failure = m_impl->transaction();
    if (auto failed = m_impl->fail_live_run(failure, protocol_domain_error());
        !failed) {
      return failed;
    }
    if (auto committed = m_impl->commit(std::move(failure)); !committed) {
      return committed;
    }
    return launched;
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                        "run start failed internally"));
  }
}

auto RunKernel::cancel(const domain::RunId& run_id,
                       const domain::InferenceId& inference_id,
                       std::optional<std::string> reason)
    -> std::expected<void, RunKernelError> {
  try {
    if (m_impl->unusable) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::storage_failure,
          "run kernel is unavailable after a persistence failure"));
    }
    if (!m_impl->active || !m_impl->active->inference_id) {
      return std::unexpected(kernel_error(RunKernelErrorCode::no_active_run,
                                          "there is no active inference"));
    }
    if (m_impl->active->run_id != run_id) {
      return std::unexpected(kernel_error(RunKernelErrorCode::wrong_run,
                                          "cancellation targets another run"));
    }
    if (*m_impl->active->inference_id != inference_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_inference,
                       "cancellation targets another inference"));
    }
    return cancel_run(run_id, std::move(reason));
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                        "run cancellation failed internally"));
  }
}

auto RunKernel::cancel_run(const domain::RunId& run_id,
                           std::optional<std::string> reason)
    -> std::expected<void, RunKernelError> {
  try {
    if (m_impl->unusable) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::storage_failure,
          "run kernel is unavailable after a persistence failure"));
    }
    if (!m_impl->active) {
      return std::unexpected(kernel_error(RunKernelErrorCode::no_active_run,
                                          "there is no active run"));
    }
    if (m_impl->active->run_id != run_id) {
      return std::unexpected(kernel_error(RunKernelErrorCode::wrong_run,
                                          "cancellation targets another run"));
    }
    if (m_impl->active->run_terminal || m_impl->active->backend_terminal_seen) {
      return std::unexpected(kernel_error(RunKernelErrorCode::already_terminal,
                                          "the run is already terminal"));
    }
    if (m_impl->active->cancel_requested) return {};

    auto transaction = m_impl->transaction();
    if (auto result = m_impl->record(run_id, domain::RunCancelRequested{reason},
                                     transaction);
        !result) {
      return result;
    }
    transaction.active->cancel_requested = true;
    transaction.active->cancel_reason = reason;
    if (!transaction.active->inference_id) {
      for (const auto& invocation_id : transaction.active->invocation_order) {
        auto& invocation = transaction.active->invocations.at(invocation_id);
        if (!invocation.terminal_event_seen) {
          if (invocation.state == Impl::InvocationState::awaiting_input) {
            for (const auto& question : invocation.questions) {
              if (auto result = m_impl->record(
                      run_id,
                      domain::QuestionCancelled{question.question_id, reason},
                      transaction, invocation.invocation_id);
                  !result) {
                return result;
              }
            }
          }
          if (auto result = m_impl->record_tool_error(
                  transaction, invocation,
                  {domain::ErrorCode::cancelled, "tool execution cancelled",
                   false});
              !result) {
            return result;
          }
        }
      }
      if (auto result =
              m_impl->record(run_id, domain::RunCancelled{reason}, transaction);
          !result) {
        return result;
      }
      transaction.active->run_terminal = true;
      if (!transaction.active->active_tool_id) transaction.active.reset();
    }
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    m_impl->backend_worker.request_stop();
    if (m_impl->operation_stop) m_impl->operation_stop->request_stop();
    return {};
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                        "run cancellation failed internally"));
  }
}

auto RunKernel::decide_approval(const domain::RunId& run_id,
                                const domain::InvocationId& invocation_id,
                                ToolApprovalResolution resolution)
    -> std::expected<void, RunKernelError> {
  try {
    if (!m_impl->active || m_impl->active->run_id != run_id) {
      return std::unexpected(
          kernel_error(m_impl->active ? RunKernelErrorCode::wrong_run
                                      : RunKernelErrorCode::no_active_run,
                       "approval decision targets no active run"));
    }
    const auto found = m_impl->active->invocations.find(invocation_id);
    if (found == m_impl->active->invocations.end()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_invocation,
                       "approval decision targets an unknown invocation"));
    }
    if (found->second.state != Impl::InvocationState::awaiting_approval) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "approval may be decided only while awaiting approval"));
    }
    if (!valid_approval_decision(resolution.decision) ||
        !valid_approval_lifetime(resolution.lifetime) ||
        !scopes_are_unique(resolution.granted_scopes) ||
        (resolution.decision != domain::ApprovalDecision::approved &&
         (!resolution.granted_scopes.empty() ||
          resolution.lifetime !=
              domain::ApprovalGrantLifetime::invocation))) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "approval decision is malformed"));
    }

    auto transaction = m_impl->transaction();
    auto& invocation = transaction.active->invocations.at(invocation_id);
    if (resolution.decision == domain::ApprovalDecision::approved) {
      if (!invocation.policy_request) {
        return std::unexpected(
            kernel_error(RunKernelErrorCode::invalid_tool_state,
                         "approval has no policy request"));
      }
      std::expected<ToolPolicyResolution, ToolPolicyError> approved =
          std::unexpected(ToolPolicyError{
              ToolPolicyErrorCode::internal_failure,
              "tool policy approval failed internally", false});
      try {
        approved = m_impl->policy->approve(
            *invocation.policy_request,
            ToolPolicyApproval{resolution.granted_scopes,
                               resolution.lifetime});
      } catch (...) {
      }
      if (!approved) {
        const auto domain_error = policy_failure_error(approved.error());
        if (auto failed = m_impl->record(
                run_id,
                domain::ToolPolicyFailed{invocation_id, domain_error},
                transaction, invocation_id);
            !failed) {
          return failed;
        }
        if (auto committed = m_impl->commit(std::move(transaction));
            !committed) {
          return committed;
        }
        return std::unexpected(kernel_error(
            approved.error().code == ToolPolicyErrorCode::scope_widening
                ? RunKernelErrorCode::policy_scope_widening
                : RunKernelErrorCode::policy_failure,
            domain_error.message, approved.error().retryable));
      }
      invocation.granted_scopes = std::move(approved->scopes);
    }
    if (auto result = m_impl->record(
            run_id,
            domain::ToolApprovalDecided{invocation_id, resolution.decision,
                                        invocation.granted_scopes,
                                        resolution.lifetime},
            transaction, invocation_id);
        !result) {
      return result;
    }
    if (resolution.decision == domain::ApprovalDecision::approved) {
      invocation.state = Impl::InvocationState::allowed;
    } else {
      auto error = resolution.decision == domain::ApprovalDecision::cancelled
                       ? approval_cancelled_error()
                       : policy_denied_error();
      if (auto result = m_impl->record_tool_error(transaction, invocation,
                                                  std::move(error));
          !result) {
        return result;
      }
    }
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    return m_impl->launch_next_tool();
  } catch (...) {
    return std::unexpected(
        kernel_error(RunKernelErrorCode::internal_failure,
                     "tool approval decision failed internally"));
  }
}

auto RunKernel::answer_questions(
    const domain::RunId& run_id,
    const domain::InvocationId& invocation_id,
    std::vector<domain::QuestionAnswer> answers)
    -> std::expected<void, RunKernelError> {
  try {
    if (m_impl->unusable) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::storage_failure,
          "run kernel is unavailable after a persistence failure"));
    }
    if (!m_impl->active || m_impl->active->run_id != run_id) {
      return std::unexpected(
          kernel_error(m_impl->active ? RunKernelErrorCode::wrong_run
                                      : RunKernelErrorCode::no_active_run,
                       "question answer targets no active run"));
    }
    const auto found = m_impl->active->invocations.find(invocation_id);
    if (found == m_impl->active->invocations.end()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_invocation,
                       "question answer targets an unknown invocation"));
    }
    if (found->second.state != Impl::InvocationState::awaiting_input ||
        found->second.terminal_event_seen ||
        !valid_question_answers(found->second.questions, answers)) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "question answer is invalid or already resolved"));
    }

    auto transaction = m_impl->transaction();
    auto& invocation = transaction.active->invocations.at(invocation_id);
    for (const auto& answer : answers) {
      if (auto result = m_impl->record(
              run_id, domain::QuestionAnswered{answer}, transaction,
              invocation_id);
          !result) {
        return result;
      }
    }
    if (auto result = m_impl->record(
            run_id,
            domain::ToolResultRecorded{invocation_id,
                                       answered_content(answers),
                                       invocation.result_message_id},
            transaction, invocation_id);
        !result) {
      return result;
    }
    if (auto result = m_impl->record(
            run_id,
            domain::RunResumed{invocation.questions.front().question_id},
            transaction, invocation_id);
        !result) {
      return result;
    }
    invocation.state = Impl::InvocationState::terminal;
    invocation.terminal_event_seen = true;
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    return m_impl->launch_next_tool();
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                        "question answer failed internally"));
  }
}

auto RunKernel::cancel_questions(
    const domain::RunId& run_id,
    const domain::InvocationId& invocation_id,
    std::optional<std::string> reason)
    -> std::expected<void, RunKernelError> {
  try {
    if (m_impl->unusable) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::storage_failure,
          "run kernel is unavailable after a persistence failure"));
    }
    if (reason &&
        (reason->size() > 4096 || has_control_character(*reason))) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "question cancellation reason is invalid"));
    }
    if (!m_impl->active || m_impl->active->run_id != run_id) {
      return std::unexpected(
          kernel_error(m_impl->active ? RunKernelErrorCode::wrong_run
                                      : RunKernelErrorCode::no_active_run,
                       "question cancellation targets no active run"));
    }
    const auto found = m_impl->active->invocations.find(invocation_id);
    if (found == m_impl->active->invocations.end()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_invocation,
                       "question cancellation targets an unknown invocation"));
    }
    if (found->second.state != Impl::InvocationState::awaiting_input ||
        found->second.terminal_event_seen) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "question invocation is not awaiting input"));
    }

    auto transaction = m_impl->transaction();
    auto& invocation = transaction.active->invocations.at(invocation_id);
    for (const auto& question : invocation.questions) {
      if (auto result = m_impl->record(
              run_id,
              domain::QuestionCancelled{question.question_id, reason},
              transaction, invocation_id);
          !result) {
        return result;
      }
    }
    if (auto result = m_impl->record(
            run_id,
            domain::ToolResultRecorded{invocation_id, cancelled_content(),
                                       invocation.result_message_id},
            transaction, invocation_id);
        !result) {
      return result;
    }
    if (auto result = m_impl->record(
            run_id,
            domain::RunResumed{invocation.questions.front().question_id},
            transaction, invocation_id);
        !result) {
      return result;
    }
    invocation.state = Impl::InvocationState::terminal;
    invocation.terminal_event_seen = true;
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    return m_impl->launch_next_tool();
  } catch (...) {
    return std::unexpected(
        kernel_error(RunKernelErrorCode::internal_failure,
                     "question cancellation failed internally"));
  }
}

auto RunKernel::continue_run(const domain::RunId& run_id,
                             backend::BackendRequest request)
    -> std::expected<void, RunKernelError> {
  try {
    if (!m_impl->active || m_impl->active->run_id != run_id) {
      return std::unexpected(
          kernel_error(m_impl->active ? RunKernelErrorCode::wrong_run
                                      : RunKernelErrorCode::no_active_run,
                       "continuation targets no active run"));
    }
    const auto& active = *m_impl->active;
    if (active.inference_id || active.active_tool_id ||
        active.invocation_order.empty() || active.run_terminal ||
        std::ranges::any_of(
            active.invocation_order,
            [&](const auto& invocation_id) {
              return active.invocations.at(invocation_id).state !=
                     Impl::InvocationState::terminal;
            }) ||
        request.tools != m_impl->tools.declarations()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::continuation_not_ready,
                       "run is not ready for another inference"));
    }
    std::set<domain::InvocationId> supplied;
    for (const auto& entry : request.context.entries) {
      if (entry.kind == domain::ContextEntryKind::tool_result &&
          entry.message.invocation_id) {
        supplied.insert(*entry.message.invocation_id);
      }
    }
    if (std::ranges::any_of(active.invocation_order,
                            [&](const auto& invocation_id) {
                              return !supplied.contains(invocation_id);
                            })) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::continuation_not_ready,
                       "continuation omits a terminal tool result"));
    }

    auto transaction = m_impl->transaction();
    transaction.active->inference_id = request.inference_id;
    transaction.active->assistant_message_id = request.assistant_message_id;
    transaction.active->invocations.clear();
    transaction.active->invocation_order.clear();
    transaction.active->cancel_requested = false;
    transaction.active->cancel_reason.reset();
    if (auto result = m_impl->record(
            run_id,
            domain::InferenceStarted{request.inference_id, request.model_id},
            transaction);
        !result) {
      return result;
    }
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    auto launched = m_impl->launch_backend(std::move(request));
    if (launched) return {};
    auto failure = m_impl->transaction();
    if (auto failed = m_impl->fail_live_run(failure, protocol_domain_error());
        !failed) {
      return failed;
    }
    if (auto committed = m_impl->commit(std::move(failure)); !committed) {
      return committed;
    }
    return launched;
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                        "run continuation failed internally"));
  }
}

auto RunKernel::drain()
    -> std::expected<std::vector<domain::RunEvent>, RunKernelError> {
  try {
    if (m_impl->unusable) {
      return std::unexpected(kernel_error(
          RunKernelErrorCode::storage_failure,
          "run kernel is unavailable after a persistence failure"));
    }
    std::deque<WorkerUpdate> updates;
    {
      std::lock_guard lock(m_impl->queue_mutex);
      updates.swap(m_impl->queue);
    }
    m_impl->queue_space.notify_all();

    std::vector<domain::RunEvent> committed;
    std::optional<RunKernelError> first_error;
    for (auto& update : updates) {
      auto transaction = m_impl->transaction();
      std::expected<void, RunKernelError> result;
      bool launch_ready{};
      if (auto* event = std::get_if<backend::BackendEvent>(&update)) {
        result = m_impl->process_backend_event(std::move(*event), transaction);
      } else if (auto* failure = std::get_if<BackendFailure>(&update)) {
        result = m_impl->process_backend_failure(std::move(failure->error),
                                                 transaction);
      } else if (std::holds_alternative<BackendEnded>(update)) {
        launch_ready = true;
        result = m_impl->process_backend_end(transaction);
      } else if (auto* tool = std::get_if<ToolUpdate>(&update)) {
        result = m_impl->process_tool_update(std::move(*tool), transaction);
      } else if (auto* failure = std::get_if<ToolFailure>(&update)) {
        result = m_impl->process_tool_failure(std::move(*failure), transaction);
      } else if (auto* expired = std::get_if<ToolDeadlineExpired>(&update)) {
        result = m_impl->process_tool_deadline(*expired, transaction);
      } else {
        launch_ready = true;
        result =
            m_impl->process_tool_end(std::get<ToolEnded>(update), transaction);
      }
      if (!result) {
        if (!first_error) first_error = std::move(result.error());
        continue;
      }
      auto events = transaction.events;
      auto persisted = m_impl->commit(std::move(transaction));
      if (!persisted) {
        first_error = std::move(persisted.error());
        break;
      }
      committed.insert(committed.end(), std::make_move_iterator(events.begin()),
                       std::make_move_iterator(events.end()));
      if (launch_ready) {
        if (auto launched = m_impl->launch_next_tool();
            !launched && !first_error) {
          first_error = std::move(launched.error());
        }
      }
    }
    if (first_error) return std::unexpected(std::move(*first_error));
    return committed;
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                        "run drain failed internally"));
  }
}

auto RunKernel::event_log() const noexcept -> const domain::SessionEventLog& {
  return m_impl->event_log;
}

auto RunKernel::projection(const domain::RunId& run_id) const noexcept
    -> const domain::RunProjection* {
  const auto found = m_impl->projections.find(run_id);
  return found == m_impl->projections.end() ? nullptr : &found->second;
}

auto RunKernel::active_run_id() const noexcept -> std::optional<domain::RunId> {
  return m_impl->active ? std::optional{m_impl->active->run_id} : std::nullopt;
}

auto RunKernel::active_inference_id() const noexcept
    -> std::optional<domain::InferenceId> {
  return m_impl->active ? m_impl->active->inference_id : std::nullopt;
}

auto RunKernel::pending_question_input() const
    -> std::optional<PendingQuestionInput> {
  if (!m_impl->active) return std::nullopt;
  for (const auto& invocation_id : m_impl->active->invocation_order) {
    const auto& invocation = m_impl->active->invocations.at(invocation_id);
    if (invocation.state == Impl::InvocationState::awaiting_input &&
        !invocation.terminal_event_seen) {
      return PendingQuestionInput{m_impl->active->run_id, invocation_id,
                                  invocation.questions};
    }
  }
  return std::nullopt;
}

}  // namespace aiforge::runtime
