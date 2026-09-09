#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_observation_tool.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
class Source final : public runtime::OpsObservationSource {
 public:
  domain::OpsTargetBinding binding{
      id<domain::OpsTargetId>("target"),
      id<domain::OpsConfigurationRevision>("revision"),
      domain::LinuxOpsIdentity{domain::LinuxExecutionScope::container,
                               "12345678-1234-1234-1234-123456789abc", 42, 43}};
  unsigned calls{};
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return binding;
  }
  auto observe(const domain::OpsObservationRequest&, std::stop_token)
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++calls;
    return std::unexpected(runtime::OpsObservationSourceError::unavailable);
  }
};
} // namespace

TEST_CASE("native endpoint preflight binds exact issuer and prepared proof "
          "without allocating work",
          "[ops][preflight]") {
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  auto broker = runtime::OpsObservationBroker::create(worker).value();
  auto source = std::make_shared<Source>();
  const auto session = id<domain::SessionId>("session");
  domain::OpsObservationAuthoritySpec spec{
      id<domain::OpsOwnerId>("owner"),
      session,
      source->binding,
      1,
      {domain::OpsObservationOperation::linux_health},
      {},
      {}};
  auto authority = domain::OpsObservationAuthority::create(spec).value();
  auto endpoint = broker->activate_session(session).value();
  REQUIRE(broker->select(authority, source));
  runtime::ToolRegistry registry;
  REQUIRE(
      runtime::register_ops_observation_tool(registry, authority, endpoint));
  const auto snapshot = registry.snapshot().value();
  const auto* native = dynamic_cast<const runtime::OpsObservationTool*>(
      snapshot.find("observe_target")->executor.get());
  REQUIRE(native != nullptr);
  const auto invocation = id<domain::InvocationId>("invocation");
  auto prepared =
      native
          ->prepare(invocation,
                    runtime::OpsObservationIntent{
                        spec.target.target_id, 1,
                        domain::OpsObservationOperation::linux_health})
          .value();
  REQUIRE(native->check_current(*broker, invocation, prepared));
  SECTION("foreign issuer with identical identities") {
    auto foreign = runtime::OpsObservationBroker::create(worker).value();
    REQUIRE(foreign->activate_session(session));
    REQUIRE(foreign->select(authority, source));
    REQUIRE_FALSE(native->check_current(*foreign, invocation, prepared));
    REQUIRE(foreign->close());
  }
  SECTION("another invocation cannot reuse proof") {
    REQUIRE_FALSE(native->check_current(
        *broker, id<domain::InvocationId>("other"), prepared));
  }
  SECTION("owner proof cannot be altered") {
    prepared.observation_request->owner_id = id<domain::OpsOwnerId>("other");
    REQUIRE_FALSE(native->check_current(*broker, invocation, prepared));
  }
  SECTION("normalized arguments cannot disagree with proof") {
    prepared.value.data = "{}";
    REQUIRE_FALSE(native->check_current(*broker, invocation, prepared));
  }
  SECTION("effects cannot exceed native preparation") {
    prepared.required_effects.push_back(domain::Effect::write);
    REQUIRE_FALSE(native->check_current(*broker, invocation, prepared));
  }
  SECTION("replacement session invalidates old endpoint") {
    REQUIRE(broker->activate_session(session));
    REQUIRE(broker->select(authority, source));
    REQUIRE_FALSE(native->check_current(*broker, invocation, prepared));
  }
  SECTION("revoked selection rejects unchanged proof") {
    ++spec.selection_generation;
    REQUIRE(broker->select(
        domain::OpsObservationAuthority::create(spec).value(), source));
    REQUIRE_FALSE(native->check_current(*broker, invocation, prepared));
  }
  REQUIRE_FALSE(broker->pending_work().value());
  REQUIRE(source->calls == 0);
  REQUIRE(broker->close());
}
