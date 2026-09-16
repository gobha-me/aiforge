#include <aiforge/detail/admin_input.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/surfaces/admin_controller.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Code = ManualOpsErrorCode;
using Failure = ManualOpsFailure;
using SourceError = runtime::OpsObservationSourceError;
using WorkerCode = runtime::LocalSourceWorkerErrorCode;
auto failure(Code code) -> std::unexpected<Failure> {
  return std::unexpected(Failure{code});
}
auto fatal(const Failure& value) -> bool {
  return value.code == Code::storage_failure ||
         value.code == Code::invalid_history ||
         value.code == Code::resource_exhausted ||
         value.code == Code::internal_failure;
}
auto valid_id(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 256 &&
         detail::is_safe_utf8_text(value);
}
auto terminal(domain::RunStatus value) -> bool {
  return value == domain::RunStatus::completed ||
         value == domain::RunStatus::failed ||
         value == domain::RunStatus::cancelled;
}
auto worker_failure(WorkerCode code) -> Failure {
  switch (code) {
    case WorkerCode::busy: return {Code::busy};
    case WorkerCode::resource_exhausted: return {Code::resource_exhausted};
    case WorkerCode::internal_failure: return {Code::internal_failure};
    default: return {Code::operation_failed};
  }
}
auto snapshot_index(domain::OpsObservationOperation operation)
    -> std::optional<std::size_t> {
  const auto slot = manual_ops_catalog_slot(operation);
  return slot < manual_ops_catalog_slots ? std::optional{slot} : std::nullopt;
}
auto operation_matches(domain::OpsTargetKind kind,
                       domain::OpsObservationOperation operation) -> bool {
  switch (kind) {
    case domain::OpsTargetKind::linux_local:
      return operation == domain::OpsObservationOperation::linux_health ||
             operation == domain::OpsObservationOperation::linux_services ||
             operation == domain::OpsObservationOperation::linux_service_health;
    case domain::OpsTargetKind::kubernetes:
      return operation ==
                 domain::OpsObservationOperation::kubernetes_workloads ||
             operation ==
                 domain::OpsObservationOperation::kubernetes_pod_health ||
             operation == domain::OpsObservationOperation::kubernetes_events;
    case domain::OpsTargetKind::ceph: return false;
  }
  return false;
}
auto disconnected(AdminState& state) noexcept -> void {
  for (std::size_t slot{}; slot < state.snapshots.size(); ++slot) {
    if (state.freshness[slot] == AdminEvidenceFreshness::historical_unverified)
      continue;
    state.freshness[slot] = state.snapshots[slot]
                                ? AdminEvidenceFreshness::disconnected
                                : AdminEvidenceFreshness::unavailable;
  }
}
auto detached(AdminState& state) noexcept -> void {
  for (std::size_t slot{}; slot < state.snapshots.size(); ++slot) {
    if (state.freshness[slot] != AdminEvidenceFreshness::refreshing) continue;
    state.freshness[slot] = state.snapshots[slot]
                                ? AdminEvidenceFreshness::last_success
                                : AdminEvidenceFreshness::unavailable;
  }
}
auto validate_catalog(std::span<const AdminTargetChoice> targets) -> bool {
  if (targets.empty() || targets.size() > 33) return false;
  for (std::size_t index{}; index < targets.size(); ++index) {
    const auto& target = targets[index];
    if (!detail::valid_admin_target(target.id.value()) ||
        target.display_name.empty() || target.display_name.size() > 128 ||
        !detail::is_safe_utf8_text(target.display_name) ||
        target.display_name.find_first_of("\r\n\t") != std::string::npos)
      return false;
    switch (target.kind) {
      case domain::OpsTargetKind::linux_local:
      case domain::OpsTargetKind::kubernetes:
      case domain::OpsTargetKind::ceph: break;
      default: return false;
    }
    for (std::size_t earlier{}; earlier < index; ++earlier)
      if (targets[earlier].id == target.id) return false;
  }
  return true;
}
struct HistoricalCandidate {
  std::array<std::optional<CommittedOpsObservation>, admin_snapshot_count>
      snapshots{};
  std::array<AdminEvidenceFreshness, admin_snapshot_count> freshness{};
  std::optional<domain::OpsTargetBinding> target{};
  std::uint64_t selection_generation{};
};
struct HistoricalCatalogState {
  std::size_t bytes{};
  std::uint64_t maximum_generation{};
  bool present{};
};
auto retain_historical_evidence(const domain::OpsOwnerId& owner,
                                const domain::SessionId& session,
                                std::size_t slot,
                                const CommittedOpsObservation& evidence,
                                HistoricalCandidate& candidate,
                                HistoricalCatalogState& state)
    -> std::expected<void, Failure> {
  const auto& request = evidence.observation.request;
  const auto usage =
      domain::validate_recorded_ops_observation(evidence.observation);
  if (manual_ops_catalog_slot(request.operation) != slot ||
      request.owner_id != owner || request.session_id != session ||
      !valid_id(evidence.submission.run_id.value()) ||
      !valid_id(evidence.submission.invocation_id.value()) ||
      !valid_id(evidence.observation_event_id.value()) ||
      !valid_id(evidence.result_event_id.value()) ||
      evidence.observation_event_id == evidence.result_event_id)
    return failure(Code::invalid_history);
  if (!usage)
    return failure(usage.error().code ==
                           domain::OpsObservationErrorCode::resource_exhausted
                       ? Code::resource_exhausted
                       : Code::invalid_history);
  if (usage->evidence_bytes > maximum_manual_ops_catalog_bytes - state.bytes)
    return failure(Code::resource_exhausted);
  for (std::size_t earlier{}; earlier < slot; ++earlier) {
    const auto& retained = candidate.snapshots[earlier];
    if (retained &&
        (retained->observation_event_id == evidence.observation_event_id ||
         retained->observation_event_id == evidence.result_event_id ||
         retained->result_event_id == evidence.observation_event_id ||
         retained->result_event_id == evidence.result_event_id))
      return failure(Code::invalid_history);
  }
  state.bytes += usage->evidence_bytes;
  state.maximum_generation =
      std::max(state.maximum_generation, request.selection_generation);
  state.present = true;
  candidate.snapshots[slot] = evidence;
  candidate.freshness[slot] = AdminEvidenceFreshness::historical_unverified;
  return {};
}
auto retain_historical_catalog(const domain::OpsOwnerId& owner,
                               const domain::SessionId& session,
                               const ManualOpsInspection& inspection,
                               HistoricalCandidate& candidate)
    -> std::expected<void, Failure> {
  HistoricalCatalogState state;
  for (std::size_t slot{}; slot < inspection.projection.catalog.size();
       ++slot) {
    const auto& evidence = inspection.projection.catalog[slot];
    if (!evidence) continue;
    auto retained = retain_historical_evidence(owner, session, slot, *evidence,
                                               candidate, state);
    if (!retained) return retained;
  }
  if (inspection.projection.latest_success) {
    const auto slot = manual_ops_catalog_slot(
        inspection.projection.latest_success->observation.request.operation);
    if (slot >= candidate.snapshots.size())
      return failure(Code::invalid_history);
    const auto& retained = candidate.snapshots[slot];
    if (!retained.has_value() ||
        retained.value() != inspection.projection.latest_success.value())
      return failure(Code::invalid_history);
  } else if (state.present) {
    return failure(Code::invalid_history);
  }
  if (inspection.projection.maximum_selection_generation <
      state.maximum_generation)
    return failure(Code::invalid_history);
  return {};
}
auto retain_historical_selection(const ManualOpsInspection& inspection,
                                 HistoricalCandidate& candidate)
    -> std::expected<void, Failure> {
  const auto& projected = inspection.projection.historical_selection;
  if (inspection.source_connection ==
      ManualOpsSourceConnection::historical_unverified) {
    if (!inspection.selection || inspection.selection_generation == 0 ||
        !projected || projected->target != *inspection.selection ||
        projected->selection_generation != inspection.selection_generation ||
        inspection.projection.maximum_selection_generation !=
            inspection.selection_generation ||
        !valid_id(projected->run_id.value()) ||
        !valid_id(projected->event_id.value()) ||
        !domain::validate_ops_target_binding(projected->target))
      return failure(Code::invalid_history);
    candidate.target = inspection.selection;
    candidate.selection_generation = inspection.selection_generation;
    return {};
  }
  if (inspection.source_connection == ManualOpsSourceConnection::unbound) {
    if (inspection.selection || projected ||
        inspection.selection_generation !=
            inspection.projection.maximum_selection_generation)
      return failure(Code::invalid_history);
    candidate.selection_generation = inspection.selection_generation;
    return {};
  }
  if (!inspection.selection || inspection.selection_generation == 0 ||
      !projected || projected->target != *inspection.selection ||
      projected->selection_generation != inspection.selection_generation ||
      inspection.projection.maximum_selection_generation !=
          inspection.selection_generation)
    return failure(Code::invalid_history);
  candidate.selection_generation = inspection.selection_generation;
  return {};
}
auto historical_candidate(const domain::OpsOwnerId& owner,
                          const domain::SessionId& session,
                          const ManualOpsInspection& inspection)
    -> std::expected<HistoricalCandidate, Failure> {
  try {
    HistoricalCandidate candidate;
    auto catalog =
        retain_historical_catalog(owner, session, inspection, candidate);
    if (!catalog) return std::unexpected(catalog.error());
    auto selection = retain_historical_selection(inspection, candidate);
    if (!selection) return std::unexpected(selection.error());
    return candidate;
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
} // namespace

struct AdminController::Impl {
  struct Preparation {
    runtime::OpsSourcePreparationRequest request;
    std::optional<runtime::PreparedOpsSource> claimed;
    bool discarded{};
  };
  domain::OpsOwnerId owner;
  std::shared_ptr<AdminSourceCatalog> catalog;
  std::shared_ptr<runtime::LocalSourceWorker> worker;
  ManualOpsSession* manual{};
  AdminSelectionBinding* binding{};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint;
  std::optional<Preparation> preparation;
  std::optional<std::size_t> current_slot;
  AdminState state;

  Impl(domain::OpsOwnerId identity, std::shared_ptr<AdminSourceCatalog> sources,
       std::shared_ptr<runtime::LocalSourceWorker> shared_worker)
      : owner(std::move(identity)), catalog(std::move(sources)),
        worker(std::move(shared_worker)) {}

  auto clear_attachment() noexcept -> void {
    manual = nullptr;
    binding = nullptr;
    endpoint.reset();
    state.session.reset();
    state.active_target.reset();
    state.current.reset();
    state.current_status = domain::RunStatus::not_started;
    current_slot.reset();
    detached(state);
  }
  auto phase() -> void {
    if (state.fatal) {
      state.phase = AdminPhase::failed;
      return;
    }
    if (preparation)
      state.phase = preparation->discarded || preparation->claimed
                        ? AdminPhase::retiring
                        : AdminPhase::preparing;
    else if (manual == nullptr)
      state.phase = AdminPhase::detached;
    else if (state.current && !terminal(state.current_status))
      state.phase = manual->inspect_observations().approval
                        ? AdminPhase::awaiting_approval
                        : AdminPhase::observing;
    else if (state.problem)
      state.phase = AdminPhase::failed;
    else
      state.phase = state.active_target ? AdminPhase::ready : AdminPhase::idle;
  }
  auto report(Failure value) -> std::unexpected<Failure> {
    if (!state.fatal) state.problem = value;
    state.fatal = state.fatal || fatal(value);
    phase();
    return std::unexpected(state.problem.value_or(value));
  }
  auto source_failure(SourceError value) -> std::unexpected<Failure> {
    if (!state.fatal) state.source_problem = value;
    if (value == SourceError::resource_exhausted)
      return report({Code::resource_exhausted});
    if (value == SourceError::internal_failure)
      return report({Code::internal_failure});
    return report({Code::operation_failed});
  }
  auto eligible() -> std::expected<void, Failure> {
    if (state.fatal) return report(state.problem.value_or(Failure{}));
    if (manual == nullptr || !state.session || !endpoint || binding == nullptr)
      return report({Code::unavailable});
    const auto& inspection = manual->inspect_observations();
    if (inspection.problem && fatal(*inspection.problem))
      return report(*inspection.problem);
    if (inspection.closed) return report({Code::closed});
    if (inspection.busy) return report({Code::busy});
    // available is false for a legitimate first, as-yet-unbound session.
    return {};
  }
  auto cancel_preparation() -> std::expected<void, Failure> {
    if (!preparation) return {};
    preparation->discarded = true;
    state.pending_target.reset();
    const auto metadata = worker->preparation_state(preparation->request.token);
    if (!metadata) return report(worker_failure(metadata.error().code));
    if (!preparation->claimed && metadata->physically_outstanding) {
      auto cancelled = worker->cancel(preparation->request.token);
      if (!cancelled) return report(worker_failure(cancelled.error().code));
    }
    if (!metadata->physically_outstanding) preparation.reset();
    phase();
    return {};
  }
  auto cancel_current() -> std::expected<void, Failure> {
    if (manual == nullptr || !state.current || terminal(state.current_status))
      return {};
    const auto& current = manual->inspect_observations().projection.current;
    if (!current || current->submission != *state.current)
      return report({Code::wrong_operation});
    if (terminal(current->status)) {
      state.current_status = current->status;
      return {};
    }
    auto result = manual->cancel_observation(state.current->run_id);
    if (!result) return report(result.error());
    return synchronize();
  }
  auto cache(const ManualOpsInspection& inspection)
      -> std::expected<void, Failure> {
    const auto& success = inspection.projection.latest_success;
    const auto& progress = inspection.projection.current;
    if (!success || !state.current || success->submission != *state.current)
      return {};
    if (!progress || progress->status != domain::RunStatus::completed ||
        progress->observation_event_id != success->observation_event_id ||
        !state.session || !state.active_target)
      return report({Code::invalid_history});
    const auto& request = success->observation.request;
    const auto index = snapshot_index(request.operation);
    if (!index || request.owner_id != owner ||
        request.session_id != *state.session ||
        request.target != *state.active_target ||
        request.selection_generation != state.selection_generation ||
        !valid_id(success->observation_event_id.value()) ||
        !valid_id(success->result_event_id.value()) ||
        !domain::validate_recorded_ops_observation(success->observation))
      return report({Code::invalid_history});
    auto& retained = state.snapshots[*index];
    if (retained &&
        retained->observation.request.session_id == request.session_id &&
        retained->observation_event_id == success->observation_event_id) {
      state.freshness[*index] = AdminEvidenceFreshness::last_success;
      return {};
    }
    // Preserve last-good evidence if any member allocation in the copy fails.
    static_assert(
        std::is_nothrow_move_constructible_v<CommittedOpsObservation>);
    static_assert(std::is_nothrow_move_assignable_v<CommittedOpsObservation>);
    auto replacement = *success;
    retained = std::move(replacement);
    state.freshness[*index] = AdminEvidenceFreshness::last_success;
    return {};
  }
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- State reducer.
  auto synchronize() -> std::expected<void, Failure> {
    if (manual == nullptr) {
      phase();
      return {};
    }
    const auto& inspection = manual->inspect_observations();
    if (inspection.problem && fatal(*inspection.problem))
      return report(*inspection.problem);
    if (state.current) {
      const auto& current = inspection.projection.current;
      if (!current || current->submission != *state.current)
        return report({Code::invalid_history});
      state.current_status = current->status;
      if (auto cached = cache(inspection); !cached) return cached;
      if (current_slot && current->status == domain::RunStatus::failed) {
        state.freshness[*current_slot] = AdminEvidenceFreshness::refresh_failed;
        if (!state.problem) state.problem = Failure{Code::operation_failed};
      } else if (current_slot &&
                 current->status == domain::RunStatus::cancelled) {
        state.freshness[*current_slot] =
            state.snapshots[*current_slot]
                ? AdminEvidenceFreshness::last_success
                : AdminEvidenceFreshness::unavailable;
      }
    }
    if (state.active_target &&
        inspection.source_connection == ManualOpsSourceConnection::disconnected)
      disconnected(state);
    phase();
    return {};
  }
  auto select(const domain::OpsTargetId& target)
      -> std::expected<void, Failure> {
    if (!detail::valid_admin_target(target.value()))
      return report({Code::invalid_input});
    const auto found =
        std::ranges::find(state.targets, target, &AdminTargetChoice::id);
    if (found == state.targets.end()) return report({Code::invalid_input});
    if (auto allowed = eligible(); !allowed) return allowed;
    if (preparation) {
      if (auto cancelled = cancel_preparation(); !cancelled) return cancelled;
      if (preparation) return report({Code::busy});
    }
    if (state.selection_generation == std::numeric_limits<std::uint64_t>::max())
      return report({Code::resource_exhausted});
    auto sequence = worker->allocate_request_id();
    if (!sequence) return report(worker_failure(sequence.error().code));
    auto revision = domain::OpsConfigurationRevision::from(
        "admin-preparation-" + std::to_string(*sequence));
    if (!revision) return report({Code::internal_failure});
    if (!state.session) return report({Code::internal_failure});
    runtime::OpsSourcePreparationRequest request{
        {*state.session,
         state.session_epoch,
         *sequence,
         {target, *revision, found->kind}},
        std::chrono::steady_clock::now() + std::chrono::seconds{5}};
    auto factory = catalog->factory(target, *revision);
    if (!factory) return source_failure(factory.error());
    // Prepare state before admission so allocation failure cannot orphan work.
    Preparation pending{request, {}, false};
    auto pending_target = target;
    auto submitted = worker->submit(*factory, request);
    if (!submitted) return report(worker_failure(submitted.error().code));
    preparation = std::move(pending);
    state.pending_target = std::move(pending_target);
    state.problem.reset();
    state.source_problem.reset();
    phase();
    return {};
  }
  auto bind_prepared() -> std::expected<void, Failure> {
    if (auto allowed = eligible(); !allowed) return allowed;
    if (!preparation || !preparation->claimed || !state.session)
      return report({Code::internal_failure});
    const auto& source = preparation->claimed->source;
    auto valid = runtime::validate_ops_source_preparation_result(
        preparation->request, *preparation->claimed);
    if (!valid) return source_failure(valid.error());
    const auto kind = domain::ops_target_kind(source->target_binding());
    if (!kind || *kind == domain::OpsTargetKind::ceph)
      return source_failure(SourceError::unsupported);
    auto target = source->target_binding();
    const auto generation = state.selection_generation + 1;
    std::vector<domain::OpsObservationOperation> operations;
    if (*kind == domain::OpsTargetKind::linux_local) {
      operations = {domain::OpsObservationOperation::linux_health,
                    domain::OpsObservationOperation::linux_services,
                    domain::OpsObservationOperation::linux_service_health};
    } else {
      operations = {domain::OpsObservationOperation::kubernetes_workloads,
                    domain::OpsObservationOperation::kubernetes_pod_health,
                    domain::OpsObservationOperation::kubernetes_events};
    }
    auto authority =
        domain::OpsObservationAuthority::create({owner,
                                                 *state.session,
                                                 target,
                                                 generation,
                                                 std::move(operations),
                                                 {},
                                                 {}});
    if (!authority) return report({Code::invalid_input});
    auto bound = binding->bind(std::move(*authority), source, endpoint);
    if (!bound) return report(bound.error());
    state.active_target = std::move(target);
    state.selection_generation = generation;
    state.current.reset();
    state.current_status = domain::RunStatus::not_started;
    current_slot.reset();
    state.problem.reset();
    state.source_problem.reset();
    return {};
  }
  auto claim_prepared(const runtime::OpsSourcePreparationState& metadata)
      -> std::expected<void, Failure> {
    if (!metadata.ready) return {};
    // Busy does not claim the producer-owned resource or reset its deadline.
    const auto& inspection = manual->inspect_observations();
    if (inspection.closed) {
      auto cancelled = cancel_preparation();
      if (!cancelled) return cancelled;
      return report({Code::closed});
    }
    if (inspection.busy) return {};
    if (!preparation) return report({Code::internal_failure});
    auto& pending = *preparation;
    auto completion = worker->poll(pending.request.token);
    if (!completion) return report(worker_failure(completion.error().code));
    if (!*completion) return {};
    if (!(*completion)->result) {
      const auto value = (*completion)->result.error();
      pending.discarded = true;
      state.pending_target.reset();
      return source_failure(value);
    }
    pending.claimed = std::move(*(*completion)->result);
    phase();
    return {};
  }
  auto pump_preparation() -> std::expected<void, Failure> {
    if (!preparation) return {};
    const auto metadata = worker->preparation_state(preparation->request.token);
    if (!metadata) return report(worker_failure(metadata.error().code));
    if (preparation->discarded) {
      if (!metadata->physically_outstanding) preparation.reset();
      phase();
      return {};
    }
    if (!state.session ||
        preparation->request.token.session_id != *state.session ||
        preparation->request.token.session_epoch != state.session_epoch ||
        state.fatal) {
      return cancel_preparation();
    }
    if (std::chrono::steady_clock::now() >= preparation->request.deadline) {
      auto cancelled = cancel_preparation();
      if (!cancelled) return cancelled;
      return source_failure(SourceError::timed_out);
    }
    if (!preparation->claimed) return claim_prepared(*metadata);
    if (metadata->physically_outstanding) return {};
    if (manual->inspect_observations().busy) return {};
    auto result = bind_prepared();
    preparation.reset();
    state.pending_target.reset();
    phase();
    return result;
  }
  auto read(domain::OpsObservationOperation operation,
            domain::OpsResourceIdentity resource = {})
      -> std::expected<void, Failure> {
    if (auto allowed = eligible(); !allowed) return allowed;
    if (preparation || state.pending_target) return report({Code::busy});
    if (!state.active_target || !manual->inspect_observations().available)
      return report({Code::unavailable});
    const auto kind = domain::ops_target_kind(*state.active_target);
    if (!kind) return report({Code::invalid_input});
    const bool target_read =
        operation == domain::OpsObservationOperation::linux_health ||
        operation == domain::OpsObservationOperation::linux_services ||
        operation == domain::OpsObservationOperation::kubernetes_workloads ||
        (operation == domain::OpsObservationOperation::kubernetes_events &&
         std::holds_alternative<std::monostate>(resource));
    const bool valid_resource =
        target_read
            ? std::holds_alternative<std::monostate>(resource)
            : (operation ==
                   domain::OpsObservationOperation::linux_service_health ||
               operation ==
                   domain::OpsObservationOperation::kubernetes_pod_health ||
               operation ==
                   domain::OpsObservationOperation::kubernetes_events) &&
                  domain::validate_ops_resource_identity(*state.active_target,
                                                         resource)
                      .has_value();
    const auto slot = snapshot_index(operation);
    if (!operation_matches(*kind, operation) || !valid_resource || !slot)
      return report({Code::invalid_input});
    auto submitted = manual->submit_observation({state.active_target->target_id,
                                                 state.selection_generation,
                                                 operation,
                                                 std::move(resource),
                                                 {}});
    if (!submitted) return report(submitted.error());
    state.current = std::move(*submitted);
    current_slot = slot;
    state.freshness[*slot] = AdminEvidenceFreshness::refreshing;
    state.current_status = domain::RunStatus::running;
    state.problem.reset();
    state.source_problem.reset();
    return synchronize();
  }
  auto cached_service(const AdminReadCachedService& action)
      -> std::expected<void, Failure> {
    const auto& cached = state.snapshots[1];
    if (!state.session || action.session != *state.session || !cached ||
        cached->observation.request.session_id != action.session ||
        cached->observation_event_id != action.inventory_event ||
        action.selection_generation != state.selection_generation ||
        cached->observation.request.selection_generation !=
            state.selection_generation ||
        !state.active_target ||
        cached->observation.request.target != *state.active_target)
      return report({Code::wrong_operation});
    const auto* inventory = std::get_if<domain::LinuxServicesObservation>(
        &cached->observation.payload);
    if (inventory == nullptr || action.row >= inventory->services.size())
      return report({Code::invalid_input});
    return read(domain::OpsObservationOperation::linux_service_health,
                inventory->services[action.row].identity);
  }
  auto cached_pod(const domain::SessionId& session,
                  const domain::EventId& inventory_event,
                  std::uint64_t selection_generation, std::size_t row,
                  domain::OpsObservationOperation operation)
      -> std::expected<void, Failure> {
    const auto& cached = state.snapshots[3];
    if (!state.session || session != *state.session || !cached ||
        cached->observation.request.session_id != session ||
        cached->observation_event_id != inventory_event ||
        selection_generation != state.selection_generation ||
        cached->observation.request.selection_generation !=
            state.selection_generation ||
        !state.active_target ||
        cached->observation.request.target != *state.active_target)
      return report({Code::wrong_operation});
    const auto* inventory = std::get_if<domain::KubernetesWorkloadsObservation>(
        &cached->observation.payload);
    if (inventory == nullptr || row >= inventory->workloads.size())
      return report({Code::invalid_input});
    const auto& identity = inventory->workloads[row].identity;
    if (identity.kind != domain::OpsWorkloadKind::pod)
      return report({Code::invalid_input});
    return read(operation,
                domain::KubernetesPodIdentity{
                    identity.namespace_name, identity.name, identity.uid, {}});
  }
  auto named_pod(std::string name, domain::OpsResourceUid uid,
                 domain::OpsObservationOperation operation)
      -> std::expected<void, Failure> {
    if (!state.active_target) return report({Code::unavailable});
    const auto* identity = std::get_if<domain::KubernetesOpsIdentity>(
        &state.active_target->identity);
    if (identity == nullptr) return report({Code::invalid_input});
    return read(
        operation,
        domain::KubernetesPodIdentity{
            identity->namespace_name, std::move(name), std::move(uid), {}});
  }
  auto refresh_displayed(const AdminRefreshDisplayed& action)
      -> std::expected<void, Failure> {
    const auto slot = snapshot_index(action.operation);
    if (!slot) return report({Code::invalid_input});
    const auto& cached = state.snapshots[*slot];
    if (!state.session || action.session != *state.session || !cached ||
        !state.active_target || action.target != *state.active_target ||
        action.selection_generation != state.selection_generation ||
        cached->observation_event_id != action.observation_event) {
      return report({Code::wrong_operation});
    }
    const auto& request = cached->observation.request;
    if (request.session_id != action.session ||
        request.target != action.target ||
        request.selection_generation != action.selection_generation ||
        request.operation != action.operation ||
        request.resource != action.resource) {
      return report({Code::wrong_operation});
    }
    return read(action.operation, action.resource);
  }
};

AdminController::AdminController(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}
AdminController::~AdminController() {
  // Detach already latches any failure; destruction cannot return it.
  [[maybe_unused]] const auto detached = detach();
}
auto AdminController::create(domain::OpsOwnerId owner,
                             std::shared_ptr<AdminSourceCatalog> catalog,
                             std::shared_ptr<runtime::LocalSourceWorker> worker)
    -> std::expected<std::unique_ptr<AdminController>, ManualOpsFailure> {
  try {
    if (!valid_id(owner.value()) || !catalog || !worker ||
        !catalog->guarantees_owned_metadata())
      return failure(Code::invalid_input);
    const auto targets = catalog->targets();
    if (!validate_catalog(targets)) return failure(Code::invalid_input);
    auto impl = std::make_unique<Impl>(std::move(owner), std::move(catalog),
                                       std::move(worker));
    impl->state.targets.assign(targets.begin(), targets.end());
    return std::unique_ptr<AdminController>{
        new AdminController(std::move(impl))};
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto AdminController::attach(AdminSessionAttachment attachment)
    -> std::expected<void, ManualOpsFailure> {
  try {
    if (!valid_id(attachment.session.value()) || !attachment.endpoint)
      return failure(Code::invalid_input);
    if (m_impl->state.session_epoch ==
        std::numeric_limits<std::uint64_t>::max())
      return m_impl->report({Code::resource_exhausted});
    auto candidate =
        historical_candidate(m_impl->owner, attachment.session,
                             attachment.manual.inspect_observations());
    if (!candidate) return std::unexpected(candidate.error());
    auto detached = detach();
    if (!detached) return detached;
    m_impl->manual = &attachment.manual;
    m_impl->binding = &attachment.binding;
    m_impl->endpoint = std::move(attachment.endpoint);
    m_impl->state.session = std::move(attachment.session);
    ++m_impl->state.session_epoch;
    m_impl->state.snapshots = std::move(candidate->snapshots);
    m_impl->state.freshness = candidate->freshness;
    m_impl->state.historical_target = std::move(candidate->target);
    m_impl->state.selection_generation = candidate->selection_generation;
    m_impl->state.fatal = false;
    m_impl->state.problem.reset();
    m_impl->state.source_problem.reset();
    m_impl->phase();
    return {};
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
auto AdminController::detach() -> std::expected<void, ManualOpsFailure> {
  try {
    auto cancelled_preparation = m_impl->cancel_preparation();
    auto cancelled_run = m_impl->cancel_current();
    // Borrowed ports are cleared before any refusal is returned to the owner.
    m_impl->clear_attachment();
    m_impl->phase();
    if (!cancelled_preparation) return cancelled_preparation;
    return cancelled_run;
  } catch (...) {
    m_impl->clear_attachment();
    return m_impl->report({Code::internal_failure});
  }
}
auto AdminController::pump() -> std::expected<void, ManualOpsFailure> {
  try {
    std::optional<Failure> problem;
    if (m_impl->manual != nullptr) {
      auto result = m_impl->manual->pump_observations();
      if (!result) {
        problem = result.error();
        static_cast<void>(m_impl->report(*problem));
      }
      auto synced = m_impl->synchronize();
      if (!synced) problem = synced.error();
    }
    if (problem) {
      auto cancelled = m_impl->cancel_preparation();
      if (!cancelled) return cancelled;
      return m_impl->report(*problem);
    }
    auto prepared = m_impl->pump_preparation();
    if (!prepared) return prepared;
    m_impl->phase();
    return {};
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Typed dispatch.
auto AdminController::execute(const AdminAction& action)
    -> std::expected<void, ManualOpsFailure> {
  try {
    return std::visit(
        [&](const auto& value) -> std::expected<void, ManualOpsFailure> {
          using T = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<T, AdminInspect>) {
            m_impl->state.visible = true;
            return {};
          } else if constexpr (std::same_as<T, AdminSelectTarget>) {
            return m_impl->select(value.target);
          } else if constexpr (std::same_as<T, AdminReadHealth>) {
            return m_impl->read(domain::OpsObservationOperation::linux_health);
          } else if constexpr (std::same_as<T, AdminReadServices>) {
            return m_impl->read(
                domain::OpsObservationOperation::linux_services);
          } else if constexpr (std::same_as<T, AdminReadNamedService>) {
            if (!detail::valid_admin_service(value.unit))
              return m_impl->report({Code::invalid_input});
            return m_impl->read(
                domain::OpsObservationOperation::linux_service_health,
                domain::LinuxServiceIdentity{value.unit, {}});
          } else if constexpr (std::same_as<T, AdminReadCachedService>) {
            return m_impl->cached_service(value);
          } else if constexpr (std::same_as<T, AdminReadWorkloads>) {
            return m_impl->read(
                domain::OpsObservationOperation::kubernetes_workloads);
          } else if constexpr (std::same_as<T, AdminReadEvents>) {
            return m_impl->read(
                domain::OpsObservationOperation::kubernetes_events);
          } else if constexpr (std::same_as<T, AdminReadNamedPod>) {
            return m_impl->named_pod(
                value.name, value.uid,
                domain::OpsObservationOperation::kubernetes_pod_health);
          } else if constexpr (std::same_as<T, AdminReadNamedPodEvents>) {
            return m_impl->named_pod(
                value.name, value.uid,
                domain::OpsObservationOperation::kubernetes_events);
          } else if constexpr (std::same_as<T, AdminReadCachedPod>) {
            return m_impl->cached_pod(
                value.session, value.inventory_event,
                value.selection_generation, value.row,
                domain::OpsObservationOperation::kubernetes_pod_health);
          } else if constexpr (std::same_as<T, AdminReadCachedPodEvents>) {
            return m_impl->cached_pod(
                value.session, value.inventory_event,
                value.selection_generation, value.row,
                domain::OpsObservationOperation::kubernetes_events);
          } else if constexpr (std::same_as<T, AdminRefreshDisplayed>) {
            return m_impl->refresh_displayed(value);
          } else {
            if constexpr (std::same_as<T, AdminCloseView>)
              m_impl->state.visible = false;
            auto preparation = m_impl->cancel_preparation();
            auto current = m_impl->cancel_current();
            if (!preparation) return preparation;
            return current;
          }
        },
        action);
  } catch (...) {
    return m_impl->report({Code::internal_failure});
  }
}
auto AdminController::inspect() const noexcept -> const AdminState& {
  return m_impl->state;
}
} // namespace aiforge::surfaces
