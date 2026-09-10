#include <aiforge/adapters/linux_ops_observation_source.hpp>
#include <aiforge/adapters/linux_ops_source_preparation.hpp>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
}
LinuxOpsSourcePreparationFactory::LinuxOpsSourcePreparationFactory(
    runtime::OpsSourcePreparationIdentity identity)
    : m_identity(std::move(identity)) {
}
auto LinuxOpsSourcePreparationFactory::create(
    domain::OpsTargetId target, domain::OpsConfigurationRevision revision)
    -> std::expected<std::shared_ptr<LinuxOpsSourcePreparationFactory>, Error> {
  try {
    runtime::OpsSourcePreparationIdentity identity{
        std::move(target), std::move(revision),
        domain::OpsTargetKind::linux_local};
    if (auto valid =
            runtime::validate_ops_source_preparation_identity(identity);
        !valid)
      return std::unexpected(valid.error());
    return std::shared_ptr<LinuxOpsSourcePreparationFactory>{
        new LinuxOpsSourcePreparationFactory{std::move(identity)}};
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
auto LinuxOpsSourcePreparationFactory::prepare(
    const runtime::OpsSourcePreparationRequest& request, std::stop_token stop)
    -> std::expected<runtime::PreparedOpsSource, Error> {
  try {
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    if (auto valid = runtime::validate_ops_source_preparation_request(request);
        !valid)
      return std::unexpected(valid.error());
    if (request.token.selection != m_identity)
      return std::unexpected(Error::invalid_result);
    auto result = LinuxOpsObservationSource::create(
        m_identity.target_id, m_identity.configuration_revision);
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    if (std::chrono::steady_clock::now() >= request.deadline)
      return std::unexpected(Error::timed_out);
    if (!result) return std::unexpected(result.error());
    return runtime::PreparedOpsSource{std::move(*result)};
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
