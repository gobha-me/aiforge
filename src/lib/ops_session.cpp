#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/surfaces/ops_session.hpp>
#include <atomic>
#include <string>
#include <type_traits>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Failure = ManualOpsFailure;
using Code = ManualOpsErrorCode;
auto failure(Code code) -> std::unexpected<Failure> {
  return std::unexpected(Failure{code});
}
auto kernel_failure(runtime::RunKernelErrorCode code) -> Failure {
  using K = runtime::RunKernelErrorCode;
  auto result = Code::operation_failed;
  switch (code) {
    case K::invalid_limits:
    case K::invalid_start: result = Code::invalid_input; break;
    case K::run_already_active: result = Code::busy; break;
    case K::wrong_run:
    case K::wrong_invocation:
    case K::no_active_run:
    case K::already_terminal: result = Code::wrong_operation; break;
    case K::storage_failure: result = Code::storage_failure; break;
    case K::replay_rejected: result = Code::invalid_history; break;
    case K::internal_failure: result = Code::internal_failure; break;
    default: break;
  }
  return {result, code, {}};
}
auto is_fatal(const Failure& value) -> bool {
  return value.code == Code::storage_failure ||
         value.code == Code::invalid_history ||
         value.code == Code::resource_exhausted ||
         value.code == Code::internal_failure;
}
auto valid_id(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 256 &&
         detail::is_safe_utf8_text(value);
}
auto terminal(domain::RunStatus status) -> bool {
  return status == domain::RunStatus::completed ||
         status == domain::RunStatus::failed ||
         status == domain::RunStatus::cancelled;
}
class NoInferenceBackend final : public backend::Backend {
 public:
  std::atomic<std::uint64_t> attempted_calls{};
  auto start(backend::BackendRequest, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    ++attempted_calls;
    return std::unexpected(backend::BackendError{
        backend::BackendErrorKind::unavailable, {}, false, {}});
  }
};
} // namespace
struct OpsSession::Impl {
  domain::RunStarted attributes;
  std::shared_ptr<runtime::OpsObservationBroker> broker;
  OpsIdentitySuffixSource identities;
  // Reverse destruction joins the kernel before destroying its backend.
  NoInferenceBackend backend;
  std::unique_ptr<runtime::RunKernel> kernel;
  std::optional<ObservationSubmission> current;
  ManualOpsInspection inspection;
  bool unusable{};

