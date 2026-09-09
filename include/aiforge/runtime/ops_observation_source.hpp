#pragma once

#include <expected>
#include <stop_token>

#include <aiforge/domain/ops_observation.hpp>

namespace aiforge::runtime {

// Closed source failures contain no raw stderr, credentials or adapter text.
// Presentation supplies fixed application diagnostics for these values.
enum class OpsObservationSourceError {
  unavailable,
  authentication_failed,
  trust_failed,
  permission_denied,
  source_changed,
  disconnected,
  timed_out,
  cancelled,
  unsupported,
  resource_exhausted,
  invalid_result,
  internal_failure
};

// Owns its complete immutable target/private configuration graph. Concurrent
// slots may call one source, so its read-only implementation must support them.
// No widgets, borrowed application state or executable caller-supplied actions.
class OpsObservationSource {
 public:
  virtual ~OpsObservationSource() = default;
  // Both metadata methods are bounded and perform no IO or refresh. This
  // implementation guarantee is not a claim of enforced OS isolation.
  [[nodiscard]] virtual auto guarantees_bound_read_only_observations()
      const noexcept -> bool {
    return false;
  }
  [[nodiscard]] virtual auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& = 0;
  // Stop/deadline checks cannot promise interruption of a stalled OS call.
  // All private response streams/child resources finish cleanup before return;
  // a returned observation owns only bounded neutral values, never credentials.
  [[nodiscard]] virtual auto observe(
      const domain::OpsObservationRequest& request, std::stop_token stop = {})
      -> std::expected<domain::OpsObservation, OpsObservationSourceError> = 0;
};

struct OpsObservationWorkToken {
  domain::SessionId session_id;
  std::uint64_t session_epoch{};
  // Strictly increasing numeric identity shared by every task in the worker,
  // distinct from the observation ID.
  std::uint64_t request_id{};
  domain::OpsObservationRequest observation;
  auto operator==(const OpsObservationWorkToken&) const -> bool = default;
};
struct OpsObservationWorkRequest {
  OpsObservationWorkToken token;
  domain::OpsObservationAuthority authority;
};
struct OpsObservationWorkCompletion {
  OpsObservationWorkToken token;
  std::expected<domain::OpsObservation, OpsObservationSourceError> result;
};

} // namespace aiforge::runtime
