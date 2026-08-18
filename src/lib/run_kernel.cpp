#include <aiforge/runtime/run_kernel.hpp>

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
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

struct StreamEnded {};

using WorkerUpdate =
    std::variant<backend::BackendEvent, BackendFailure, StreamEnded>;

[[nodiscard]] auto default_timestamp() -> domain::EventTimestamp {
  return std::chrono::floor<std::chrono::milliseconds>(
      std::chrono::system_clock::now());
}

[[nodiscard]] auto kernel_error(const RunKernelErrorCode code,
                                std::string message,
                                const bool retryable = false) -> RunKernelError {
  return RunKernelError{code, std::move(message), retryable};
}

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  for (const unsigned char character : value) {
    if (character < 0x20U || character == 0x7FU) return true;
  }
  return false;
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

[[nodiscard]] auto protocol_domain_error() -> domain::DomainError {
  return {domain::ErrorCode::invalid_state, "backend stream protocol failure",
          false};
}

}  // namespace

struct RunKernel::Impl {
  struct ToolAssembly {
    std::string name;
    std::string arguments;
  };

  struct ActiveRun {
    domain::RunId run_id;
    domain::InferenceId inference_id;
    domain::MessageId assistant_message_id;
    std::map<std::string, std::vector<domain::Effect>> declared_tools;
    std::map<domain::InvocationId, ToolAssembly> tool_calls;
    bool response_started{};
    bool terminal_seen{};
    bool cancellation_ack_expected{};
    bool cancel_requested{};
    std::optional<std::string> cancel_reason;
  };

  struct Transaction {
    domain::SessionEventLog event_log;
    std::map<domain::RunId, domain::RunProjection> projections;
    std::optional<ActiveRun> active;
    std::vector<domain::RunEvent> events;
  };

  Impl(domain::SessionId session_id, backend::Backend& backend,
       RunWakeSink* wake_sink, TimestampSource timestamp_source,
       RunKernelLimits limits)
      : event_log(std::move(session_id)),
        backend_port(backend),
        wake(wake_sink),
        timestamp(timestamp_source ? std::move(timestamp_source)
                                   : TimestampSource{default_timestamp}),
        limits(limits) {}