  Impl(domain::RunStarted frozen,
       std::shared_ptr<runtime::OpsObservationBroker> owner_broker,
       OpsIdentitySuffixSource generator)
      : attributes(std::move(frozen)), broker(std::move(owner_broker)),
        identities(std::move(generator)) {}
  auto report(Failure value) -> std::unexpected<Failure> {
    // Preserve the first fatal outcome through later broker/cleanup failures.
    if (!unusable) inspection.problem = value;
    if (is_fatal(value)) {
      inspection.available = false;
      unusable = true;
    }
    return std::unexpected(inspection.problem.value_or(value));
  }
  auto synchronize() -> std::expected<void, Failure> {
    try {
      if (backend.attempted_calls != 0) return report({Code::internal_failure});
      const auto& log = kernel->event_log();
      const bool current_changed =
          current.has_value() != inspection.projection.current.has_value() ||
          (current && inspection.projection.current &&
           *current != inspection.projection.current->submission);
      if (log.last_sequence() != inspection.projection.last_sequence ||
          current_changed) {
        auto projected = project_manual_observations(log, current);
        if (!projected) return report(projected.error());
        inspection.projection = std::move(*projected);
      }
      inspection.busy = kernel->active_run_id().has_value();
      inspection.approval = kernel->pending_tool_approval();
      if (inspection.approval &&
          (!current || inspection.approval->run_id != current->run_id ||
           inspection.approval->invocation_id != current->invocation_id))
        return report({Code::invalid_history});
      return {};
    } catch (...) {
      return report({Code::internal_failure});
    }
  }
  auto cancel_current() -> std::expected<void, Failure> {
    if (!current || !kernel->active_run_id()) return {};
    const auto* projection = kernel->projection(current->run_id);
    if (projection != nullptr && terminal(projection->status())) return {};
    auto result = kernel->cancel_run(current->run_id);
    if (!result) return report(kernel_failure(result.error().code));
    return {};
  }
};
OpsSession::OpsSession(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {
}
OpsSession::~OpsSession() {
  if (m_impl && m_impl->kernel) {
    const auto closed = close();
    static_cast<void>(closed);
  }
}
auto OpsSession::open(OpsSessionOpen request, storage::SessionStore& store,
                      OpsSessionDependencies dependencies)
    -> std::expected<std::unique_ptr<OpsSession>, Failure> {
  try {
    if (!valid_id(request.session_id.value()) ||
        !valid_id(request.surface_id.value()) ||
        !valid_id(request.workspace_id.value()) ||
        !valid_id(dependencies.launch_policy.permission_profile_id.value()) ||
        !dependencies.identity_suffix_source)
      return failure(Code::invalid_input);
    if (!dependencies.broker) return failure(Code::unavailable);
    runtime::ToolRegistry initial;
    if (!initial.declare_unavailable_tool(
            "observe_target", runtime::ToolUnavailableReason::not_configured))
      return failure(Code::internal_failure);
    auto tools = initial.snapshot();
    if (!tools) return failure(Code::internal_failure);
    auto policy =
        runtime::make_tool_launch_policy(*tools, dependencies.launch_policy);
    if (!policy) return failure(Code::invalid_input);
    auto impl = std::make_unique<Impl>(
        domain::RunStarted{request.surface_id,
                           request.workspace_id,
                           dependencies.launch_policy.permission_profile_id,
                           {},
                           {},
                           domain::RunPurpose::control},
        dependencies.broker, std::move(dependencies.identity_suffix_source));
    auto candidate =
        std::unique_ptr<OpsSession>{new OpsSession{std::move(impl)}};
    auto opened = runtime::RunKernel::open_durable(
        {request.session_id, runtime::DurableSessionMode::create,
         request.created_at},
        store, candidate->m_impl->backend, dependencies.wake_sink,
        std::move(dependencies.timestamp_source), dependencies.run_limits,
        std::move(*tools), std::move(*policy), {},
        std::move(dependencies.broker));
    if (!opened) return std::unexpected(kernel_failure(opened.error().code));
    // All surface allocations precede durable creation. Unique ownership moves
    // and returning the prepared candidate are nonthrowing after kernel
    // success.
    candidate->m_impl->kernel = std::move(*opened);
    return candidate;
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto OpsSession::bind_observation(
    domain::OpsObservationAuthority authority,
    std::shared_ptr<runtime::OpsObservationSource> source,
    std::shared_ptr<runtime::OpsObservationEndpoint> endpoint)
    -> std::expected<void, Failure> {
  try {
    if (m_impl->inspection.closed) return failure(Code::closed);
    if (m_impl->unusable) return failure(Code::unavailable);
    std::optional<domain::OpsTargetBinding> visible{
        authority.specification().target};
    auto bound = m_impl->kernel->bind_ops_observation(
        std::move(authority), std::move(source), std::move(endpoint));
    if (!bound) return m_impl->report(kernel_failure(bound.error().code));
    static_assert(std::is_nothrow_move_assignable_v<decltype(visible)>);
    m_impl->inspection.selection = std::move(visible);
    m_impl->inspection.available = true;
    m_impl->inspection.problem.reset();
    return {};
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
auto OpsSession::submit_observation(runtime::OpsObservationIntent intent)
    -> std::expected<ObservationSubmission, Failure> {
  try {
    if (m_impl->inspection.closed) return failure(Code::closed);
    if (!m_impl->inspection.available) return failure(Code::unavailable);
    if (m_impl->kernel->active_run_id()) return failure(Code::busy);
    const auto suffix = m_impl->identities();
    if (suffix == 0) return failure(Code::invalid_input);
    auto run = domain::RunId::from("ops-run-" + std::to_string(suffix));
    auto invocation =
        domain::InvocationId::from("ops-invocation-" + std::to_string(suffix));
    if (!run || !invocation) return failure(Code::invalid_input);
    ObservationSubmission submitted{*run, *invocation};
    std::optional<ObservationSubmission> next{submitted};
    runtime::ObservationControlStart start{*run, m_impl->attributes,
                                           *invocation, std::move(intent)};
    const auto previous = m_impl->kernel->event_log().last_sequence();
    auto started = m_impl->kernel->start_observation_control(std::move(start));
    if (m_impl->kernel->event_log().last_sequence() != previous) {
      static_assert(std::is_nothrow_move_assignable_v<decltype(next)>);
      m_impl->current = std::move(next);
    }
    if (auto synced = m_impl->synchronize(); !synced)
      return std::unexpected(synced.error());
    if (!started) return m_impl->report(kernel_failure(started.error().code));
    m_impl->inspection.problem.reset();
    return submitted;
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
auto OpsSession::cancel_observation(const domain::RunId& run_id)
    -> std::expected<void, Failure> {
  try {
    if (!m_impl->current || m_impl->current->run_id != run_id)
      return failure(Code::wrong_operation);
    auto cancelled = m_impl->cancel_current();
    auto synced = m_impl->synchronize();
    if (!cancelled) return cancelled;
    return synced;
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
auto OpsSession::decide_observation_approval(
    const domain::RunId& run_id, const domain::InvocationId& invocation_id,
    runtime::ToolApprovalResolution decision) -> std::expected<void, Failure> {
  try {
    if (m_impl->inspection.closed) return failure(Code::closed);
    if (m_impl->unusable) return failure(Code::unavailable);
    if (!m_impl->current || m_impl->current->run_id != run_id ||
        m_impl->current->invocation_id != invocation_id ||
        !m_impl->inspection.approval)
      return failure(Code::wrong_operation);
    auto decided = m_impl->kernel->decide_approval(run_id, invocation_id,
                                                   std::move(decision));
    auto synced = m_impl->synchronize();
    if (!decided) return m_impl->report(kernel_failure(decided.error().code));
    return synced;
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
auto OpsSession::pump_observations() -> std::expected<void, Failure> {
  try {
    std::optional<Failure> first;
    const auto service = [&] {
      auto result = m_impl->broker->service();
      if (!result) {
        if (!first) first = Failure{Code::unavailable, {}, result.error().code};
        m_impl->inspection.available = false;
        const auto cancelled = m_impl->cancel_current();
        if (!cancelled && is_fatal(cancelled.error()))
          first = cancelled.error();
      }
    };
    service();
    auto drained = m_impl->kernel->drain();
    if (!drained && !first) first = kernel_failure(drained.error().code);
    service();
    auto synced = m_impl->synchronize();
    if (!synced && !first) first = synced.error();
    if (first) return m_impl->report(*first);
    return {};
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
auto OpsSession::inspect_observations() const noexcept
    -> const ManualOpsInspection& {
  return m_impl->inspection;
}
auto OpsSession::close() -> std::expected<void, Failure> {
  try {
    if (!m_impl->inspection.closed) {
      m_impl->inspection.closed = true;
      m_impl->inspection.available = false;
      auto cancelled = m_impl->cancel_current();
      auto synced = m_impl->synchronize();
      if (!cancelled) return cancelled;
      if (!synced) return synced;
    }
    // Successful cleanup cannot erase a prior failure to persist cancellation.
    if (m_impl->inspection.problem && is_fatal(*m_impl->inspection.problem))
      return std::unexpected(*m_impl->inspection.problem);
    return {};
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
} // namespace aiforge::surfaces
