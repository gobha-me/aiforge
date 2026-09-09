// Failure matrix: malformed/duplicate/deep/oversized JSON; unknown fields and
// operations; invalid IDs, integer boundaries, resource kind mismatch; wrong
// target/generation; disabled log scope; widening limits/effects/scopes;
// missing or altered invocation-bound proof; pre-cancellation and unavailable
// endpoint; stale selection before dispatch/publication. Ordinary executors
// cannot mint receipts. Model-supplied owner/session/request/config/log
// authority is refused. Prepare is pure and bounded; execution returns only a
// receipt, source errors stay closed, and the owner gate alone yields
// observation content.

#include "fixture.hpp"

TEST_CASE("Ops tool refuses ambiguous input and model authority fields") {
  Fixture fixture;
  for (const auto* document :
       {"", "[]", "{}", "{} {}",
        "{\"target_id\":\"target\",\"target_id\":\"other\"}"})
    CHECK_FALSE(fixture.tool->prepare(
        invocation_id(),
        domain::StructuredDataBlock{"application/json", document}));
  for (const auto* field :
       {"owner_id", "session_id", "request_id", "configuration_revision",
        "log_policy_revision", "command"}) {
    auto value = input();
    value[field] = "injected";
    INFO(field);
    CHECK_FALSE(fixture.prepare(value));
  }
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    auto value = input();
    switch (mutation) {
      case 0: value["target_id"] = "foreign"; break;
      case 1: value["selection_generation"] = 2; break;
      case 2: value["selection_generation"] = -1; break;
      case 3: value["selection_generation"] = 1.0; break;
      case 4: value["operation"] = "execute"; break;
      case 5: value["operation"] = "linux_service_logs"; break;
      case 6: value["resource"]["extra"] = true; break;
      case 7:
        value["resource"] = {{"kind", "linux_service"},
                             {"unit_name", "a.service"},
                             {"invocation_id", nullptr}};
        break;
    }
    INFO(mutation);
    CHECK_FALSE(fixture.prepare(value));
    CHECK_FALSE(fixture.tool->validate({"application/json", value.dump()}));
  }
  CHECK_FALSE(fixture.tool->prepare(
      invocation_id(), domain::StructuredDataBlock{"application/json",
                                                   std::string(16385, ' ')}));
  CHECK_FALSE(fixture.tool->prepare(
      invocation_id(), domain::StructuredDataBlock{"application/json",
                                                   std::string(18, '[') + "0" +
                                                       std::string(18, ']')}));
  CHECK(fixture.source->calls == 0);
}
TEST_CASE("typed human intent is bounded before request construction") {
  Fixture fixture;
  runtime::OpsObservationIntent intent{
      spec().target.target_id, 1,
      domain::OpsObservationOperation::linux_service_logs,
      domain::LinuxServiceIdentity{
          std::string(1024U * 1024U, 'x'),
          id<domain::OpsResourceUid>("service-invocation")}};
  CHECK_FALSE(fixture.tool->prepare(invocation_id(), intent));
  auto pending = fixture.broker->pending_work();
  REQUIRE(pending);
  CHECK_FALSE(*pending);
  CHECK(fixture.source->calls == 0);
}
TEST_CASE("Ops preparation cannot broaden configured bounds or reuse another "
          "invocation proof") {
  Fixture fixture;
  auto prepared = fixture.prepare(input());
  REQUIRE(prepared);
  const auto& proof = *prepared->observation_request;
  CHECK(proof.request_id.value() == invocation_id().value());
  CHECK(proof.owner_id == spec().owner_id);
  CHECK(proof.session_id == spec().session_id);
  CHECK(proof.target == spec().target);
  CHECK(proof.log_policy_revision == spec().logs.revision);
  auto normalized = Json::parse(prepared->value.data);
  for (const auto* field :
       {"maximum_entries", "maximum_bytes", "timeout_ms", "maximum_log_lines",
        "maximum_log_bytes", "maximum_log_age_seconds"}) {
    auto value = normalized;
    value["limits"][field] = 0;
    INFO(field);
    CHECK_FALSE(fixture.prepare(value));
    value = normalized;
    value["limits"][field] = value["limits"][field].get<std::uint64_t>() + 1;
    CHECK_FALSE(fixture.prepare(value));
  }
  normalized["limits"]["maximum_entries"] = 1;
  CHECK(fixture.prepare(normalized));
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    auto invocation = fixture.invocation();
    switch (mutation) {
      case 0: invocation.arguments.observation_request.reset(); break;
      case 1:
        invocation.invocation_id = id<domain::InvocationId>("different");
        break;
      case 2:
        invocation.arguments.observation_request->log_policy_revision = 2;
        break;
      case 3: invocation.granted_scopes.clear(); break;
      case 4:
        invocation.arguments.required_effects.push_back(domain::Effect::write);
        break;
      case 5: invocation.parent_invocation_id = invocation_id(); break;
    }
    INFO(mutation);
    CHECK_FALSE(fixture.registry.find("observe_target")
                    ->executor->start(invocation, {}));
  }
  CHECK(fixture.source->calls == 0);
}
TEST_CASE("Ops scope grants never cover generic effects or mutation") {
  const domain::CapabilityScope read{domain::Effect::read, "ops.target",
                                     "target"};
  REQUIRE(runtime::normalize_capability_scope(read));
  CHECK(runtime::capability_scope_covers(read, read));
  CHECK_FALSE(runtime::capability_scope_covers(
      read, {domain::Effect::read, "ops.target", "foreign"}));
  CHECK_FALSE(runtime::capability_scope_covers(
      read, {domain::Effect::read, "filesystem.root", "/"}));
  CHECK_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::write, "ops.target", "target"}));
  CHECK_FALSE(runtime::normalize_capability_scope(
      {domain::Effect::change_infrastructure, "ops.target", "target"}));
  // Opaque target IDs have exact equality; '*' is never a wildcard grant.
  CHECK_FALSE(runtime::capability_scope_covers(
      {domain::Effect::read, "ops.target", "*"}, read));
}
TEST_CASE("Ops registry excludes disabled logs and narrows health effects") {
  Fixture fixture;
  const auto schema = Json::parse(
      fixture.registry.find("observe_target")->declaration.input_schema.data);
  CHECK(schema["properties"]["operation"]["enum"] ==
        Json::array({"linux_health"}));
  auto prepared = fixture.prepare(input());
  REQUIRE(prepared);
  CHECK(prepared->required_effects == std::vector{domain::Effect::read});
  CHECK(prepared->required_scopes ==
        std::vector{domain::CapabilityScope{domain::Effect::read, "ops.target",
                                            "target"}});
  std::stop_source stop;
  stop.request_stop();
  CHECK_FALSE(fixture.registry.find("observe_target")
                  ->executor->start(fixture.invocation(), stop.get_token()));
  CHECK(fixture.source->calls == 0);
}
TEST_CASE(
    "Ops stream fails closed after endpoint invalidation without reading") {
  Fixture fixture;
  auto stream = fixture.registry.find("observe_target")
                    ->executor->start(fixture.invocation(), {});
  REQUIRE(stream);
  REQUIRE(fixture.broker->deactivate_session());
  auto result = (*stream)->next({});
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::ToolExecutionErrorCode::unavailable);
  CHECK_FALSE(result.error().retryable);
  CHECK(fixture.source->calls == 0);
}
TEST_CASE("ordinary preparation preserves the existing validation contract") {
  class Ordinary final : public runtime::ToolExecutor {
   public:
    mutable unsigned calls{};
    auto validate(const domain::StructuredDataBlock& value) const
        -> std::expected<runtime::ValidatedToolArguments,
                         runtime::ToolExecutionError> override {
      ++calls;
      return runtime::ValidatedToolArguments{value};
    }
    auto start(runtime::ToolInvocation, std::stop_token)
        -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                         runtime::ToolExecutionError> override {
      return std::unexpected(runtime::ToolExecutionError{
          runtime::ToolExecutionErrorCode::unavailable, "fixture unavailable",
          false});
    }
  } executor;
  const domain::StructuredDataBlock arguments{"application/json", "{}"};
  auto prepared = executor.prepare(invocation_id(), arguments);
  REQUIRE(prepared);
  CHECK(prepared->value == arguments);
  CHECK_FALSE(prepared->observation_request);
  CHECK(executor.calls == 1);
}
TEST_CASE("Ops stream returns only an owner-consumable receipt") {
  Fixture fixture;
  const auto invocation = fixture.invocation();
  auto stream =
      fixture.registry.find("observe_target")->executor->start(invocation, {});
  REQUIRE(stream);
  CHECK(fixture.source->calls == 0);
  auto future =
      std::async(std::launch::async, [&] { return (*stream)->next({}); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{3};
  bool serviced = true;
  while (future.wait_for(std::chrono::milliseconds{0}) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    serviced = fixture.broker->service().has_value();
    if (!serviced) break;
    std::this_thread::yield();
  }
  REQUIRE(serviced);
  REQUIRE(future.wait_for(std::chrono::milliseconds{0}) ==
          std::future_status::ready);
  auto event = future.get();
  REQUIRE(event);
  REQUIRE(event->has_value());
  const auto* ready = std::get_if<runtime::OpsObservationReady>(&**event);
  REQUIRE(ready != nullptr);
  const auto receipt_copy = ready->receipt;
  CHECK(receipt_copy == ready->receipt);
  auto result = fixture.broker->take_for_publication(
      ready->receipt, invocation.invocation_id,
      *invocation.arguments.observation_request);
  REQUIRE(result);
  CHECK(result->request == *invocation.arguments.observation_request);
  CHECK_FALSE(fixture.broker->take_for_publication(
      receipt_copy, invocation.invocation_id,
      *invocation.arguments.observation_request));
  auto end = (*stream)->next({});
  REQUIRE(end);
  CHECK_FALSE(end->has_value());
  CHECK(fixture.source->calls == 1);
}
