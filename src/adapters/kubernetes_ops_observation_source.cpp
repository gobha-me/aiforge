#include "kubernetes_ops_observation_source.hpp"
#include "kubernetes_credential_exclusion.hpp"
#include "kubernetes_https_read.hpp"
#include <algorithm>
#include <utility>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
using Clock = std::chrono::steady_clock;
auto validation_error(domain::OpsTargetErrorCode code) -> Error {
  if (code == domain::OpsTargetErrorCode::internal_failure)
    return Error::internal_failure;
  if (code == domain::OpsTargetErrorCode::resource_exhausted)
    return Error::resource_exhausted;
  return Error::invalid_result;
}
} // namespace
struct KubernetesOpsObservationSource::Impl {
  const domain::OpsTargetBinding binding;
  const StaticKubernetesConfig configuration;
};
KubernetesOpsObservationSource::KubernetesOpsObservationSource(
    std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {
}
KubernetesOpsObservationSource::~KubernetesOpsObservationSource() = default;
auto KubernetesOpsObservationSource::create(
    const domain::OpsTargetBinding& binding,
    StaticKubernetesConfig configuration) noexcept
    -> std::expected<std::shared_ptr<KubernetesOpsObservationSource>, Error> {
  try {
    const auto* identity =
        std::get_if<domain::KubernetesOpsIdentity>(&binding.identity);
    const auto validated = domain::validate_ops_target_binding(binding);
    if (!validated)
      return std::unexpected(validation_error(validated.error().code));
    if (identity == nullptr || *identity != configuration.identity() ||
        configuration.configuration_bytes().empty() ||
        KubernetesCredentialExclusion{configuration.tls_material()}.contains(
            binding))
      return std::unexpected(Error::invalid_result);
    auto impl = std::make_unique<Impl>(binding, std::move(configuration));
    return std::shared_ptr<KubernetesOpsObservationSource>(
        new KubernetesOpsObservationSource(std::move(impl)));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
auto KubernetesOpsObservationSource::target_binding() const noexcept
    -> const domain::OpsTargetBinding& {
  return m_impl->binding;
}
auto KubernetesOpsObservationSource::guarantees_bound_read_only_observations()
    const noexcept -> bool {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}
auto KubernetesOpsObservationSource::observe(
    const domain::OpsObservationRequest& request, std::stop_token stop)
    -> std::expected<domain::OpsObservation, Error> {
  try {
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    const auto began = Clock::now();
    const auto validated = domain::validate_recorded_ops_request(request);
    if (!validated)
      return std::unexpected(validation_error(validated.error().code));
    const auto deadline = began + request.limits.timeout;
    if (request.target != m_impl->binding)
      return std::unexpected(Error::source_changed);
    if (std::ranges::find(supported_operations, request.operation) ==
        supported_operations.end())
      return std::unexpected(Error::unsupported);
    if (KubernetesCredentialExclusion{m_impl->configuration.tls_material()}
            .contains(request))
      return std::unexpected(Error::invalid_result);
    const auto started = domain::EventTimestamp{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())};
    return read_kubernetes_observation(m_impl->configuration, request, deadline,
                                       started, stop);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
