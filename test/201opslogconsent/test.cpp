#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_log_consent.hpp>
#include <aiforge/runtime/ops_observation_broker.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace aiforge;
template <class T> auto id(std::string text) -> T {
  return T::from(std::move(text)).value();
}
auto pod(std::string runtime = "containerd://one")
    -> domain::KubernetesPodIdentity {
  return {"selected", "pod-one", id<domain::OpsResourceUid>("pod-uid"),
          domain::KubernetesContainerIdentity{"main", std::move(runtime)}};
}
auto spec(std::string target = "target")
    -> domain::OpsObservationAuthoritySpec {
  return {id<domain::OpsOwnerId>("owner"),
          id<domain::SessionId>("session"),
          {id<domain::OpsTargetId>(std::move(target)),
           id<domain::OpsConfigurationRevision>("revision"),
           domain::KubernetesOpsIdentity{
               "context", "selected", {"api.example.test", 6443}, "ca"}},
          1,
          {domain::OpsObservationOperation::kubernetes_pod_logs},
          {1, false, {}},
          {}};
}
auto change(const domain::OpsObservationAuthoritySpec& current, bool enabled,
            domain::OpsResourceIdentity source = pod())
    -> runtime::OpsLogConsentChange {
  return {
      current.session_id,    current.target,    current.selection_generation,
      current.logs.revision, std::move(source), enabled};
}
struct Activation {
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::unique_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint{
      broker->activate_session(spec().session_id).value()};
};
auto start(Activation& activation,
           domain::OpsObservationAuthoritySpec value = spec()) {
  return runtime::OpsSessionLogConsent::start(*activation.endpoint,
                                              std::move(value));
}
} // namespace

TEST_CASE("Fresh Ops sessions cannot restore enabled log authority") {
  Activation first_activation;
  Activation restored_activation;
  auto enabled = spec();
  enabled.logs = {2, true, {pod()}};
  REQUIRE_FALSE(start(first_activation, enabled));
  enabled.logs.enabled = false;
  REQUIRE_FALSE(start(first_activation, enabled));

  auto first = start(first_activation);
  auto restored = start(restored_activation);
  REQUIRE(first);
  REQUIRE(restored);
  REQUIRE_FALSE(start(first_activation));
  REQUIRE(first->apply(change(first->authority()->specification(), true)));
  CHECK_FALSE(restored->authority()->specification().logs.enabled);
  CHECK(restored->authority()->specification().logs.permitted_sources.empty());
}

TEST_CASE("Consent changes require exact current session target and source") {
  for (unsigned bad{}; bad < 5; ++bad) {
    Activation activation;
    auto consent = start(activation);
    REQUIRE(consent);
    auto current = consent->authority()->specification();
    auto requested = change(current, true);
    switch (bad) {
      case 0: requested.session_id = id<domain::SessionId>("foreign"); break;
      case 1:
        requested.target.target_id = id<domain::OpsTargetId>("foreign");
        break;
      case 2: ++requested.selection_generation; break;
      case 3: ++requested.log_policy_revision; break;
      case 4:
        std::get<domain::KubernetesPodIdentity>(requested.source)
            .namespace_name = "foreign";
        break;
    }
    REQUIRE_FALSE(consent->apply(requested));
    CHECK(consent->authority()->specification() == current);
  }
}

TEST_CASE("Every effective exact-source change revokes its prior snapshot") {
  Activation activation;
  auto consent = start(activation);
  REQUIRE(consent);
  const auto initial = consent->authority()->specification();
  auto enabled = consent->apply(change(initial, true));
  REQUIRE(enabled);
  const auto granted = enabled->specification();
  CHECK(granted.selection_generation == initial.selection_generation + 1);
  CHECK(granted.logs.revision == initial.logs.revision + 1);
  CHECK(granted.logs.enabled);
  REQUIRE(granted.logs.permitted_sources ==
          std::vector<domain::OpsResourceIdentity>{pod()});
  REQUIRE_FALSE(consent->apply(change(initial, true)));
  REQUIRE_FALSE(consent->apply(change(granted, true)));

  auto disabled = consent->apply(change(granted, false));
  REQUIRE(disabled);
  CHECK(disabled->specification().selection_generation ==
        granted.selection_generation + 1);
  CHECK(disabled->specification().logs.revision == granted.logs.revision + 1);
  CHECK_FALSE(disabled->specification().logs.enabled);
  CHECK(disabled->specification().logs.permitted_sources.empty());
}

