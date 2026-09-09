// Resource failure matrix: namespace/kind/source/runtime mismatches, disabled
// logs, absent exact identities, extra JSON fields and widened limits refuse
// pure preparation. Health/list monostate remains valid; inspection/log reads
// require the matching exact resource. Model and typed preparation must agree.
// No source is selected and no broker pump runs in these parser-only fixtures.

#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_observation_tool.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
using namespace aiforge;
using Json = nlohmann::json;
using Operation = domain::OpsObservationOperation;
template <class Id> auto resource_id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}
auto resource_invocation() -> domain::InvocationId {
  return resource_id<domain::InvocationId>("resource-invocation");
}
auto selected_pod() -> domain::KubernetesPodIdentity {
  return {
      "apps", "database-0", resource_id<domain::OpsResourceUid>("pod-uid"),
      domain::KubernetesContainerIdentity{"postgres", "containerd://runtime"}};
}
auto pod_json(const domain::KubernetesPodIdentity& pod) -> Json {
  Json container = nullptr;
  if (pod.container)
    container = {{"name", pod.container->name},
                 {"runtime_identity", pod.container->runtime_identity}};
  return {{"kind", "kubernetes_pod"},
          {"namespace_name", pod.namespace_name},
          {"name", pod.name},
          {"uid", std::string(pod.uid.value())},
          {"container", std::move(container)}};
}
auto selected_service() -> domain::LinuxServiceIdentity {
  return {"database.service",
          resource_id<domain::OpsResourceUid>("service-invocation")};
}
auto service_json(const domain::LinuxServiceIdentity& service) -> Json {
  return {
      {"kind", "linux_service"},
      {"unit_name", service.unit_name},
      {"invocation_id", service.invocation_id
                            ? Json(std::string(service.invocation_id->value()))
                            : Json(nullptr)}};
}
auto resources_spec(bool kubernetes) -> domain::OpsObservationAuthoritySpec {
  domain::OpsTargetIdentity target =
      domain::LinuxOpsIdentity{domain::LinuxExecutionScope::unknown,
                               "12345678-1234-1234-1234-123456789abc", 42, 43};
  std::vector<Operation> operations{
      Operation::linux_health, Operation::linux_services,
      Operation::linux_service_health, Operation::linux_service_logs};
  domain::OpsResourceIdentity source = selected_service();
  if (kubernetes) {
    target = domain::KubernetesOpsIdentity{"private-context",
                                           "apps",
                                           {"cluster.internal", 443},
                                           "public-ca-revision"};
    operations = {Operation::kubernetes_workloads,
                  Operation::kubernetes_pod_health,
                  Operation::kubernetes_events, Operation::kubernetes_pod_logs};
    source = selected_pod();
  }
  return {resource_id<domain::OpsOwnerId>("resource-owner"),
          resource_id<domain::SessionId>("resource-session"),
          {resource_id<domain::OpsTargetId>("resource-target"),
           resource_id<domain::OpsConfigurationRevision>("config-revision"),
           std::move(target)},
          7,
          std::move(operations),
          {3, true, {std::move(source)}},
          {64, 65536, std::chrono::milliseconds{2000}, 40, 8192,
           std::chrono::seconds{60}}};
}
struct ResourceFixture {
  domain::OpsObservationAuthoritySpec specification;
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::unique_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  runtime::ToolRegistrySnapshot registry;
  const runtime::OpsObservationTool* tool{};
  explicit ResourceFixture(domain::OpsObservationAuthoritySpec value)
      : specification(std::move(value)) {
    auto endpoint = broker->activate_session(specification.session_id);
    REQUIRE(endpoint);
    auto authority = domain::OpsObservationAuthority::create(specification);
    REQUIRE(authority);
    runtime::ToolRegistry registrations;
    REQUIRE(runtime::register_ops_observation_tool(registrations, *authority,
                                                   *endpoint));
    registry = registrations.snapshot().value();
    tool = dynamic_cast<const runtime::OpsObservationTool*>(
        registry.find("observe_target")->executor.get());
    REQUIRE(tool != nullptr);
  }
  ~ResourceFixture() { [[maybe_unused]] const auto closed = broker->close(); }
  auto intent(Operation operation,
              domain::OpsResourceIdentity resource = {}) const
      -> runtime::OpsObservationIntent {
    return {specification.target.target_id, specification.selection_generation,
            operation, std::move(resource), specification.limits};
  }
  auto input(std::string operation, Json resource = {{"kind", "none"}}) const
      -> Json {
    return {{"target_id", std::string(specification.target.target_id.value())},
            {"selection_generation", specification.selection_generation},
            {"operation", std::move(operation)},
            {"resource", std::move(resource)}};
  }
  auto prepare(const Json& value) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> {
    return tool->prepare(
        resource_invocation(),
        domain::StructuredDataBlock{"application/json", value.dump()});
  }
  auto parity(const runtime::OpsObservationIntent& intent,
              const Json& input) const -> runtime::ValidatedToolArguments {
    auto model = prepare(input);
    auto typed = tool->prepare(resource_invocation(), intent);
    auto validated = tool->validate({"application/json", input.dump()});
    REQUIRE(model);
    REQUIRE(typed);
    REQUIRE(validated);
    CHECK(*model == *typed);
    auto without_proof = *model;
    without_proof.observation_request.reset();
    CHECK(*validated == without_proof);
    REQUIRE(model->observation_request);
    CHECK(model->observation_request->target == specification.target);
    CHECK(model->observation_request->resource == intent.resource);
    CHECK(model->observation_request->limits == intent.limits);
    CHECK(model->observation_request->log_policy_revision ==
          specification.logs.revision);
    CHECK(model->observation_request->request_id.value() ==
          resource_invocation().value());
    return *model;
  }
  auto rejected(const runtime::OpsObservationIntent& intent,
                const Json& input) const -> void {
    CHECK_FALSE(prepare(input));
    CHECK_FALSE(tool->prepare(resource_invocation(), intent));
    CHECK_FALSE(tool->validate({"application/json", input.dump()}));
  }
  auto unchanged() const -> void {
    auto pending = broker->pending_work();
    REQUIRE(pending);
    CHECK_FALSE(*pending);
    CHECK(worker->occupied_slots() == 0);
  }
};
} // namespace

