#pragma once

#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_observation_tool.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
using namespace aiforge;
using Json = nlohmann::json;
template <class Id> auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}
auto spec() -> domain::OpsObservationAuthoritySpec {
  return {id<domain::OpsOwnerId>("owner"),
          id<domain::SessionId>("session"),
          {id<domain::OpsTargetId>("target"),
           id<domain::OpsConfigurationRevision>("revision"),
           domain::LinuxOpsIdentity{domain::LinuxExecutionScope::unknown,
                                    "12345678-1234-1234-1234-123456789abc", 42,
                                    43}},
          1,
          {domain::OpsObservationOperation::linux_health,
           domain::OpsObservationOperation::linux_service_logs},
          {},
          {}};
}
auto invocation_id() -> domain::InvocationId {
  return id<domain::InvocationId>("invocation");
}
auto input() -> Json {
  return {{"target_id", "target"},
          {"selection_generation", 1},
          {"operation", "linux_health"},
          {"resource", {{"kind", "none"}}}};
}
class Source final : public runtime::OpsObservationSource {
 public:
  domain::OpsTargetBinding binding{spec().target};
  std::atomic<unsigned> calls{};
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return binding;
  }
  auto observe(const domain::OpsObservationRequest& request, std::stop_token)
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++calls;
    return domain::OpsObservation{
        request,
        domain::EventTimestamp{std::chrono::milliseconds{1000}},
        domain::EventTimestamp{std::chrono::milliseconds{1001}},
        domain::OpsObservationCompleteness::complete,
        0,
        0,
        {},
        domain::LinuxHealthObservation{
            domain::OpsHealthState::healthy, 12, {}, 1, 0}};
  }
};
struct Fixture {
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::unique_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint{
      broker->activate_session(spec().session_id).value()};
  std::shared_ptr<Source> source{std::make_shared<Source>()};
  runtime::ToolRegistrySnapshot registry;
  const runtime::OpsObservationTool* tool{};
  Fixture() {
    REQUIRE(broker->select(
        domain::OpsObservationAuthority::create(spec()).value(), source));
    runtime::ToolRegistry registrations;
    REQUIRE(runtime::register_ops_observation_tool(
        registrations, domain::OpsObservationAuthority::create(spec()).value(),
        endpoint));
    registry = registrations.snapshot().value();
    tool = dynamic_cast<const runtime::OpsObservationTool*>(
        registry.find("observe_target")->executor.get());
    REQUIRE(tool != nullptr);
  }
  ~Fixture() { static_cast<void>(broker->close()); }
  auto prepare(Json value) const {
    return tool->prepare(
        invocation_id(),
        domain::StructuredDataBlock{"application/json", value.dump()});
  }
  auto invocation() const -> runtime::ToolInvocation {
    auto arguments = prepare(input());
    REQUIRE(arguments);
    return {invocation_id(),
            {},
            "observe_target",
            *arguments,
            arguments->required_scopes,
            registry.find("observe_target")->limits};
  }
};
} // namespace
