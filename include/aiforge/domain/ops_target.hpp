#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/domain/ids.hpp>

namespace aiforge::domain {

struct OpsTargetIdTag;
struct OpsOwnerIdTag;
struct OpsRequestIdTag;
struct OpsConfigurationRevisionTag;
struct OpsResourceUidTag;
using OpsTargetId = Id<OpsTargetIdTag>;
using OpsOwnerId = Id<OpsOwnerIdTag>;
using OpsRequestId = Id<OpsRequestIdTag>;
// Independently assigned revision, never a hash or encoding of credentials.
using OpsConfigurationRevision = Id<OpsConfigurationRevisionTag>;
using OpsResourceUid = Id<OpsResourceUidTag>;

enum class OpsTargetKind { linux_local, kubernetes, ceph };
enum class LinuxExecutionScope { host, container, unknown };
struct LinuxOpsIdentity {
  LinuxExecutionScope scope{LinuxExecutionScope::unknown};
  std::string boot_id;
  std::uint64_t pid_namespace{};
  std::uint64_t mount_namespace{};
  auto operator==(const LinuxOpsIdentity&) const -> bool = default;
};

// HTTPS is the only scheme. Host is a literal DNS name or IP address, without
// credentials, URL syntax, path, query or fragment; port is explicit and
// nonzero.
struct OpsHttpsEndpoint {
  std::string host;
  std::uint16_t port{443};
  auto operator==(const OpsHttpsEndpoint&) const -> bool = default;
};
struct KubernetesOpsIdentity {
  std::string context_name;
  std::string namespace_name;
  OpsHttpsEndpoint endpoint;
  // Non-secret CA/SPKI digests are valid trust identities. Private-key/token
  // material and digests of secrets are forbidden.
  std::string trust_identity;
  auto operator==(const KubernetesOpsIdentity&) const -> bool = default;
};
struct CephOpsIdentity {
  std::string cluster_identity;
  auto operator==(const CephOpsIdentity&) const -> bool = default;
};
using OpsTargetIdentity =
    std::variant<LinuxOpsIdentity, KubernetesOpsIdentity, CephOpsIdentity>;
struct OpsTargetBinding {
  OpsTargetId target_id;
  OpsConfigurationRevision configuration_revision;
  OpsTargetIdentity identity;
  auto operator==(const OpsTargetBinding&) const -> bool = default;
};
struct OpsTarget {
  OpsTargetBinding binding;
  // Presentation only: aliases never resolve authority or appear in requests.
  std::string display_name;
  auto operator==(const OpsTarget&) const -> bool = default;
};

// Node/cluster-wide health is deliberately separate future scope. No arbitrary
// resource, API path, shell, argv, mutation or transport option is
// representable.
enum class OpsObservationOperation {
  linux_health,
  linux_services,
  linux_service_health,
  linux_service_logs,
  kubernetes_workloads,
  kubernetes_pod_health,
  kubernetes_events,
  kubernetes_pod_logs,
};
struct LinuxServiceIdentity {
  std::string unit_name;
  // A stopped/inactive unit may have no observed invocation. Logs require one.
  std::optional<OpsResourceUid> invocation_id{};
  auto operator==(const LinuxServiceIdentity&) const -> bool = default;
};
struct KubernetesContainerIdentity {
  std::string name;
  std::string runtime_identity;
  auto operator==(const KubernetesContainerIdentity&) const -> bool = default;
};
struct KubernetesPodIdentity {
  std::string namespace_name;
  std::string name;
  OpsResourceUid uid;
  std::optional<KubernetesContainerIdentity> container{};
  auto operator==(const KubernetesPodIdentity&) const -> bool = default;
};
using OpsResourceIdentity =
    std::variant<std::monostate, LinuxServiceIdentity, KubernetesPodIdentity>;

// These defaults are hard implementation bounds for the initial contract.
// Authority and per-request limits may only narrow them.
struct OpsObservationLimits {
  std::size_t maximum_entries{256};
  std::uint64_t maximum_bytes{std::uint64_t{1024} * 1024};
  std::chrono::milliseconds timeout{5000};
  std::size_t maximum_log_lines{200};
  std::uint64_t maximum_log_bytes{std::uint64_t{32} * 1024};
  std::chrono::seconds maximum_log_age{600};
  auto operator==(const OpsObservationLimits&) const -> bool = default;
};
struct OpsLogPolicy {
  std::uint64_t revision{1};
  bool enabled{};
  // Exact selected services/pod containers only; never an arbitrary application
  // selector. At most 32 distinct sources, all within the selected target.
  std::vector<OpsResourceIdentity> permitted_sources{};
  auto operator==(const OpsLogPolicy&) const -> bool = default;
};
struct OpsObservationAuthoritySpec {
  // Process-local application owner, distinct from the observed application.
  OpsOwnerId owner_id;
  SessionId session_id;
  OpsTargetBinding target;
  std::uint64_t selection_generation{};
  std::vector<OpsObservationOperation> operations;
  OpsLogPolicy logs{};
  OpsObservationLimits limits{};
  auto operator==(const OpsObservationAuthoritySpec&) const -> bool = default;
};
struct OpsObservationRequest {
  OpsOwnerId owner_id;
  SessionId session_id;
  OpsRequestId request_id;
  OpsTargetBinding target;
  std::uint64_t selection_generation{};
  OpsObservationOperation operation{OpsObservationOperation::linux_health};
  OpsResourceIdentity resource{};
  // Must match even when logging is disabled. A request cannot opt logs in.
  std::uint64_t log_policy_revision{};
  OpsObservationLimits limits{};
  auto operator==(const OpsObservationRequest&) const -> bool = default;
};

enum class OpsTargetErrorCode {
  invalid_target,
  invalid_authority,
  invalid_request,
  invalid_text,
  unavailable,
  foreign_owner,
  foreign_session,
  target_mismatch,
  stale_selection,
  stale_log_policy,
  operation_denied,
  logs_disabled,
  resource_mismatch,
  resource_exhausted,
  internal_failure,
};
struct OpsTargetError {
  OpsTargetErrorCode code;
  // Fixed safe application diagnostic; never echo input or adapter failures.
  std::string message;
  auto operator==(const OpsTargetError&) const -> bool = default;
};

// Construction validates an application-supplied grant; it does not discover a
// target, authorize the application, or manufacture authority from model text.
// No mutators: requests are checked against this exact caller-owned snapshot.
class OpsObservationAuthority final {
 public:
  [[nodiscard]] static auto create(OpsObservationAuthoritySpec specification)
      -> std::expected<OpsObservationAuthority, OpsTargetError>;
  [[nodiscard]] auto specification() const noexcept
      -> const OpsObservationAuthoritySpec& {
    return m_specification;
  }
  [[nodiscard]] auto validate(const OpsObservationRequest& request) const
      -> std::expected<void, OpsTargetError>;