TEST_CASE("Kubernetes tool rejects foreign and incomplete exact resources") {
  ResourceFixture fixture{resources_spec(true)};
  auto pod = selected_pod();
  SECTION("foreign namespace") {
    pod.namespace_name = "other";
  }
  SECTION("foreign selected pod UID") {
    pod.uid = resource_id<domain::OpsResourceUid>("replacement");
  }
  SECTION("foreign selected pod name") {
    pod.name = "database-1";
  }
  SECTION("foreign container") {
    pod.container->name = "sidecar";
  }
  SECTION("changed runtime") {
    pod.container->runtime_identity = "containerd://replacement";
  }
  SECTION("missing runtime") {
    pod.container->runtime_identity.clear();
  }
  SECTION("missing container") {
    pod.container.reset();
  }
  fixture.rejected(fixture.intent(Operation::kubernetes_pod_logs, pod),
                   fixture.input("kubernetes_pod_logs", pod_json(pod)));
  fixture.unchanged();
}

TEST_CASE("Ops tool rejects resource variant mismatches while preserving list "
          "monostate") {
  ResourceFixture fixture{resources_spec(true)};
  for (const auto operation :
       {Operation::kubernetes_pod_health, Operation::kubernetes_pod_logs}) {
    const auto name = operation == Operation::kubernetes_pod_health
                          ? "kubernetes_pod_health"
                          : "kubernetes_pod_logs";
    INFO(name);
    fixture.rejected(fixture.intent(operation), fixture.input(name));
    fixture.rejected(fixture.intent(operation, selected_service()),
                     fixture.input(name, service_json(selected_service())));
  }
  fixture.rejected(
      fixture.intent(Operation::kubernetes_workloads, selected_pod()),
      fixture.input("kubernetes_workloads", pod_json(selected_pod())));
  fixture.rejected(
      fixture.intent(Operation::kubernetes_events, selected_service()),
      fixture.input("kubernetes_events", service_json(selected_service())));
  fixture.unchanged();
}

