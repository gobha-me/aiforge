#pragma once

#include <aiforge/runtime/ops_observation_source.hpp>
#include <chrono>
#include <memory>

namespace aiforge::runtime {
struct OpsSourcePreparationIdentity {
  domain::OpsTargetId target_id;
  domain::OpsConfigurationRevision configuration_revision;
  domain::OpsTargetKind kind;
  auto operator==(const OpsSourcePreparationIdentity&) const -> bool = default;
};
struct OpsSourcePreparationToken {
  domain::SessionId session_id;
  std::uint64_t session_epoch{};
  std::uint64_t request_id{};
  OpsSourcePreparationIdentity selection;
  auto operator==(const OpsSourcePreparationToken&) const -> bool = default;
};
// Exact-token scheduling metadata, never evidence of authority or admission.
struct OpsSourcePreparationState {
  bool ready{};
  bool physically_outstanding{};
};
struct OpsSourcePreparationRequest {
  OpsSourcePreparationToken token;
  // Established once before admission; neither startup nor preparation resets
  // it.
  std::chrono::steady_clock::time_point deadline;
};
struct PreparedOpsSource {
  std::shared_ptr<OpsObservationSource> source;
};
class OpsSourcePreparationFactory {
 public:
  virtual ~OpsSourcePreparationFactory() = default;
  // Owns the complete immutable configuration graph; concurrent calls perform
  // only bounded read-only source construction. No borrowed app state, ambient
  // discovery, helper/provider calls, mutation or service/log observation.
  // Returned sources transfer physical ownership only through the result: the
  // factory and its retained owners must not retain or reacquire source owners.
  // Both metadata methods are bounded, do no IO and grant no authority.
  [[nodiscard]] virtual auto guarantees_owned_read_only_preparation()
      const noexcept -> bool {
    return false;
  }
  [[nodiscard]] virtual auto preparation_identity() const noexcept
      -> const OpsSourcePreparationIdentity& = 0;
  [[nodiscard]] virtual auto prepare(const OpsSourcePreparationRequest& request,
                                     std::stop_token stop = {})
      -> std::expected<PreparedOpsSource, OpsObservationSourceError> = 0;
};
struct OpsSourcePreparationCompletion {
  OpsSourcePreparationToken token;
  std::expected<PreparedOpsSource, OpsObservationSourceError> result;
};
[[nodiscard]] auto validate_ops_source_preparation_identity(
    const OpsSourcePreparationIdentity& identity) noexcept
    -> std::expected<void, OpsObservationSourceError>;
[[nodiscard]] auto validate_ops_source_preparation_request(
    const OpsSourcePreparationRequest& request,
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now()) noexcept
    -> std::expected<void, OpsObservationSourceError>;
[[nodiscard]] auto validate_ops_source_preparation_result(
    const OpsSourcePreparationRequest& request,
    const PreparedOpsSource& result) noexcept
    -> std::expected<void, OpsObservationSourceError>;
// Prepared sources stay producer-owned until claimed/discarded. A successful
// worker poll transfers ownership and final cleanup responsibility to the app.
// Cancellation/deadlines cannot interrupt blocked OS work or cleanup; physical
// worker capacity remains occupied until both producer and stop relay finish.
} // namespace aiforge::runtime
