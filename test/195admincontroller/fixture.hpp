#pragma once
#include "../159foldergrant/fixture.hpp"
#include <aiforge/surfaces/admin_controller.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

namespace admin_controller_test {
using namespace aiforge;
using namespace aiforge::domain;
using namespace aiforge::surfaces;
using namespace std::chrono_literals;
using folder_grant_test::Gate;
using folder_grant_test::Release;
using folder_grant_test::until;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
struct PreparationState {
  std::mutex mutex;
  std::optional<runtime::OpsSourcePreparationRequest> input;
  std::shared_ptr<Gate> gate;
  std::atomic<unsigned> prepared{}, observed{}, destroyed{};
  std::optional<runtime::OpsObservationSourceError> failure;
  auto request() -> runtime::OpsSourcePreparationRequest {
    const std::lock_guard lock{mutex};
    REQUIRE(input);
    return *input;
  }
};
class Source final : public runtime::OpsObservationSource {
 public:
  Source(runtime::OpsSourcePreparationIdentity identity,
         std::shared_ptr<PreparationState> state)
      : m_binding{std::move(identity.target_id),
                  std::move(identity.configuration_revision),
                  LinuxOpsIdentity{LinuxExecutionScope::container,
                                   "12345678-1234-1234-1234-123456789abc", 42,
                                   43}},
        m_state(std::move(state)) {}
  ~Source() override { ++m_state->destroyed; }
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept -> const OpsTargetBinding& override {
    return m_binding;
  }
  auto observe(const OpsObservationRequest&, std::stop_token)
      -> std::expected<OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++m_state->observed;
    return std::unexpected(runtime::OpsObservationSourceError::unsupported);
  }

 private:
  OpsTargetBinding m_binding;
  std::shared_ptr<PreparationState> m_state;
};
class Factory final : public runtime::OpsSourcePreparationFactory {
 public:
  Factory(runtime::OpsSourcePreparationIdentity identity,
          std::shared_ptr<PreparationState> state)
      : m_identity(std::move(identity)), m_state(std::move(state)) {}
  auto guarantees_owned_read_only_preparation() const noexcept
      -> bool override {
    return true;
  }
  auto preparation_identity() const noexcept
      -> const runtime::OpsSourcePreparationIdentity& override {
    return m_identity;
  }
  auto prepare(const runtime::OpsSourcePreparationRequest& request,
               std::stop_token)
      -> std::expected<runtime::PreparedOpsSource,
                       runtime::OpsObservationSourceError> override {
    {
      const std::lock_guard lock{m_state->mutex};
      m_state->input = request;
    }
    ++m_state->prepared;
    if (m_state->gate) m_state->gate->wait();
    if (m_state->failure) return std::unexpected(*m_state->failure);
    return runtime::PreparedOpsSource{
        std::make_shared<Source>(m_identity, m_state)};
  }