  [[nodiscard]] auto transaction() const -> Transaction {
    return {event_log, projections, active, {}};
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
        worker.request_stop();
        active.reset();
        unusable = true;
        return std::unexpected(kernel_error(
            RunKernelErrorCode::storage_failure,
            "session event persistence failed", appended.error().retryable));
      }
    }
    event_log = std::move(transaction.event_log);
    projections = std::move(transaction.projections);
    active = std::move(transaction.active);
    return {};
  }

  ~Impl() {
    {
      std::lock_guard lock(queue_mutex);
      queue_closed = true;
    }
    queue_space.notify_all();
    worker.request_stop();
    if (worker.joinable()) worker.join();
  }

  [[nodiscard]] auto valid_limits() const noexcept -> bool {
    return limits.pending_updates != 0 && limits.tool_argument_bytes != 0;
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
      static_cast<void>(push(StreamEnded{}));
      return;
    }

    for (;;) {
      auto next = (*started)->next(stop_token);
      if (!next) {
        static_cast<void>(push(BackendFailure{std::move(next.error())}));
        static_cast<void>(push(StreamEnded{}));
        return;
      }
      if (!*next) {
        static_cast<void>(push(StreamEnded{}));
        return;
      }
      if (!push(std::move(**next))) return;
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
    return domain::RunEvent{
        domain::EventMetadata{std::move(*event_id), run_id, sequence, 1,
                              timestamp(), std::nullopt, std::nullopt,
                              std::move(invocation_id)},
        std::move(payload)};
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
    auto projected = projection_candidate.apply(*event);
    if (!projected) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::projection_rejected,
                       "run projection rejected a generated event"));
    }

    auto appended = transaction.event_log.append(*event);
    if (!appended) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::event_log_rejected,
                       "session event log rejected a generated event"));
    }

    transaction.projections.insert_or_assign(
        run_id, std::move(projection_candidate));
    transaction.events.push_back(std::move(*event));
    return {};
  }

  [[nodiscard]] auto fail_live_run(Transaction& transaction,
                                   domain::DomainError error)
      -> std::expected<void, RunKernelError> {
    if (!transaction.active || transaction.active->terminal_seen) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "event followed a terminal backend event"));
    }
    const auto run_id = transaction.active->run_id;
    const auto inference_id = transaction.active->inference_id;
    if (auto result = record(
            run_id, domain::InferenceFailed{inference_id, error}, transaction);
        !result) {
      return result;
    }
    if (auto result =
            record(run_id, domain::RunFailed{std::move(error)}, transaction);
        !result) {
      return result;
    }
    transaction.active->terminal_seen = true;
    transaction.active->cancellation_ack_expected = true;
    worker.request_stop();
    return {};
  }

  [[nodiscard]] auto process_event(backend::BackendEvent event,
                                   Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active) {
      return std::unexpected(kernel_error(RunKernelErrorCode::no_active_run,
                                          "backend event has no active run"));
    }
    if (active->terminal_seen) {
      if (active->cancellation_ack_expected &&
          std::holds_alternative<backend::ResponseCancelled>(event)) {
        active->cancellation_ack_expected = false;
        return {};
      }
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "backend event followed a terminal event"));
    }

    const auto record_or_fail =
        [&](domain::RunEventPayload payload,
            std::optional<domain::InvocationId> invocation =
                std::nullopt) -> std::expected<void, RunKernelError> {
          auto recorded = record(active->run_id, std::move(payload),
                                 transaction, std::move(invocation));
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
                active->assistant_message_id, active->inference_id});
          } else if constexpr (std::same_as<Value, backend::ContentDelta>) {
            if (!active->response_started ||
                value.message_id != active->assistant_message_id) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(domain::AssistantContentDeltaAdded{
                value.message_id, active->inference_id,
                std::move(value.delta)});
          } else if constexpr (std::same_as<Value, backend::ReasoningDelta>) {
            if (!active->response_started) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(domain::ReasoningMetadataAdded{
                active->inference_id, std::move(value.text),
                std::move(value.metadata)});
          } else if constexpr (std::same_as<Value, backend::CitationObserved>) {
            if (!active->response_started) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(domain::AssistantContentDeltaAdded{
                active->assistant_message_id, active->inference_id,
                std::move(value.citation)});
          } else if constexpr (std::same_as<Value, backend::UsageObserved>) {
            if (!active->response_started) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            return record_or_fail(
                domain::UsageRecorded{active->inference_id, value.usage});
          } else if constexpr (std::same_as<Value, backend::ToolCallDelta>) {
            if (!active->response_started || value.tool_name.size() > 256 ||
                has_control_character(value.tool_name)) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            auto found = active->tool_calls.find(value.invocation_id);
            if (found == active->tool_calls.end()) {
              if (value.tool_name.empty() ||
                  !active->declared_tools.contains(value.tool_name)) {
                return fail_live_run(transaction, protocol_domain_error());
              }
              found = active->tool_calls
                          .emplace(value.invocation_id,
                                   ToolAssembly{value.tool_name, {}})
                          .first;
            } else if (!value.tool_name.empty() &&
                       found->second.name != value.tool_name) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            if (value.arguments_fragment.size() >
                limits.tool_argument_bytes - found->second.arguments.size()) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            found->second.arguments.append(value.arguments_fragment);
            return {};
          } else if constexpr (std::same_as<Value, backend::ResponseFinished>) {
            if (!active->response_started ||
                (value.reason == domain::FinishReason::tool_call) !=
                    !active->tool_calls.empty()) {
              return fail_live_run(transaction, protocol_domain_error());
            }
            for (auto& [invocation_id, assembly] : active->tool_calls) {
              auto effects = active->declared_tools.at(assembly.name);
              if (auto result = record_or_fail(
                      domain::ToolProposed{invocation_id,
                                           std::move(assembly.name),
                                           domain::StructuredDataBlock{
                                               "application/json",
                                               std::move(assembly.arguments)},
                                           std::move(effects)},
                      invocation_id);
                  !result) {
                return result;
              }
            }
            if (auto result = record_or_fail(domain::AssistantContentFinished{
                    active->assistant_message_id, active->inference_id});
                !result) {
              return result;
            }
            if (auto result = record_or_fail(domain::InferenceFinished{
                    active->inference_id, value.reason});
                !result) {
              return result;
            }
            if (auto result = record_or_fail(domain::RunCompleted{}); !result) {
              return result;
            }
            active->terminal_seen = true;
            return {};
          } else if constexpr (std::same_as<Value,
                                            backend::ResponseCancelled>) {
            auto reason = active->cancel_requested ? active->cancel_reason
                                                   : std::move(value.reason);
            if (auto result = record_or_fail(
                    domain::InferenceCancelled{active->inference_id, reason});
                !result) {
              return result;
            }
            if (auto result = record_or_fail(domain::RunCancelled{reason});
                !result) {
              return result;
            }
            active->terminal_seen = true;
            return {};
          }
        },
        std::move(event));
  }

  [[nodiscard]] auto process_failure(backend::BackendError error,
                                     Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "backend failure followed termination"));
    }
    if (active->terminal_seen) {
      if (active->cancellation_ack_expected &&
          error.kind == backend::BackendErrorKind::cancelled) {
        active->cancellation_ack_expected = false;
        return {};
      }
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "backend failure followed termination"));
    }
    if (error.kind == backend::BackendErrorKind::cancelled &&
        active->cancel_requested) {
      const auto reason = active->cancel_reason;
      if (auto result =
              record(active->run_id,
                     domain::InferenceCancelled{active->inference_id, reason},
                     transaction);
          !result) {
        return result;
      }
      if (auto result =
              record(active->run_id, domain::RunCancelled{reason}, transaction);
          !result) {
        return result;
      }
      active->terminal_seen = true;
      return {};
    }
    return fail_live_run(transaction, backend_domain_error(error));
  }

  [[nodiscard]] auto process_end(Transaction& transaction)
      -> std::expected<void, RunKernelError> {
    auto& active = transaction.active;
    if (!active) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::no_active_run,
                       "stream ended without an active run"));
    }
    if (!active->terminal_seen) {
      if (auto failed = fail_live_run(transaction, protocol_domain_error());
          !failed) {
        return failed;
      }
    }
    active.reset();
    if (worker.joinable()) worker.join();
    return {};
  }

  domain::SessionEventLog event_log;
  std::map<domain::RunId, domain::RunProjection> projections;
  backend::Backend& backend_port;
  RunWakeSink* wake;
  TimestampSource timestamp;
  RunKernelLimits limits;
  storage::SessionStore* session_store{};
  std::optional<ActiveRun> active;
  bool unusable{};
  std::jthread worker;

  std::mutex queue_mutex;
  std::condition_variable queue_space;
  std::deque<WorkerUpdate> queue;
  bool queue_closed{};
};

