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
                                std::string message) -> RunKernelError {
  return RunKernelError{code, std::move(message)};
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
    bool cancel_requested{};
    std::optional<std::string> cancel_reason;
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
      const domain::RunId& run_id, domain::RunEventPayload payload,
      std::optional<domain::InvocationId> invocation_id = std::nullopt)
      -> std::expected<domain::RunEvent, RunKernelError> {
    if (event_log.last_sequence() ==
        std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::event_sequence_overflow,
                       "session event sequence overflow"));
    }
    const auto sequence = event_log.last_sequence() + 1;
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
      std::vector<domain::RunEvent>& committed,
      std::optional<domain::InvocationId> invocation_id = std::nullopt)
      -> std::expected<void, RunKernelError> {
    auto event =
        make_event(run_id, std::move(payload), std::move(invocation_id));
    if (!event) return std::unexpected(std::move(event.error()));

    auto projection_candidate = projections.contains(run_id)
                                    ? projections.at(run_id)
                                    : domain::RunProjection{};
    auto projected = projection_candidate.apply(*event);
    if (!projected) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::projection_rejected,
                       "run projection rejected a generated event"));
    }

    auto log_candidate = event_log;
    auto appended = log_candidate.append(*event);
    if (!appended) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::event_log_rejected,
                       "session event log rejected a generated event"));
    }

    event_log = std::move(log_candidate);
    projections.insert_or_assign(run_id, std::move(projection_candidate));
    committed.push_back(std::move(*event));
    return {};
  }

  [[nodiscard]] auto fail_live_run(std::vector<domain::RunEvent>& committed,
                                   domain::DomainError error)
      -> std::expected<void, RunKernelError> {
    if (!active || active->terminal_seen) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "event followed a terminal backend event"));
    }
    const auto run_id = active->run_id;
    const auto inference_id = active->inference_id;
    if (auto result = record(
            run_id, domain::InferenceFailed{inference_id, error}, committed);
        !result) {
      return result;
    }
    if (auto result =
            record(run_id, domain::RunFailed{std::move(error)}, committed);
        !result) {
      return result;
    }
    active->terminal_seen = true;
    worker.request_stop();
    return {};
  }

  [[nodiscard]] auto process_event(backend::BackendEvent event,
                                   std::vector<domain::RunEvent>& committed)
      -> std::expected<void, RunKernelError> {
    if (!active) {
      return std::unexpected(kernel_error(RunKernelErrorCode::no_active_run,
                                          "backend event has no active run"));
    }
    if (active->terminal_seen) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::protocol_failure,
                       "backend event followed a terminal event"));
    }

    const auto record_or_fail =
        [&](domain::RunEventPayload payload,
            std::optional<domain::InvocationId> invocation =
                std::nullopt) -> std::expected<void, RunKernelError> {
      auto recorded = record(active->run_id, std::move(payload), committed,
                             std::move(invocation));
      if (recorded) return {};
      return fail_live_run(committed, protocol_domain_error());
    };

    return std::visit(
        [&](auto&& value) -> std::expected<void, RunKernelError> {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<Value, backend::ResponseStarted>) {
            if (active->response_started || value.response_id.size() > 4096 ||
                has_control_character(value.response_id)) {
              return fail_live_run(committed, protocol_domain_error());
            }
            active->response_started = true;
            return record_or_fail(domain::AssistantContentStarted{
                active->assistant_message_id, active->inference_id});
          } else if constexpr (std::same_as<Value, backend::ContentDelta>) {
            if (!active->response_started ||
                value.message_id != active->assistant_message_id) {
              return fail_live_run(committed, protocol_domain_error());
            }
            return record_or_fail(domain::AssistantContentDeltaAdded{
                value.message_id, active->inference_id,
                std::move(value.delta)});
          } else if constexpr (std::same_as<Value, backend::ReasoningDelta>) {
            if (!active->response_started) {
              return fail_live_run(committed, protocol_domain_error());
            }
            return record_or_fail(domain::ReasoningMetadataAdded{
                active->inference_id, std::move(value.text),
                std::move(value.metadata)});
          } else if constexpr (std::same_as<Value, backend::CitationObserved>) {
            if (!active->response_started) {
              return fail_live_run(committed, protocol_domain_error());
            }
            return record_or_fail(domain::AssistantContentDeltaAdded{
                active->assistant_message_id, active->inference_id,
                std::move(value.citation)});
          } else if constexpr (std::same_as<Value, backend::UsageObserved>) {
            if (!active->response_started) {
              return fail_live_run(committed, protocol_domain_error());
            }
            return record_or_fail(
                domain::UsageRecorded{active->inference_id, value.usage});
          } else if constexpr (std::same_as<Value, backend::ToolCallDelta>) {
            if (!active->response_started || value.tool_name.size() > 256 ||
                has_control_character(value.tool_name)) {
              return fail_live_run(committed, protocol_domain_error());
            }
            auto found = active->tool_calls.find(value.invocation_id);
            if (found == active->tool_calls.end()) {
              if (value.tool_name.empty() ||
                  !active->declared_tools.contains(value.tool_name)) {
                return fail_live_run(committed, protocol_domain_error());
              }
              found = active->tool_calls
                          .emplace(value.invocation_id,
                                   ToolAssembly{value.tool_name, {}})
                          .first;
            } else if (!value.tool_name.empty() &&
                       found->second.name != value.tool_name) {
              return fail_live_run(committed, protocol_domain_error());
            }
            if (value.arguments_fragment.size() >
                limits.tool_argument_bytes - found->second.arguments.size()) {
              return fail_live_run(committed, protocol_domain_error());
            }
            found->second.arguments.append(value.arguments_fragment);
            return {};
          } else if constexpr (std::same_as<Value, backend::ResponseFinished>) {
            if (!active->response_started ||
                (value.reason == domain::FinishReason::tool_call) !=
                    !active->tool_calls.empty()) {
              return fail_live_run(committed, protocol_domain_error());
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
                                     std::vector<domain::RunEvent>& committed)
      -> std::expected<void, RunKernelError> {
    if (!active || active->terminal_seen) {
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
                     committed);
          !result) {
        return result;
      }
      if (auto result =
              record(active->run_id, domain::RunCancelled{reason}, committed);
          !result) {
        return result;
      }
      active->terminal_seen = true;
      return {};
    }
    return fail_live_run(committed, backend_domain_error(error));
  }

  [[nodiscard]] auto process_end(std::vector<domain::RunEvent>& committed)
      -> std::expected<void, RunKernelError> {
    if (!active) {
      return std::unexpected(
          kernel_error(RunKernelErrorCode::no_active_run,
                       "stream ended without an active run"));
    }
    if (!active->terminal_seen) {
      if (auto failed = fail_live_run(committed, protocol_domain_error());
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
  std::optional<ActiveRun> active;
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

RunKernel::~RunKernel() = default;

auto RunKernel::start(RunStart start) -> std::expected<void, RunKernelError> {
  try {
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
                           std::nullopt};
    for (const auto& tool : start.request.tools) {
      if (tool.name.empty() || has_control_character(tool.name) ||
          !active.declared_tools.emplace(tool.name, tool.effects).second) {
        return std::unexpected(kernel_error(RunKernelErrorCode::invalid_start,
                                            "tool declarations are invalid"));
      }
    }

    std::vector<domain::RunEvent> ignored;
    if (auto result =
            m_impl->record(start.run_id, std::move(start.attributes), ignored);
        !result) {
      return result;
    }
    if (auto result = m_impl->record(
            start.run_id,
            domain::UserContentAdded{std::move(start.user_message)}, ignored);
        !result) {
      return result;
    }
    if (auto result = m_impl->record(start.run_id,
                                     domain::RunCompletionRequested{}, ignored);
        !result) {
      return result;
    }
    if (auto result =
            m_impl->record(start.run_id,
                           domain::InferenceStarted{start.request.inference_id,
                                                    start.request.model_id},
                           ignored);
        !result) {
      return result;
    }

    if (m_impl->worker.joinable()) m_impl->worker.join();
    m_impl->active = std::move(active);
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
      auto failed = m_impl->fail_live_run(ignored, error);
      if (!failed) return failed;
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

    std::vector<domain::RunEvent> ignored;
    if (auto result =
            m_impl->record(run_id, domain::RunCancelRequested{reason}, ignored);
        !result) {
      return result;
    }
    m_impl->active->cancel_requested = true;
    m_impl->active->cancel_reason = std::move(reason);
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
    std::deque<WorkerUpdate> updates;
    {
      std::lock_guard lock(m_impl->queue_mutex);
      updates.swap(m_impl->queue);
    }
    m_impl->queue_space.notify_all();

    std::vector<domain::RunEvent> committed;
    std::optional<RunKernelError> first_error;
    for (auto& update : updates) {
      std::expected<void, RunKernelError> result;
      if (auto* event = std::get_if<backend::BackendEvent>(&update)) {
        result = m_impl->process_event(std::move(*event), committed);
      } else if (auto* failure = std::get_if<BackendFailure>(&update)) {
        result = m_impl->process_failure(std::move(failure->error), committed);
      } else {
        result = m_impl->process_end(committed);
      }
      if (!result && !first_error) first_error = std::move(result.error());
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