TEST_CASE("Resource shape and namespace checks apply beyond consented logs") {
  SECTION("Kubernetes inspection and events reject foreign namespace") {
    ResourceFixture fixture{resources_spec(true)};
    auto pod = selected_pod();
    pod.namespace_name = "other";
    fixture.rejected(fixture.intent(Operation::kubernetes_pod_health, pod),
                     fixture.input("kubernetes_pod_health", pod_json(pod)));
    fixture.rejected(fixture.intent(Operation::kubernetes_events, pod),
                     fixture.input("kubernetes_events", pod_json(pod)));
    fixture.unchanged();
  }
  SECTION("Linux inspection requires the Linux service variant") {
    ResourceFixture fixture{resources_spec(false)};
    fixture.rejected(fixture.intent(Operation::linux_service_health),
                     fixture.input("linux_service_health"));
    fixture.rejected(
        fixture.intent(Operation::linux_service_health, selected_pod()),
        fixture.input("linux_service_health", pod_json(selected_pod())));
    fixture.rejected(
        fixture.intent(Operation::linux_services, selected_service()),
        fixture.input("linux_services", service_json(selected_service())));
    fixture.unchanged();
  }
}

TEST_CASE("Selected-source logs reject disabled policy and missing Linux "
          "invocation") {
  SECTION("disabled Kubernetes logs") {
    auto spec = resources_spec(true);
    spec.logs.enabled = false;
    ResourceFixture fixture{std::move(spec)};
    fixture.rejected(
        fixture.intent(Operation::kubernetes_pod_logs, selected_pod()),
        fixture.input("kubernetes_pod_logs", pod_json(selected_pod())));
    fixture.unchanged();
  }
  SECTION("disabled Linux logs") {
    auto spec = resources_spec(false);
    spec.logs.enabled = false;
    ResourceFixture fixture{std::move(spec)};
    fixture.rejected(
        fixture.intent(Operation::linux_service_logs, selected_service()),
        fixture.input("linux_service_logs", service_json(selected_service())));
    fixture.unchanged();
  }
  SECTION("missing Linux invocation") {
    ResourceFixture fixture{resources_spec(false)};
    auto service = selected_service();
    service.invocation_id.reset();
    fixture.rejected(
        fixture.intent(Operation::linux_service_logs, service),
        fixture.input("linux_service_logs", service_json(service)));
    fixture.unchanged();
  }
  SECTION("replaced Linux invocation") {
    ResourceFixture fixture{resources_spec(false)};
    auto service = selected_service();
    service.invocation_id = resource_id<domain::OpsResourceUid>("replacement");
    fixture.rejected(
        fixture.intent(Operation::linux_service_logs, service),
        fixture.input("linux_service_logs", service_json(service)));
    fixture.unchanged();
  }
  SECTION("foreign Linux service") {
    ResourceFixture fixture{resources_spec(false)};
    auto service = selected_service();
    service.unit_name = "other.service";
    fixture.rejected(
        fixture.intent(Operation::linux_service_logs, service),
        fixture.input("linux_service_logs", service_json(service)));
    fixture.unchanged();
  }
}

TEST_CASE(
    "Kubernetes model resources refuse extra fields and invalid field types") {
  ResourceFixture fixture{resources_spec(true)};
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto value = fixture.input("kubernetes_pod_logs", pod_json(selected_pod()));
    switch (mutation) {
      case 0: value["resource"]["container"]["image"] = "injected"; break;
      case 1: value["resource"]["uid"] = 17; break;
      case 2:
        value["resource"]["container"]["runtime_identity"] = nullptr;
        break;
      case 3: value["resource"]["namespace_name"] = ""; break;
      case 4: value["resource"]["container"].erase("runtime_identity"); break;
    }
    INFO(mutation);
    CHECK_FALSE(fixture.prepare(value));
    CHECK_FALSE(fixture.tool->validate({"application/json", value.dump()}));
  }
  fixture.unchanged();
}