 private:
  runtime::OpsSourcePreparationIdentity m_identity;
  std::shared_ptr<PreparationState> m_state;
};
class Catalog final : public AdminSourceCatalog {
 public:
  std::vector<AdminTargetChoice> choices{
      {id<OpsTargetId>("alpha"), "Alpha", OpsTargetKind::linux_local},
      {id<OpsTargetId>("beta"), "Beta", OpsTargetKind::linux_local}};
  std::shared_ptr<PreparationState> state{std::make_shared<PreparationState>()};
  bool owned{true};
  unsigned calls{};
  std::optional<runtime::OpsObservationSourceError> failure;
  auto guarantees_owned_metadata() const noexcept -> bool override {
    return owned;
  }
  auto targets() const noexcept -> std::span<const AdminTargetChoice> override {
    return choices;
  }
  auto factory(OpsTargetId target, OpsConfigurationRevision revision)
      -> std::expected<std::shared_ptr<runtime::OpsSourcePreparationFactory>,
                       runtime::OpsObservationSourceError> override {
    ++calls;
    if (failure) return std::unexpected(*failure);
    return std::make_shared<Factory>(
        runtime::OpsSourcePreparationIdentity{
            std::move(target), std::move(revision), OpsTargetKind::linux_local},
        state);
  }
};
class Manual final : public ManualOpsSession {
 public:
  ManualOpsInspection inspection;
  std::optional<OpsObservationAuthoritySpec> authority;
  std::vector<runtime::OpsObservationIntent> intents;
  unsigned pumps{}, cancels{};
  bool approval{}, hold{}, fail_completion{}, malformed{}, throws_cancel{};
  std::optional<ManualOpsFailure> pump_failure, cancel_failure, submit_failure;
  auto submit_observation(runtime::OpsObservationIntent intent)
      -> std::expected<ObservationSubmission, ManualOpsFailure> override {
    if (submit_failure) return std::unexpected(*submit_failure);
    intents.push_back(intent);
    ObservationSubmission submitted{
        id<RunId>("run-" + std::to_string(intents.size())),
        id<InvocationId>("invocation-" + std::to_string(intents.size()))};
    inspection.projection.current = ManualObservationProgress{
        submitted,
        approval ? RunStatus::awaiting_approval : RunStatus::running,
        {},
        {}};
    inspection.busy = true;
    if (approval)
      inspection.approval = runtime::PendingToolApproval{
          submitted.run_id,
          submitted.invocation_id,
          "observe_target",
          {},
          {},
          {},
          {},
          {},
          ToolApprovalMode::prompt,
          runtime::ToolApprovalSupplySource::per_invocation,
          {}};
    return submitted;
  }
  auto cancel_observation(const RunId& run)
      -> std::expected<void, ManualOpsFailure> override {
    ++cancels;
    if (throws_cancel) throw std::runtime_error("fixture cancel failure");
    if (cancel_failure) return std::unexpected(*cancel_failure);
    if (!inspection.projection.current ||
        inspection.projection.current->submission.run_id != run)
      return std::unexpected(
          ManualOpsFailure{ManualOpsErrorCode::wrong_operation});
    inspection.projection.current->status = RunStatus::cancelled;
    inspection.busy = false;
    inspection.approval.reset();
    return {};
  }
  auto decide_observation_approval(const RunId&, const InvocationId&,
                                   runtime::ToolApprovalResolution)
      -> std::expected<void, ManualOpsFailure> override {
    return std::unexpected(
        ManualOpsFailure{ManualOpsErrorCode::wrong_operation});
  }
  auto pump_observations() -> std::expected<void, ManualOpsFailure> override {
    ++pumps;
    if (pump_failure) return std::unexpected(*pump_failure);
    if (hold || !inspection.projection.current || !inspection.busy ||
        inspection.approval)
      return {};
    auto& current = *inspection.projection.current;
    inspection.busy = false;
    if (fail_completion) {
      current.status = RunStatus::failed;
      return {};
    }
    current.status = RunStatus::completed;
    const auto& intent = intents.back();
    const auto& grant = *authority;
    OpsObservationRequest request{
        grant.owner_id,
        grant.session_id,
        id<OpsRequestId>("request-" + std::to_string(intents.size())),
        grant.target,
        intent.selection_generation,
        intent.operation,
        intent.resource,
        grant.logs.revision,
        intent.limits};
    if (malformed) ++request.selection_generation;
    OpsObservationPayload payload =
        LinuxHealthObservation{OpsHealthState::healthy, 12, {}, {}, {}};
    if (intent.operation == OpsObservationOperation::linux_services)
      payload = LinuxServicesObservation{{LinuxServiceObservation{
          {"fixture.service", id<OpsResourceUid>("invocation")},
          OpsServiceState::active,
          OpsObservationReason::none,
          {},
          {}}}};
    if (intent.operation == OpsObservationOperation::linux_service_health)
      payload = LinuxServiceObservation{
          std::get<LinuxServiceIdentity>(intent.resource),
          OpsServiceState::active,
          OpsObservationReason::none,
          {},
          {}};
    const auto event =
        id<EventId>("observation-" + std::to_string(intents.size()));
    current.observation_event_id = event;
    inspection.projection.latest_success = CommittedOpsObservation{
        current.submission,
        event,
        id<EventId>("result-" + std::to_string(intents.size())),
        {std::move(request),
         EventTimestamp{std::chrono::milliseconds{1000}},
         EventTimestamp{std::chrono::milliseconds{1001}},
         OpsObservationCompleteness::complete,
         0,
         0,
         {},
         std::move(payload)}};
    return {};
  }
  auto inspect_observations() const noexcept
      -> const ManualOpsInspection& override {
    return inspection;
  }
};
class Binding final : public AdminSelectionBinding {
 public:
  std::shared_ptr<runtime::OpsObservationBroker> broker;
  Manual& manual;
  unsigned calls{};
  std::optional<ManualOpsFailure> failure;
  explicit Binding(std::shared_ptr<runtime::OpsObservationBroker> value,
                   Manual& port)
      : broker(std::move(value)), manual(port) {}
  auto bind(OpsObservationAuthority authority,
            std::shared_ptr<runtime::OpsObservationSource> source,
            std::shared_ptr<runtime::OpsObservationEndpoint> endpoint)
      -> std::expected<void, ManualOpsFailure> override {
    ++calls;
    if (failure) return std::unexpected(*failure);
    const auto& grant = authority.specification();
    if (!broker->preflight_selection(*endpoint, authority, source))
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::unavailable});
    auto selected = broker->select(authority, source);
    if (!selected)
      return std::unexpected(
          ManualOpsFailure{ManualOpsErrorCode::operation_failed});
    manual.authority = grant;
    manual.inspection.selection = grant.target;
    manual.inspection.available = true;
    return {};
  }
};
struct DetachOnExit {
  AdminController& controller;
  ~DetachOnExit() { static_cast<void>(controller.detach()); }
};
struct Fixture {
  SessionId session{id<SessionId>("session")};
  std::shared_ptr<Catalog> catalog{std::make_shared<Catalog>()};
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(2).value()};
  std::shared_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint{
      broker->activate_session(session).value()};
  Manual manual;
  Binding binding{broker, manual};
  std::unique_ptr<AdminController> controller;
  ~Fixture() {
    controller.reset();
    static_cast<void>(broker->close());
  }
  auto open() -> void {
    auto created =
        AdminController::create(id<OpsOwnerId>("owner"), catalog, worker);
    REQUIRE(created);
    controller = std::move(*created);
    REQUIRE(controller->attach({session, manual, binding, endpoint}));
  }
  auto select(std::string name = "alpha") -> void {
    REQUIRE(controller->execute(
        AdminSelectTarget{id<OpsTargetId>(std::move(name))}));
  }
  auto ready() -> void {
    REQUIRE(until([&] {
      auto pumped = controller->pump();
      REQUIRE(pumped);
      return controller->inspect().phase == AdminPhase::ready;
    }));
  }
  auto capture(AdminAction action) -> void {
    REQUIRE(controller->execute(action));
    REQUIRE(controller->pump());
    REQUIRE(controller->inspect().current_status == RunStatus::completed);
  }
};
} // namespace admin_controller_test
