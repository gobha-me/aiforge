// Failure matrix written before implementation:
// Reject invalid or oversized UTF-8 identities, aliases, names and endpoints.
// Require concrete Linux boot/namespace identity for host/container/unknown.
// Represent Ceph without making an observation operation available.
// Bind exact owner, session, target revision, generation and namespace.
// Reject unknown, duplicate or wrong-kind operation grants and bad ceilings.
// Logs require enabled consent at the exact revision for the selected source.
// Requests cannot enable logs or expand consent and observation ceilings.
// Pod health requires UID; pod logs also require exact container identity.
// Service health permits absent invocation; logs require observed invocation.
// List operations cannot carry hidden resource selections.
// Reject zero, overflow and over-limit counts, bytes and time bounds.
// Aliases never change authority; re-selection invalidates old generations.
// No transport, secrets, filesystem reads, commands or durable writes.

#include <aiforge/domain/ops_target.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace aiforge::domain;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto linux_target() -> OpsTargetBinding {
  return {id<OpsTargetId>("local"), id<OpsConfigurationRevision>("revision-1"),
          LinuxOpsIdentity{LinuxExecutionScope::container,
                           "12345678-1234-1234-1234-123456789abc", 42, 43}};
}
auto kube_target() -> OpsTargetBinding {
  return {id<OpsTargetId>("cluster"),
          id<OpsConfigurationRevision>("revision-1"),
          KubernetesOpsIdentity{"explicit-context",
                                "chosen",
                                {"api.example.test", 6443},
                                "ca-public-digest"}};
}
auto service() -> OpsResourceIdentity {
  return LinuxServiceIdentity{"example.service",
                              id<OpsResourceUid>("observed-invocation")};
}
auto pod() -> OpsResourceIdentity {
  return KubernetesPodIdentity{
      "chosen", "example-pod", id<OpsResourceUid>("pod-uid"),
      KubernetesContainerIdentity{"main", "containerd://observed-runtime-id"}};
}
auto spec(bool kube = false) -> OpsObservationAuthoritySpec {
  return {id<OpsOwnerId>("owner"),
          id<SessionId>("session"),
          kube ? kube_target() : linux_target(),
          1,
          kube ? std::vector{OpsObservationOperation::kubernetes_workloads,
                             OpsObservationOperation::kubernetes_pod_health,
                             OpsObservationOperation::kubernetes_events,
                             OpsObservationOperation::kubernetes_pod_logs}
               : std::vector{OpsObservationOperation::linux_health,
                             OpsObservationOperation::linux_services,
                             OpsObservationOperation::linux_service_health,
                             OpsObservationOperation::linux_service_logs},
          {1, false, {}},
          {}};
}
auto request(const OpsObservationAuthoritySpec& grant)
    -> OpsObservationRequest {
  return {grant.owner_id,
          grant.session_id,
          id<OpsRequestId>("request"),
          grant.target,
          grant.selection_generation,
          grant.operations.front(),
          {},
          grant.logs.revision,
          grant.limits};
}
auto permit_logs(OpsObservationAuthoritySpec& grant, bool kube = false)
    -> void {
  grant.logs.enabled = true;
  grant.logs.permitted_sources = {kube ? pod() : service()};
}
} // namespace