TEST_CASE("Target replacement is newer disabled and never imports consent") {
  Activation activation;
  auto consent = start(activation);
  REQUIRE(consent);
  auto granted = consent->apply(change(spec(), true));
  REQUIRE(granted);
  auto next = spec("next-target");
  next.target.configuration_revision =
      id<domain::OpsConfigurationRevision>("next-revision");
  next.selection_generation = granted->specification().selection_generation + 1;
  next.logs.revision = granted->specification().logs.revision + 1;
  auto replaced = consent->replace_selection(next);
  REQUIRE(replaced);
  CHECK_FALSE(replaced->specification().logs.enabled);
  CHECK(replaced->specification().logs.permitted_sources.empty());
  next.selection_generation = granted->specification().selection_generation;
  REQUIRE_FALSE(consent->replace_selection(next));
}

TEST_CASE("Consent revision exhaustion clears sources and fails closed") {
  Activation activation;
  auto value = spec();
  value.selection_generation =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 1;
  value.logs.revision = value.selection_generation;
  auto consent = start(activation, value);
  REQUIRE(consent);
  auto granted = consent->apply(change(value, true));
  REQUIRE(granted);
  REQUIRE(granted->specification().logs.enabled);
  auto result = consent->apply(change(granted->specification(), false));
  REQUIRE_FALSE(result);
  CHECK(result.error().code == domain::OpsTargetErrorCode::resource_exhausted);
  REQUIRE_FALSE(consent->authority());
  REQUIRE_FALSE(consent->apply(change(granted->specification(), false)));
}

TEST_CASE("Consent keeps the exact-source set bounded") {
  Activation activation;
  auto consent = start(activation);
  REQUIRE(consent);
  for (unsigned index{}; index < 32; ++index) {
    const auto current = consent->authority()->specification();
    auto granted = consent->apply(
        change(current, true, pod("containerd://" + std::to_string(index))));
    REQUIRE(granted);
  }
  const auto before = consent->authority()->specification();
  auto excess = consent->apply(change(before, true, pod("containerd://32")));
  REQUIRE_FALSE(excess);
  CHECK(excess.error().code == domain::OpsTargetErrorCode::invalid_authority);
  CHECK(consent->authority()->specification() == before);
}

TEST_CASE("Same SessionId reactivation cannot regain old log consent") {
  Activation activation;
  auto consent = start(activation);
  REQUIRE(consent);
  auto enabled = consent->apply(change(spec(), true));
  REQUIRE(enabled);
  auto replacement =
      activation.broker->activate_session(spec().session_id).value();
  REQUIRE_FALSE(consent->authority());
  REQUIRE_FALSE(consent->apply(change(enabled->specification(), false)));
  auto fresh = runtime::OpsSessionLogConsent::start(*replacement, spec());
  REQUIRE(fresh);
  CHECK_FALSE(fresh->authority()->specification().logs.enabled);
}

TEST_CASE("Explicit consent revocation is immediate and idempotent") {
  Activation activation;
  auto consent = start(activation);
  REQUIRE(consent);
  REQUIRE(consent->apply(change(spec(), true)));
  consent->revoke();
  consent->revoke();
  REQUIRE_FALSE(consent->authority());
  REQUIRE_FALSE(consent->apply(change(spec(), true)));
  auto next = spec("next-target");
  next.selection_generation = 3;
  next.logs.revision = 3;
  REQUIRE_FALSE(consent->replace_selection(std::move(next)));
}