TEST_CASE("Kubernetes resource preparation narrows every configured bound") {
  ResourceFixture fixture{resources_spec(true)};
  auto intent = fixture.intent(Operation::kubernetes_pod_logs, selected_pod());
  auto baseline = fixture.parity(
      intent, fixture.input("kubernetes_pod_logs", pod_json(selected_pod())));
  auto value = Json::parse(baseline.value.data);
  unsigned bound{};
  for (const auto* field :
       {"maximum_entries", "maximum_bytes", "timeout_ms", "maximum_log_lines",
        "maximum_log_bytes", "maximum_log_age_seconds"}) {
    auto widened = value;
    widened["limits"][field] =
        widened["limits"][field].get<std::uint64_t>() + 1;
    INFO(field);
    CHECK_FALSE(fixture.prepare(widened));
    CHECK_FALSE(fixture.tool->validate({"application/json", widened.dump()}));
    auto widened_intent = intent;
    switch (bound++) {
      case 0: ++widened_intent.limits.maximum_entries; break;
      case 1: ++widened_intent.limits.maximum_bytes; break;
      case 2:
        widened_intent.limits.timeout += std::chrono::milliseconds{1};
        break;
      case 3: ++widened_intent.limits.maximum_log_lines; break;
      case 4: ++widened_intent.limits.maximum_log_bytes; break;
      case 5:
        widened_intent.limits.maximum_log_age += std::chrono::seconds{1};
        break;
    }
    CHECK_FALSE(fixture.tool->prepare(resource_invocation(), widened_intent));
    value["limits"][field] = value["limits"][field].get<std::uint64_t>() / 2;
  }
  intent.limits = {32, 32768, std::chrono::milliseconds{1000},
                   20, 4096,  std::chrono::seconds{30}};
  static_cast<void>(fixture.parity(intent, value));
  fixture.unchanged();
}

TEST_CASE(
    "Kubernetes operations preserve exact resources and model typed parity") {
  ResourceFixture fixture{resources_spec(true)};
  auto pod = selected_pod();
  pod.container.reset();
  const std::vector<domain::Effect> expected_effects{
      domain::Effect::read, domain::Effect::execute, domain::Effect::network};
  for (const auto& prepared :
       {fixture.parity(fixture.intent(Operation::kubernetes_workloads),
                       fixture.input("kubernetes_workloads")),
        fixture.parity(fixture.intent(Operation::kubernetes_events),
                       fixture.input("kubernetes_events")),
        fixture.parity(fixture.intent(Operation::kubernetes_events, pod),
                       fixture.input("kubernetes_events", pod_json(pod))),
        fixture.parity(fixture.intent(Operation::kubernetes_pod_health, pod),
                       fixture.input("kubernetes_pod_health", pod_json(pod))),
        fixture.parity(
            fixture.intent(Operation::kubernetes_pod_logs, selected_pod()),
            fixture.input("kubernetes_pod_logs", pod_json(selected_pod())))}) {
    CHECK(prepared.required_effects == expected_effects);
    REQUIRE(prepared.required_scopes.size() == expected_effects.size());
    for (std::size_t index = 0; index < expected_effects.size(); ++index)
      CHECK(prepared.required_scopes[index] ==
            domain::CapabilityScope{expected_effects[index], "ops.target",
                                    "resource-target"});
  }
  fixture.unchanged();
}

TEST_CASE(
    "Linux list inspection and consented logs share typed model preparation") {
  ResourceFixture fixture{resources_spec(false)};
  auto inactive = selected_service();
  inactive.invocation_id.reset();
  const auto list = fixture.parity(fixture.intent(Operation::linux_services),
                                   fixture.input("linux_services"));
  const auto service = fixture.parity(
      fixture.intent(Operation::linux_service_health, inactive),
      fixture.input("linux_service_health", service_json(inactive)));
  const auto logs = fixture.parity(
      fixture.intent(Operation::linux_service_logs, selected_service()),
      fixture.input("linux_service_logs", service_json(selected_service())));
  for (const auto* prepared : {&list, &service, &logs})
    CHECK(prepared->required_effects ==
          std::vector{domain::Effect::read, domain::Effect::execute});
  fixture.unchanged();
}
