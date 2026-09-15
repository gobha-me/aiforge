#include <aiforge/adapters/linux_ops_observation_source.hpp>
#include <aiforge/adapters/linux_ops_source_preparation.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
unsigned source_creations{};
std::optional<aiforge::runtime::OpsSourcePreparationIdentity> requested;
} // namespace
// Resolve the archived source-construction symbol to this fixed no-IO fixture.
// The real procfs source object is never needed by this test executable.
namespace aiforge::adapters {
auto LinuxOpsObservationSource::create(
    domain::OpsTargetId target, domain::OpsConfigurationRevision revision)
    -> std::expected<std::shared_ptr<LinuxOpsObservationSource>,
                     runtime::OpsObservationSourceError> {
  ++source_creations;
  requested.emplace(runtime::OpsSourcePreparationIdentity{
      std::move(target), std::move(revision),
      domain::OpsTargetKind::linux_local});
  return std::unexpected(runtime::OpsObservationSourceError::unavailable);
}
} // namespace aiforge::adapters

TEST_CASE("Linux preparation factory metadata and early refusals perform no "
          "source IO",
          "[ops][preparation][adapter]") {
  using namespace aiforge;
  using Error = runtime::OpsObservationSourceError;
  source_creations = 0;
  requested.reset();
  auto factory = adapters::LinuxOpsSourcePreparationFactory::create(
      domain::OpsTargetId::from("local").value(),
      domain::OpsConfigurationRevision::from("reserved").value());
  REQUIRE(factory);
  CHECK((*factory)->guarantees_owned_read_only_preparation());
  CHECK((*factory)->preparation_identity().kind ==
        domain::OpsTargetKind::linux_local);
  runtime::OpsSourcePreparationRequest request{
      {domain::SessionId::from("session").value(), 1, 1,
       (*factory)->preparation_identity()},
      std::chrono::steady_clock::now() + std::chrono::seconds{5}};
  std::stop_source stop;
  stop.request_stop();
  auto cancelled = (*factory)->prepare(request, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error() == Error::cancelled);
  request.deadline = std::chrono::steady_clock::time_point::min();
  auto expired = (*factory)->prepare(request);
  REQUIRE_FALSE(expired);
  CHECK(expired.error() == Error::timed_out);
  auto invalid = adapters::LinuxOpsSourcePreparationFactory::create(
      domain::OpsTargetId::from(std::string(1, static_cast<char>(0xff)))
          .value(),
      domain::OpsConfigurationRevision::from("reserved").value());
  REQUIRE_FALSE(invalid);
  CHECK(source_creations == 0);
  request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  auto unavailable = (*factory)->prepare(request);
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error() == Error::unavailable);
  CHECK(source_creations == 1);
  REQUIRE(requested);
  CHECK(*requested == (*factory)->preparation_identity());
}