TEST_CASE("Ops target identities reject malformed bounded text and endpoint "
          "ambiguity") {
  for (unsigned bad = 0; bad < 12; ++bad) {
    auto target = kube_target();
    auto& identity = std::get<KubernetesOpsIdentity>(target.identity);
    switch (bad) {
      case 0: identity.context_name = ""; break;
      case 1: identity.context_name = std::string(257, 'a'); break;
      case 2: identity.context_name = "bad\ncontext"; break;
      case 3:
        identity.context_name = std::string(1, static_cast<char>(0xff));
        break;
      case 4: identity.namespace_name = "Other"; break;
      case 5: identity.namespace_name = "../other"; break;
      case 6: identity.endpoint.host = "user@api.example.test"; break;
      case 7: identity.endpoint.host = "api.example.test/path"; break;
      case 8: identity.endpoint.host = "api.example.test?token=secret"; break;
      case 9: identity.endpoint.host = "01.2.3.4"; break;
      case 10: identity.endpoint.port = 0; break;
      case 11: identity.trust_identity = std::string(257, 'x'); break;
    }
    INFO(bad);
    auto result = validate_ops_target_binding(target);
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("secret") == std::string::npos);
  }
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto target = linux_target();
    auto& identity = std::get<LinuxOpsIdentity>(target.identity);
    if (bad == 0) identity.scope = static_cast<LinuxExecutionScope>(99);
    if (bad == 1) identity.boot_id = "not-a-boot-uuid";
    if (bad == 2) identity.pid_namespace = 0;
    if (bad == 3) identity.mount_namespace = 0;
    CHECK_FALSE(validate_ops_target_binding(target));
  }
  CHECK_FALSE(validate_ops_target({linux_target(), "alias\033control"}));
  CHECK_FALSE(validate_ops_target({linux_target(), std::string(257, 'a')}));
}
TEST_CASE("Ops grants fail closed on unsupported targets operations and "
          "invalid limits") {
  for (unsigned bad = 0; bad < 8; ++bad) {
    auto grant = spec();
    switch (bad) {
      case 0: grant.selection_generation = 0; break;
      case 1: grant.operations.clear(); break;
      case 2: grant.operations.push_back(grant.operations.front()); break;
      case 3:
        grant.operations = {OpsObservationOperation::kubernetes_events};
        break;
      case 4:
        grant.operations = {static_cast<OpsObservationOperation>(99)};
        break;
      case 5: grant.logs.revision = 0; break;
      case 6:
        grant.limits.maximum_bytes = std::numeric_limits<std::uint64_t>::max();
        break;
      case 7: grant.limits.timeout = std::chrono::milliseconds::max(); break;
    }
    CHECK_FALSE(OpsObservationAuthority::create(std::move(grant)));
  }
  auto ceph = spec();
  ceph.target.identity = CephOpsIdentity{"future-cluster"};
  REQUIRE(validate_ops_target_binding(ceph.target));
  auto unavailable = OpsObservationAuthority::create(ceph);
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().code == OpsTargetErrorCode::unavailable);
  for (unsigned field = 0; field < 6; ++field) {
    auto limits = OpsObservationLimits{};
    switch (field) {
      case 0: limits.maximum_entries = 0; break;
      case 1: limits.maximum_bytes = 0; break;
      case 2: limits.timeout = std::chrono::milliseconds{-1}; break;
      case 3: limits.maximum_log_lines = 0; break;
      case 4: limits.maximum_log_bytes = 0; break;
      case 5: limits.maximum_log_age = std::chrono::seconds{-1}; break;
    }
    CHECK_FALSE(validate_ops_observation_limits(limits));
  }
}
TEST_CASE(
    "Ops requests bind exact owner session target generation and policy") {
  auto grant = spec();
  auto authority = OpsObservationAuthority::create(grant);
  REQUIRE(authority);
  for (unsigned bad = 0; bad < 10; ++bad) {
    auto input = request(grant);
    switch (bad) {
      case 0: input.owner_id = id<OpsOwnerId>("other-owner"); break;
      case 1: input.session_id = id<SessionId>("other-session"); break;
      case 2: input.target.target_id = id<OpsTargetId>("other"); break;
      case 3:
        input.target.configuration_revision =
            id<OpsConfigurationRevision>("revision-2");
        break;
      case 4:
        std::get<LinuxOpsIdentity>(input.target.identity).mount_namespace++;
        break;
      case 5: input.selection_generation++; break;
      case 6: input.log_policy_revision++; break;
      case 7:
        input.operation = OpsObservationOperation::kubernetes_events;
        break;
      case 8: input.resource = service(); break;
      case 9:
        input.selection_generation = std::numeric_limits<std::uint64_t>::max();
        break;
    }
    INFO(bad);
    CHECK_FALSE(authority->validate(input));
  }
  auto limited = grant;
  limited.limits.maximum_log_bytes = 16;
  limited.limits.maximum_entries = 1;
  auto smaller = OpsObservationAuthority::create(limited);
  REQUIRE(smaller);
  CHECK_FALSE(smaller->validate(request(grant)));
}
TEST_CASE(
    "Ops log consent grants only exact selected services and pod containers") {
  for (bool kube : {false, true}) {
    auto grant = spec(kube);
    auto input = request(grant);
    input.operation = kube ? OpsObservationOperation::kubernetes_pod_logs
                           : OpsObservationOperation::linux_service_logs;
    input.resource = kube ? pod() : service();
    auto disabled = OpsObservationAuthority::create(grant);
    REQUIRE(disabled);
    auto denied = disabled->validate(input);
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == OpsTargetErrorCode::logs_disabled);
    permit_logs(grant, kube);
    auto allowed = OpsObservationAuthority::create(grant);
    REQUIRE(allowed);
    REQUIRE(allowed->validate(input));
    if (kube)
      std::get<KubernetesPodIdentity>(input.resource).uid =
          id<OpsResourceUid>("other-pod");
    else
      std::get<LinuxServiceIdentity>(input.resource).unit_name =
          "other.service";
    CHECK_FALSE(allowed->validate(input));
    input.resource = kube ? pod() : service();
    if (kube)
      std::get<KubernetesPodIdentity>(input.resource)
          .container->runtime_identity = "containerd://restarted";
    else
      std::get<LinuxServiceIdentity>(input.resource).invocation_id =
          id<OpsResourceUid>("restarted");
    CHECK_FALSE(allowed->validate(input));
    auto invalid = grant;
    invalid.logs.permitted_sources.push_back(
        invalid.logs.permitted_sources.front());
    CHECK_FALSE(OpsObservationAuthority::create(invalid));
    invalid = grant;
    invalid.logs.permitted_sources = {std::monostate{}};
    CHECK_FALSE(OpsObservationAuthority::create(invalid));
    invalid = grant;
    invalid.logs.permitted_sources.resize(
        33, invalid.logs.permitted_sources.front());
    CHECK_FALSE(OpsObservationAuthority::create(invalid));
  }
}
TEST_CASE("Ops exact reads require matching resource kind namespace and "
          "observed identity") {
  auto grant = spec(true);
  permit_logs(grant, true);
  auto authority = OpsObservationAuthority::create(grant);
  REQUIRE(authority);
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto input = request(grant);
    input.operation = OpsObservationOperation::kubernetes_pod_logs;
    input.resource = pod();
    auto& resource = std::get<KubernetesPodIdentity>(input.resource);
    switch (bad) {
      case 0: resource.namespace_name = "other"; break;
      case 1: resource.name = "../other"; break;
      case 2: resource.container.reset(); break;
      case 3: resource.container->name = ""; break;
      case 4: resource.container->runtime_identity = ""; break;
      case 5:
        resource.uid =
            id<OpsResourceUid>(std::string(1, static_cast<char>(0xff)));
        break;
    }
    CHECK_FALSE(authority->validate(input));
  }
  auto input = request(grant);
  input.operation = OpsObservationOperation::kubernetes_pod_health;
  CHECK_FALSE(authority->validate(input));
  input.resource = service();
  CHECK_FALSE(authority->validate(input));
}
TEST_CASE("Ops inactive service health and truthful Linux scope do not invent "
          "invocation identity") {
  for (auto scope : {LinuxExecutionScope::host, LinuxExecutionScope::container,
                     LinuxExecutionScope::unknown}) {
    auto grant = spec();
    std::get<LinuxOpsIdentity>(grant.target.identity).scope = scope;
    auto authority = OpsObservationAuthority::create(grant);
    REQUIRE(authority);
    auto input = request(grant);
    input.operation = OpsObservationOperation::linux_service_health;
    input.resource = LinuxServiceIdentity{"inactive.service", {}};
    REQUIRE(authority->validate(input));
    input.operation = OpsObservationOperation::linux_service_logs;
    CHECK_FALSE(authority->validate(input));
    grant.logs = {2, true, {input.resource}};
    CHECK_FALSE(OpsObservationAuthority::create(grant));
  }
}
TEST_CASE("Ops every request ceiling narrows and oversized numeric fields "
          "never wrap") {
  const auto grant = spec();
  const auto authority = OpsObservationAuthority::create(grant);
  REQUIRE(authority);
  for (unsigned field = 0; field < 6; ++field) {
    auto input = request(grant);
    switch (field) {
      case 0:
        input.limits.maximum_entries = std::numeric_limits<std::size_t>::max();
        break;
      case 1:
        input.limits.maximum_bytes = std::numeric_limits<std::uint64_t>::max();
        break;
      case 2: input.limits.timeout = std::chrono::milliseconds::max(); break;
      case 3:
        input.limits.maximum_log_lines =
            std::numeric_limits<std::size_t>::max();
        break;
      case 4:
        input.limits.maximum_log_bytes =
            std::numeric_limits<std::uint64_t>::max();
        break;
      case 5: input.limits.maximum_log_age = std::chrono::seconds::max(); break;
    }
    CHECK_FALSE(validate_ops_observation_limits(input.limits));
    CHECK_FALSE(authority->validate(input));
  }
  auto narrowed = request(grant);
  narrowed.limits = {1, 1, std::chrono::milliseconds{1},
                     1, 1, std::chrono::seconds{1}};
  REQUIRE(authority->validate(narrowed));
  auto stale = narrowed;
  stale.log_policy_revision = std::numeric_limits<std::uint64_t>::max();
  CHECK_FALSE(authority->validate(stale));
}
TEST_CASE("Ops log grants refuse foreign or incomplete source selections "
          "before requests") {
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto grant = spec(true);
    permit_logs(grant, true);
    auto& selected =
        std::get<KubernetesPodIdentity>(grant.logs.permitted_sources.front());
    switch (bad) {
      case 0: selected.namespace_name = "foreign"; break;
      case 1: selected.container.reset(); break;
      case 2:
        selected.container->runtime_identity = std::string(513, 'x');
        break;
      case 3: selected.container->name = "invalid/name"; break;
      case 4: grant.logs.permitted_sources = {service()}; break;
      case 5: grant.logs.permitted_sources.clear(); break;
    }
    CHECK_FALSE(OpsObservationAuthority::create(grant));
  }
  auto grant = spec();
  permit_logs(grant);
  auto authority = OpsObservationAuthority::create(grant);
  REQUIRE(authority);
  auto input = request(grant);
  input.operation = OpsObservationOperation::linux_service_health;
  for (const std::string& name :
       std::vector<std::string>{"../bad.service", "bad\nname.service",
                                "name.socket", std::string(256, 'a')}) {
    input.resource = LinuxServiceIdentity{name, {}};
    CHECK_FALSE(authority->validate(input));
  }
}

TEST_CASE("Ops bounded healthy requests preserve exact target independently of "
          "display aliases") {
  auto grant = spec(true);
  auto authority = OpsObservationAuthority::create(grant);
  REQUIRE(authority);
  auto input = request(grant);
  REQUIRE(authority->validate(input));
  REQUIRE(validate_ops_target({grant.target, "Production"}));
  REQUIRE(validate_ops_target({grant.target, "Renamed display"}));
  CHECK(authority->specification() == grant);
  input.operation = OpsObservationOperation::kubernetes_pod_health;
  input.resource = pod();
  REQUIRE(authority->validate(input));
  input.operation = OpsObservationOperation::kubernetes_events;
  REQUIRE(authority->validate(input));
  input.resource = {};
  REQUIRE(authority->validate(input));
  for (const std::string host : {"127.0.0.1", "::1", "2001:db8::1"}) {
    auto target = kube_target();
    std::get<KubernetesOpsIdentity>(target.identity).endpoint.host = host;
    REQUIRE(validate_ops_target_binding(target));
  }
}