RunKernel::RunKernel(domain::SessionId session_id, backend::Backend& backend,
                     RunWakeSink* wake_sink, TimestampSource timestamp_source,
                     RunKernelLimits limits)
    : m_impl(std::make_unique<Impl>(std::move(session_id), backend, wake_sink,
                                    std::move(timestamp_source), limits)) {}

auto RunKernel::open_durable(DurableSessionOpen session,
                             storage::SessionStore& store,
                             backend::Backend& backend,
                             RunWakeSink* wake_sink,
                             TimestampSource timestamp_source,
                             RunKernelLimits limits)
    -> std::expected<std::unique_ptr<RunKernel>, RunKernelError> {
  try {
    auto kernel = std::unique_ptr<RunKernel>{new RunKernel(
        session.session_id, backend, wake_sink, std::move(timestamp_source),
        limits)};
    if (!kernel->m_impl->valid_limits()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_limits,
                       "run-kernel limits must be positive"));
    }

    if (session.mode == DurableSessionMode::create) {
      auto created = store.create_session(
          {session.session_id, session.created_at});
      if (!created) {
        return std::unexpected(kernel_error(
            RunKernelErrorCode::storage_failure,
            "durable session creation failed", created.error().retryable));
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
        return std::unexpected(kernel_error(
            RunKernelErrorCode::storage_failure,
            "durable session replay failed", events.error().retryable));
      }

      domain::SessionEventLog event_log{session.session_id};
      std::map<domain::RunId, domain::RunProjection> projections;
      for (const auto& event : *events) {
        auto projection = projections.contains(event.metadata.run_id)
                              ? projections.at(event.metadata.run_id)
                              : domain::RunProjection{};
        if (!projection.apply(event) || !event_log.append(event)) {
          return std::unexpected(kernel_error(
              RunKernelErrorCode::replay_rejected,
              "durable session events could not rebuild projections"));
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
    }
    kernel->m_impl->session_store = &store;
    return kernel;
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
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
        start.request.assistant_message_id == start.user_message.message_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::invalid_start,
                       "run start contains invalid message identity"));
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
                           false,
                           false,
                           false,
                           false,
                           std::nullopt};
    for (const auto& tool : start.request.tools) {
      if (tool.name.empty() || has_control_character(tool.name) ||
          !active.declared_tools.emplace(tool.name, tool.effects).second) {
        return std::unexpected(kernel_error(RunKernelErrorCode::invalid_start,
                                            "tool declarations are invalid"));
      }
    }

    auto transaction = m_impl->transaction();
    if (auto result =
            m_impl->record(start.run_id, std::move(start.attributes),
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
    if (auto result = m_impl->record(start.run_id,
                                     domain::RunCompletionRequested{},
                                     transaction);
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

    transaction.active = active;
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }

    if (m_impl->worker.joinable()) m_impl->worker.join();
    try {
      m_impl->worker = std::jthread(
          [impl = m_impl.get(), request = std::move(start.request)](
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
                static_cast<void>(impl->push(StreamEnded{}));
              } catch (...) {
              }
            }
          });
    } catch (...) {
      auto error = protocol_domain_error();
      auto failure = m_impl->transaction();
      auto failed = m_impl->fail_live_run(failure, error);
      if (!failed) return failed;
      if (auto committed = m_impl->commit(std::move(failure)); !committed) {
        return committed;
      }
      return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                          "could not start backend worker"));
    }
    return {};
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
    if (!m_impl->active) {
      return std::unexpected(kernel_error(RunKernelErrorCode::no_active_run,
                                          "there is no active run"));
    }
    if (m_impl->active->run_id != run_id) {
      return std::unexpected(kernel_error(RunKernelErrorCode::wrong_run,
                                          "cancellation targets another run"));
    }
    if (m_impl->active->inference_id != inference_id) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::wrong_inference,
                       "cancellation targets another inference"));
    }
    if (m_impl->active->terminal_seen) {
      return std::unexpected(kernel_error(RunKernelErrorCode::already_terminal,
                                          "the run is already terminal"));
    }
    if (m_impl->active->cancel_requested) return {};

    auto transaction = m_impl->transaction();
    if (auto result =
            m_impl->record(run_id, domain::RunCancelRequested{reason},
                           transaction);
        !result) {
      return result;
    }
    transaction.active->cancel_requested = true;
    transaction.active->cancel_reason = std::move(reason);
    if (auto committed = m_impl->commit(std::move(transaction)); !committed) {
      return committed;
    }
    m_impl->worker.request_stop();
    return {};
  } catch (...) {
    return std::unexpected(kernel_error(RunKernelErrorCode::internal_failure,
                                        "run cancellation failed internally"));
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
      if (auto* event = std::get_if<backend::BackendEvent>(&update)) {
        result = m_impl->process_event(std::move(*event), transaction);
      } else if (auto* failure = std::get_if<BackendFailure>(&update)) {
        result =
            m_impl->process_failure(std::move(failure->error), transaction);
      } else {
        result = m_impl->process_end(transaction);
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
      committed.insert(committed.end(),
                       std::make_move_iterator(events.begin()),
                       std::make_move_iterator(events.end()));
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
  return m_impl->active ? std::optional{m_impl->active->inference_id}
                        : std::nullopt;
}

}  // namespace aiforge::runtime
