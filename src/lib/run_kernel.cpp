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

[[nodiscard]] auto scope_is_declared(
    const domain::CapabilityScope& scope,
    const backend::ToolDeclaration& declaration) -> bool {
  return std::ranges::find(declaration.capability_scopes, scope) !=
         declaration.capability_scopes.end();
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
    std::vector<domain::CapabilityScope> granted_scopes;
    InvocationState state{InvocationState::proposed};
    std::size_t output_bytes{};
    std::size_t progress_events{};
    bool terminal_event_seen{};
  };

  struct ActiveRun {
    domain::RunId run_id;
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
       RunKernelLimits limits, ToolRegistrySnapshot tool_snapshot)
      : event_log(std::move(session_id)), backend_port(backend),
        wake(wake_sink),
        timestamp(timestamp_source ? std::move(timestamp_source)
                                   : TimestampSource{default_timestamp}),
        limits(limits), tools(std::move(tool_snapshot)) {}

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
                if (auto result = record_or_fail(
                        domain::ToolProposed{
                            invocation_id, assembly.name,
                            domain::StructuredDataBlock{"application/json",
                                                        assembly.arguments},
                            registration->declaration.effects, std::nullopt},
                        invocation_id);
                    !result) {
                  return result;
                }
                std::expected<ValidatedToolArguments, ToolExecutionError>
                    validated = std::unexpected(ToolExecutionError{
                        ToolExecutionErrorCode::internal_failure,
                        "tool validation failed internally", false});
                try {
                  validated = registration->executor->validate(
                      domain::StructuredDataBlock{"application/json",
                                                  assembly.arguments});
                } catch (...) {
                }
                if (validated &&
                    (validated->value.media_type != "application/json" ||
                     validated->value.data.empty() ||
                     validated->value.data.size() >
                         limits.tool_argument_bytes)) {
                  validated = std::unexpected(ToolExecutionError{
                      ToolExecutionErrorCode::protocol_failure,
                      "tool validator returned invalid normalized arguments",
                      false});
                }
                if (!validated) {
                  PendingInvocation failed{
                      invocation_id,
                      std::nullopt,
                      registration->declaration,
                      ValidatedToolArguments{domain::StructuredDataBlock{
                          "application/json", assembly.arguments}},
                      registration->limits,
                      registration->executor,
                      *message_id,
                      {},
                      InvocationState::proposed,
                      0,
                      0,
                      false};
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
                                        {},
                                        InvocationState::proposed,
                                        0,
                                        0,
                                        false});
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
    if (active->run_terminal && active->cancel_requested &&
        invocation.terminal_event_seen) {
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
          const auto bytes = content_bytes(event.content);
          if (!bytes ||
              invocation.output_bytes > invocation.limits.output_bytes ||
              *bytes >
                  invocation.limits.output_bytes - invocation.output_bytes) {
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
            auto result =
                record(active->run_id,
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
    if (!invocation.terminal_event_seen) {
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
                     RunKernelLimits limits, ToolRegistrySnapshot tools)
    : m_impl(std::make_unique<Impl>(std::move(session_id), backend, wake_sink,
                                    std::move(timestamp_source), limits,
                                    std::move(tools))) {
}

auto RunKernel::open_durable(DurableSessionOpen session,
                             storage::SessionStore& store,
                             backend::Backend& backend, RunWakeSink* wake_sink,
                             TimestampSource timestamp_source,
                             RunKernelLimits limits, ToolRegistrySnapshot tools)
    -> std::expected<std::unique_ptr<RunKernel>, RunKernelError> {
  try {
    auto kernel = std::unique_ptr<RunKernel>{
        new RunKernel(session.session_id, backend, wake_sink,
                      std::move(timestamp_source), limits, std::move(tools))};
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

    Impl::ActiveRun active{start.run_id,
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

auto RunKernel::decide_tool(const domain::RunId& run_id,
                            const domain::InvocationId& invocation_id,
                            ToolPolicyResolution resolution)
    -> std::expected<void, RunKernelError> {
  try {
    if (!m_impl->active || m_impl->active->run_id != run_id) {
      return std::unexpected(
          kernel_error(m_impl->active ? RunKernelErrorCode::wrong_run
                                      : RunKernelErrorCode::no_active_run,
                       "tool decision targets no active run"));
    }
    if (m_impl->active->inference_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "tool policy waits for the inference boundary"));
    }
    const auto found = m_impl->active->invocations.find(invocation_id);
    if (found == m_impl->active->invocations.end()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_invocation,
                       "tool decision targets an unknown invocation"));
    }
    if (found->second.state != Impl::InvocationState::proposed) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "tool policy may be decided only once"));
    }
    if (!scopes_are_unique(resolution.scopes) ||
        (resolution.redacted_reason &&
         (resolution.redacted_reason->size() > 4096 ||
          has_control_character(*resolution.redacted_reason)))) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "tool policy decision is malformed"));
    }
    if (std::ranges::any_of(resolution.scopes, [&](const auto& scope) {
          return !scope_is_declared(scope, found->second.declaration);
        })) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::policy_scope_widening,
                       "tool policy cannot widen declared capability scopes"));
    }

    auto transaction = m_impl->transaction();
    auto& invocation = transaction.active->invocations.at(invocation_id);
    if (auto result =
            m_impl->record(run_id,
                           domain::ToolPolicyDecided{
                               invocation_id, resolution.decision,
                               resolution.scopes,
                               std::move(resolution.redacted_reason)},
                           transaction, invocation_id);
        !result) {
      return result;
    }
    switch (resolution.decision) {
      case domain::PolicyDecision::allow:
        invocation.granted_scopes = std::move(resolution.scopes);
        invocation.state = Impl::InvocationState::allowed;
        break;
      case domain::PolicyDecision::deny:
        if (auto result = m_impl->record_tool_error(transaction, invocation,
                                                    policy_denied_error());
            !result) {
          return result;
        }
        break;
      case domain::PolicyDecision::require_approval:
        if (auto result = m_impl->record(
                run_id,
                domain::ToolApprovalRequested{invocation_id, resolution.scopes},
                transaction, invocation_id);
            !result) {
          return result;
        }
        invocation.granted_scopes = std::move(resolution.scopes);
        invocation.state = Impl::InvocationState::awaiting_approval;
        break;
    }
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    return m_impl->launch_next_tool();
  } catch (...) {
    return std::unexpected(
        kernel_error(RunKernelErrorCode::internal_failure,
                     "tool policy decision failed internally"));
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
    if (!scopes_are_unique(resolution.granted_scopes)) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_tool_state,
                       "approval decision contains duplicate scopes"));
    }
    if (std::ranges::any_of(resolution.granted_scopes, [&](const auto& scope) {
          return std::ranges::find(found->second.granted_scopes, scope) ==
                 found->second.granted_scopes.end();
        })) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::policy_scope_widening,
                       "approval cannot widen requested capability scopes"));
    }

    auto transaction = m_impl->transaction();
    auto& invocation = transaction.active->invocations.at(invocation_id);
    if (auto result = m_impl->record(
            run_id,
            domain::ToolApprovalDecided{invocation_id, resolution.decision,
                                        resolution.granted_scopes},
            transaction, invocation_id);
        !result) {
      return result;
    }
    if (resolution.decision == domain::ApprovalDecision::approved) {
      invocation.granted_scopes = std::move(resolution.granted_scopes);
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
      bool tool_ended{};
      if (auto* event = std::get_if<backend::BackendEvent>(&update)) {
        result = m_impl->process_backend_event(std::move(*event), transaction);
      } else if (auto* failure = std::get_if<BackendFailure>(&update)) {
        result = m_impl->process_backend_failure(std::move(failure->error),
                                                 transaction);
      } else if (std::holds_alternative<BackendEnded>(update)) {
        result = m_impl->process_backend_end(transaction);
      } else if (auto* tool = std::get_if<ToolUpdate>(&update)) {
        result = m_impl->process_tool_update(std::move(*tool), transaction);
      } else if (auto* failure = std::get_if<ToolFailure>(&update)) {
        result = m_impl->process_tool_failure(std::move(*failure), transaction);
      } else if (auto* expired = std::get_if<ToolDeadlineExpired>(&update)) {
        result = m_impl->process_tool_deadline(*expired, transaction);
      } else {
        tool_ended = true;
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
      if (tool_ended) {
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

}  // namespace aiforge::runtime