 private:
  explicit OpsObservationAuthority(OpsObservationAuthoritySpec specification);
  OpsObservationAuthoritySpec m_specification;
};

[[nodiscard]] auto ops_target_kind(const OpsTargetBinding& target) noexcept
    -> std::optional<OpsTargetKind>;
[[nodiscard]] auto validate_ops_target_binding(const OpsTargetBinding& value)
    -> std::expected<void, OpsTargetError>;
// Pure identity shape validation, independent of owner-assigned binding IDs.
[[nodiscard]] auto validate_kubernetes_ops_identity(
    const KubernetesOpsIdentity& value) -> std::expected<void, OpsTargetError>;
[[nodiscard]] auto validate_ops_target(const OpsTarget& value)
    -> std::expected<void, OpsTargetError>;
// Pure resource shape/binding validation; does not grant authority or perform
// IO.
[[nodiscard]] auto validate_ops_resource_identity(
    const OpsTargetBinding& binding, const OpsResourceIdentity& resource,
    bool require_log_identity = false) -> std::expected<void, OpsTargetError>;
[[nodiscard]] auto validate_ops_observation_limits(
    const OpsObservationLimits& limits) -> std::expected<void, OpsTargetError>;
// Structural historical data only. Does not establish current selection,
// consent, authority, or permission to collect/publish a new observation.
[[nodiscard]] auto validate_recorded_ops_request(
    const OpsObservationRequest& request)
    -> std::expected<void, OpsTargetError>;

} // namespace aiforge::domain
