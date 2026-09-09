#include <aiforge/domain/ops_target.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {
using Code = OpsTargetErrorCode;
using Status = std::expected<void, OpsTargetError>;
auto fail(Code code, std::string message) -> Status {
  return std::unexpected(OpsTargetError{code, std::move(message)});
}
auto text(std::string_view value, std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         detail::is_safe_utf8_text(value) &&
         std::ranges::none_of(
             value, [](unsigned char c) { return c < 32 || c == 127; });
}
auto identifier(const auto& value) -> bool {
  return text(value.value(), 128);
}
auto sequence(std::uint64_t value) -> bool {
  return value != 0 && value <= static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max());
}
auto lower_alnum(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
auto hex(char c) -> bool {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}
auto label(std::string_view value, std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         lower_alnum(value.front()) && lower_alnum(value.back()) &&
         std::ranges::all_of(value,
                             [](char c) { return lower_alnum(c) || c == '-'; });
}
auto dns(std::string_view value) -> bool {
  if (value.empty() || value.size() > 253) return false;
  while (true) {
    const auto dot = value.find('.');
    if (!label(value.substr(0, dot), 63)) return false;
    if (dot == std::string_view::npos) return true;
    value.remove_prefix(dot + 1);
  }
}
auto ipv4(std::string_view value) -> bool {
  unsigned count{};
  while (true) {
    const auto dot = value.find('.');
    const auto component = value.substr(0, dot);
    if (component.empty() || component.size() > 3 ||
        (component.size() > 1 && component.front() == '0'))
      return false;
    unsigned number{};
    for (char c : component) {
      if (c < '0' || c > '9') return false;
      number = (number * 10) + static_cast<unsigned>(c - '0');
    }
    if (number > 255 || ++count > 4) return false;
    if (dot == std::string_view::npos) return count == 4;
    value.remove_prefix(dot + 1);
  }
}
auto ipv6_groups(std::string_view value, bool allow_ipv4)
    -> std::optional<unsigned> {
  unsigned count{};
  if (value.empty()) return count;
  while (true) {
    const auto colon = value.find(':');
    const auto component = value.substr(0, colon);
    if (component.find('.') != std::string_view::npos) {
      if (!allow_ipv4 || colon != std::string_view::npos || !ipv4(component))
        return {};
      return count + 2;
    }
    if (component.empty() || component.size() > 4 ||
        !std::ranges::all_of(component, hex))
      return {};
    if (++count > 8) return {};
    if (colon == std::string_view::npos) return count;
    value.remove_prefix(colon + 1);
  }
}
auto host(std::string_view value) -> bool {
  if (value.empty() || value.size() > 253) return false;
  if (value.find(':') == std::string_view::npos) {
    const auto numeric = std::ranges::all_of(
        value, [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
    return numeric ? ipv4(value) : dns(value);
  }
  const auto compression = value.find("::");
  if (compression == std::string_view::npos)
    return ipv6_groups(value, true) == 8;
  if (value.find("::", compression + 2) != std::string_view::npos) return false;
  const auto before = ipv6_groups(value.substr(0, compression), false);
  const auto after = ipv6_groups(value.substr(compression + 2), true);
  return before && after && *before + *after < 8;
}
auto uuid(std::string_view value) -> bool {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    if (separator ? value[i] != '-' : !hex(value[i])) return false;
  }
  return true;
}
auto linux_identity(const LinuxOpsIdentity& value) -> bool {
  const bool scope = value.scope == LinuxExecutionScope::host ||
                     value.scope == LinuxExecutionScope::container ||
                     value.scope == LinuxExecutionScope::unknown;
  return scope && uuid(value.boot_id) && value.pid_namespace != 0 &&
         value.mount_namespace != 0;
}
auto target(const OpsTargetBinding& value) -> Status {
  if (!identifier(value.target_id) || !identifier(value.configuration_revision))
    return fail(Code::invalid_target, "Ops target identity is invalid");
  if (const auto* linux = std::get_if<LinuxOpsIdentity>(&value.identity)) {
    if (linux_identity(*linux)) return {};
  } else if (const auto* kube =
                 std::get_if<KubernetesOpsIdentity>(&value.identity)) {
    if (text(kube->context_name, 256) && label(kube->namespace_name, 63) &&
        host(kube->endpoint.host) && kube->endpoint.port != 0 &&
        text(kube->trust_identity, 256))
      return {};
  } else if (const auto* ceph = std::get_if<CephOpsIdentity>(&value.identity)) {
    if (text(ceph->cluster_identity, 128)) return {};
  }
  return fail(Code::invalid_target,
              "Ops target binding is malformed or unsupported");
}
auto known_operation(OpsObservationOperation value) -> bool {
  switch (value) {
    case OpsObservationOperation::linux_health:
    case OpsObservationOperation::linux_services:
    case OpsObservationOperation::linux_service_health:
    case OpsObservationOperation::linux_service_logs:
    case OpsObservationOperation::kubernetes_workloads:
    case OpsObservationOperation::kubernetes_pod_health:
    case OpsObservationOperation::kubernetes_events:
    case OpsObservationOperation::kubernetes_pod_logs: return true;
  }
  return false;
}
auto logs_operation(OpsObservationOperation value) -> bool {
  return value == OpsObservationOperation::linux_service_logs ||
         value == OpsObservationOperation::kubernetes_pod_logs;
}
auto matches_kind(OpsObservationOperation operation, OpsTargetKind kind)
    -> bool {
  if (!known_operation(operation) || kind == OpsTargetKind::ceph) return false;
  const bool local = operation <= OpsObservationOperation::linux_service_logs;
  return (kind == OpsTargetKind::linux_local) == local;
}
auto service_name(std::string_view value) -> bool {
  if (value.size() <= 8 || value.size() > 255 || !value.ends_with(".service") ||
      value.front() == '-')
    return false;
  return std::ranges::all_of(value, [](char c) {
    return lower_alnum(c) || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' ||
           c == '.' || c == '@' || c == ':';
  });
}
auto service_resource(const LinuxServiceIdentity& value, bool logs) -> bool {
  return service_name(value.unit_name) &&
         (!logs || value.invocation_id.has_value()) &&
         (!value.invocation_id || identifier(*value.invocation_id));
}
auto pod_resource(const KubernetesPodIdentity& value,
                  const KubernetesOpsIdentity& binding, bool logs) -> bool {
  if (value.namespace_name != binding.namespace_name || !dns(value.name) ||
      !identifier(value.uid))
    return false;
  if (!value.container) return !logs;
  return label(value.container->name, 63) &&
         text(value.container->runtime_identity, 512);
}
auto resource_matches(const OpsResourceIdentity& resource,
                      const OpsTargetBinding& binding, bool logs) -> bool {
  if (std::holds_alternative<LinuxOpsIdentity>(binding.identity)) {
    const auto* value = std::get_if<LinuxServiceIdentity>(&resource);
    return value != nullptr && service_resource(*value, logs);
  }
  const auto* kube = std::get_if<KubernetesOpsIdentity>(&binding.identity);
  const auto* value = std::get_if<KubernetesPodIdentity>(&resource);
  return kube != nullptr && value != nullptr &&
         pod_resource(*value, *kube, logs);
}
auto operation_resource(const OpsObservationRequest& request) -> bool {
  const auto empty = std::holds_alternative<std::monostate>(request.resource);
  switch (request.operation) {
    case OpsObservationOperation::linux_health:
    case OpsObservationOperation::linux_services:
    case OpsObservationOperation::kubernetes_workloads: return empty;
    case OpsObservationOperation::kubernetes_events:
      return empty || resource_matches(request.resource, request.target, false);
    default:
      return resource_matches(request.resource, request.target,
                              logs_operation(request.operation));
  }
}
auto limits_within(const OpsObservationLimits& value,
                   const OpsObservationLimits& ceiling) -> bool {
  return value.maximum_entries != 0 &&
         value.maximum_entries <= ceiling.maximum_entries &&
         value.maximum_bytes != 0 &&
         value.maximum_bytes <= ceiling.maximum_bytes &&
         value.timeout > std::chrono::milliseconds::zero() &&
         value.timeout <= ceiling.timeout && value.maximum_log_lines != 0 &&
         value.maximum_log_lines <= ceiling.maximum_log_lines &&
         value.maximum_log_bytes != 0 &&
         value.maximum_log_bytes <= ceiling.maximum_log_bytes &&
         value.maximum_log_age > std::chrono::seconds::zero() &&
         value.maximum_log_age <= ceiling.maximum_log_age;
}
auto log_policy(const OpsObservationAuthoritySpec& spec) -> Status {
  if (!sequence(spec.logs.revision) ||
      spec.logs.permitted_sources.size() > 32 ||
      (spec.logs.enabled && spec.logs.permitted_sources.empty()))
    return fail(Code::invalid_authority, "Ops log policy is invalid");
  for (auto current = spec.logs.permitted_sources.begin();
       current != spec.logs.permitted_sources.end(); ++current) {
    if (!resource_matches(*current, spec.target, true) ||
        std::find(spec.logs.permitted_sources.begin(), current, *current) !=
            current)
      return fail(Code::invalid_authority,
                  "Ops log sources must be distinct exact selected resources");
  }
  return {};
}
auto authority(const OpsObservationAuthoritySpec& spec) -> Status {
  if (auto valid = target(spec.target); !valid) return valid;
  const auto kind = ops_target_kind(spec.target);
  if (!kind || *kind == OpsTargetKind::ceph)
    return fail(Code::unavailable,
                "Ops target has no available observation implementation");
  if (!identifier(spec.owner_id) || !identifier(spec.session_id) ||
      !sequence(spec.selection_generation) || spec.operations.empty() ||
      spec.operations.size() > 8)
    return fail(Code::invalid_authority,
                "Ops owner session or operation grant is invalid");
  for (auto current = spec.operations.begin(); current != spec.operations.end();
       ++current) {
    if (!matches_kind(*current, *kind) ||
        std::find(spec.operations.begin(), current, *current) != current)
      return fail(
          Code::invalid_authority,
          "Ops operations must be distinct and match the selected target");
  }
  if (!limits_within(spec.limits, OpsObservationLimits{}))
    return fail(Code::resource_exhausted,
                "Ops observation ceilings are invalid");
  return log_policy(spec);
}
auto request_binding(const OpsObservationAuthoritySpec& spec,
                     const OpsObservationRequest& request) -> Status {
  if (!identifier(request.owner_id) || !identifier(request.session_id) ||
      !identifier(request.request_id))
    return fail(Code::invalid_request, "Ops request identity is invalid");
  if (request.owner_id != spec.owner_id)
    return fail(Code::foreign_owner, "Ops request belongs to another owner");
  if (request.session_id != spec.session_id)
    return fail(Code::foreign_session,
                "Ops request belongs to another session");
  if (auto valid = target(request.target); !valid) return valid;
  if (request.target != spec.target)
    return fail(Code::target_mismatch,
                "Ops request target no longer matches selection");
  if (!sequence(request.selection_generation) ||
      request.selection_generation != spec.selection_generation)
    return fail(Code::stale_selection,
                "Ops request selection generation is stale");
  if (!sequence(request.log_policy_revision) ||
      request.log_policy_revision != spec.logs.revision)
    return fail(Code::stale_log_policy,
                "Ops request log policy revision is stale");
  return {};
}
} // namespace

OpsObservationAuthority::OpsObservationAuthority(
    OpsObservationAuthoritySpec specification)
    : m_specification(std::move(specification)) {
}
auto OpsObservationAuthority::create(OpsObservationAuthoritySpec specification)
    -> std::expected<OpsObservationAuthority, OpsTargetError> {
  try {
    if (auto valid = authority(specification); !valid)
      return std::unexpected(valid.error());
    return OpsObservationAuthority{std::move(specification)};
  } catch (...) {
    return std::unexpected(OpsTargetError{Code::internal_failure,
                                          "Ops authority validation failed"});
  }
}
auto OpsObservationAuthority::validate(
    const OpsObservationRequest& request) const -> Status {
  try {
    if (auto valid = request_binding(m_specification, request); !valid)
      return valid;
    if (!known_operation(request.operation) ||
        !std::ranges::contains(m_specification.operations, request.operation))
      return fail(Code::operation_denied,
                  "Ops operation is outside the selected grant");
    if (!limits_within(request.limits, m_specification.limits))
      return fail(Code::resource_exhausted,
                  "Ops request exceeds observation ceilings");
    if (logs_operation(request.operation) && !m_specification.logs.enabled)
      return fail(Code::logs_disabled, "Ops logs are disabled");
    if (!operation_resource(request))
      return fail(Code::resource_mismatch,
                  "Ops operation requires an exact selected resource identity");
    if (logs_operation(request.operation) &&
        !std::ranges::contains(m_specification.logs.permitted_sources,
                               request.resource))
      return fail(Code::operation_denied,
                  "Ops log source is outside the selected consent");
    return {};
  } catch (...) {
    return fail(Code::internal_failure, "Ops request validation failed");
  }
}
auto ops_target_kind(const OpsTargetBinding& target) noexcept
    -> std::optional<OpsTargetKind> {
  switch (target.identity.index()) {
    case 0: return OpsTargetKind::linux_local;
    case 1: return OpsTargetKind::kubernetes;
    case 2: return OpsTargetKind::ceph;
    default: return {};
  }
}
auto validate_ops_target_binding(const OpsTargetBinding& value) -> Status {
  try {
    return target(value);
  } catch (...) {
    return fail(Code::internal_failure, "Ops target validation failed");
  }
}
auto validate_ops_target(const OpsTarget& value) -> Status {
  try {
    if (!text(value.display_name, 256))
      return fail(Code::invalid_text,
                  "Ops display alias must be bounded printable text");
    return target(value.binding);
  } catch (...) {
    return fail(Code::internal_failure, "Ops target validation failed");
  }
}
auto validate_ops_resource_identity(const OpsTargetBinding& binding,
                                    const OpsResourceIdentity& resource,
                                    bool require_log_identity) -> Status {
  try {
    if (auto valid = target(binding); !valid) return valid;
    if (!resource_matches(resource, binding, require_log_identity))
      return fail(Code::resource_mismatch,
                  "Ops resource identity does not match target");
    return {};
  } catch (...) {
    return fail(Code::internal_failure,
                "Ops resource identity validation failed");
  }
}
auto validate_ops_observation_limits(const OpsObservationLimits& limits)
    -> Status {
  try {
    if (!limits_within(limits, OpsObservationLimits{}))
      return fail(Code::resource_exhausted,
                  "Ops observation limits are invalid");
    return {};
  } catch (...) {
    return fail(Code::internal_failure,
                "Ops observation limit validation failed");
  }
}
auto validate_recorded_ops_request(const OpsObservationRequest& request)
    -> Status {
  try {
    if (!identifier(request.owner_id) || !identifier(request.session_id) ||
        !identifier(request.request_id) ||
        !sequence(request.selection_generation) ||
        !sequence(request.log_policy_revision))
      return fail(Code::invalid_request,
                  "Recorded Ops request identity is invalid");
    if (auto valid = target(request.target); !valid) return valid;
    const auto kind = ops_target_kind(request.target);
    if (!kind || !matches_kind(request.operation, *kind))
      return fail(Code::invalid_request,
                  "Recorded Ops operation does not match target");
    if (!limits_within(request.limits, OpsObservationLimits{}))
      return fail(Code::resource_exhausted,
                  "Recorded Ops request exceeds hard limits");
    if (!operation_resource(request))
      return fail(Code::resource_mismatch, "Recorded Ops resource is invalid");
    return {};
  } catch (...) {
    return fail(Code::internal_failure,
                "Recorded Ops request validation failed");
  }
}
} // namespace aiforge::domain
