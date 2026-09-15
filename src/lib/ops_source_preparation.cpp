#include <aiforge/runtime/ops_source_preparation.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>

namespace aiforge::runtime {
namespace {
using Error = OpsObservationSourceError;
auto identifier(const auto& value) -> bool {
  const auto text = value.value();
  return !text.empty() && text.size() <= 128 &&
         detail::is_safe_utf8_text(text) &&
         std::ranges::none_of(
             text, [](unsigned char c) { return c < 32 || c == 127; });
}
} // namespace
auto validate_ops_source_preparation_identity(
    const OpsSourcePreparationIdentity& identity) noexcept
    -> std::expected<void, Error> {
  if (!identifier(identity.target_id) ||
      !identifier(identity.configuration_revision) ||
      (identity.kind != domain::OpsTargetKind::linux_local &&
       identity.kind != domain::OpsTargetKind::kubernetes &&
       identity.kind != domain::OpsTargetKind::ceph))
    return std::unexpected(Error::invalid_result);
  return {};
}
auto validate_ops_source_preparation_request(
    const OpsSourcePreparationRequest& request,
    std::chrono::steady_clock::time_point now) noexcept
    -> std::expected<void, Error> {
  if (!identifier(request.token.session_id) ||
      request.token.session_epoch == 0 || request.token.request_id == 0 ||
      !validate_ops_source_preparation_identity(request.token.selection))
    return std::unexpected(Error::invalid_result);
  if (request.deadline <= now) return std::unexpected(Error::timed_out);
  // Compare absolute endpoints without subtracting potentially extreme inputs.
  constexpr auto maximum = std::chrono::seconds{5};
  if (now > std::chrono::steady_clock::time_point::max() - maximum ||
      request.deadline > now + maximum)
    return std::unexpected(Error::invalid_result);
  return {};
}
auto validate_ops_source_preparation_result(
    const OpsSourcePreparationRequest& request,
    const PreparedOpsSource& result) noexcept -> std::expected<void, Error> {
  try {
    if (!result.source ||
        !result.source->guarantees_bound_read_only_observations())
      return std::unexpected(Error::invalid_result);
    const auto& binding = result.source->target_binding();
    const auto& selected = request.token.selection;
    if (binding.target_id != selected.target_id ||
        binding.configuration_revision != selected.configuration_revision ||
        domain::ops_target_kind(binding) != selected.kind ||
        !domain::validate_ops_target_binding(binding))
      return std::unexpected(Error::invalid_result);
    return {};
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
} // namespace aiforge::runtime
