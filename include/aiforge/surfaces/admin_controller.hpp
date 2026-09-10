#pragma once

#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/surfaces/manual_ops_session.hpp>
#include <array>
#include <span>
#include <variant>

namespace aiforge::surfaces {
struct AdminTargetChoice {
  domain::OpsTargetId id;
  std::string display_name;
  domain::OpsTargetKind kind;
};
// Owner-supplied immutable metadata/configuration graph. Neither method
// performs IO or initializes a live source/crypto runtime. Factories own their
// complete selected configuration and perform preparation only on the existing
// worker.
class AdminSourceCatalog {
 public:
  virtual ~AdminSourceCatalog() = default;
  [[nodiscard]] virtual auto guarantees_owned_metadata() const noexcept
      -> bool {
    return false;
  }
  [[nodiscard]] virtual auto targets() const noexcept
      -> std::span<const AdminTargetChoice> = 0;
  [[nodiscard]] virtual auto factory(domain::OpsTargetId target,
                                     domain::OpsConfigurationRevision revision)
      -> std::expected<std::shared_ptr<runtime::OpsSourcePreparationFactory>,
                       runtime::OpsObservationSourceError> = 0;
};
// Borrowed application binding port, separate from the widget action port.
// Production delegates only to ChatSession/OpsSession native bind. It cannot
// select a new policy, widen a model ceiling or replace an arbitrary registry.
class AdminSelectionBinding {
 public:
  virtual ~AdminSelectionBinding() = default;
  [[nodiscard]] virtual auto bind(
      domain::OpsObservationAuthority authority,
      std::shared_ptr<runtime::OpsObservationSource> source,
      std::shared_ptr<runtime::OpsObservationEndpoint> endpoint)
      -> std::expected<void, ManualOpsFailure> = 0;
};
struct AdminInspect {};
struct AdminSelectTarget {
  domain::OpsTargetId target;
};
struct AdminReadHealth {};
struct AdminReadServices {};
struct AdminReadNamedService {
  std::string unit;
};
struct AdminReadCachedService {
  domain::SessionId session;
  domain::EventId inventory_event;
  std::uint64_t selection_generation{};
  std::size_t row{};
};
struct AdminCancel {};
struct AdminCloseView {};
using AdminAction =
    std::variant<AdminInspect, AdminSelectTarget, AdminReadHealth,
                 AdminReadServices, AdminReadNamedService,
                 AdminReadCachedService, AdminCancel, AdminCloseView>;
enum class AdminPhase {
  detached,
  idle,
  preparing,
  retiring,
  ready,
  observing,
  awaiting_approval,
  failed
};
struct AdminState {
  std::vector<AdminTargetChoice> targets;
  std::optional<domain::SessionId> session;
  std::uint64_t session_epoch{};
  std::optional<domain::OpsTargetId> pending_target;
  std::optional<domain::OpsTargetBinding> active_target;
  std::uint64_t selection_generation{};
  std::optional<ObservationSubmission> current;
  domain::RunStatus current_status{domain::RunStatus::not_started};
  // Health, loaded services and exact service health. Historical values keep
  // their original session/target/event identity and never imply current grant.
  std::array<std::optional<CommittedOpsObservation>, 3> snapshots;
  AdminPhase phase{AdminPhase::detached};
  std::optional<ManualOpsFailure> problem;
  std::optional<runtime::OpsObservationSourceError> source_problem;
  bool visible{};
  bool fatal{};
};
class AdminControls {
 public:
  virtual ~AdminControls() = default;
  [[nodiscard]] virtual auto execute(const AdminAction& action)
      -> std::expected<void, ManualOpsFailure> = 0;
  [[nodiscard]] virtual auto inspect() const noexcept -> const AdminState& = 0;
};
struct AdminSessionAttachment {
  domain::SessionId session;
  ManualOpsSession& manual;
  AdminSelectionBinding& binding;
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint;
};
// Owner-thread API. Detach before destroying the attached manual/binding ports.
// Inspection is cached and its borrowed reference expires at the next mutation.
class AdminController final : public AdminControls {
 public:
  [[nodiscard]] static auto create(
      domain::OpsOwnerId owner, std::shared_ptr<AdminSourceCatalog> catalog,
      std::shared_ptr<runtime::LocalSourceWorker> worker)
      -> std::expected<std::unique_ptr<AdminController>, ManualOpsFailure>;
  ~AdminController() override;
  AdminController(const AdminController&) = delete;
  auto operator=(const AdminController&) -> AdminController& = delete;
  // Owner-only after successful durable open and broker activation. Neither
  // activation nor detach deactivates an application/replacement broker issuer.
  [[nodiscard]] auto attach(AdminSessionAttachment attachment)
      -> std::expected<void, ManualOpsFailure>;
  [[nodiscard]] auto detach() -> std::expected<void, ManualOpsFailure>;
  [[nodiscard]] auto pump() -> std::expected<void, ManualOpsFailure>;
  [[nodiscard]] auto execute(const AdminAction& action)
      -> std::expected<void, ManualOpsFailure> override;
  [[nodiscard]] auto inspect() const noexcept -> const AdminState& override;

 private:
  struct Impl;
  explicit AdminController(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
} // namespace aiforge::surfaces
