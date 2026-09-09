#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>

#include <aiforge/runtime/ops_observation_source.hpp>

namespace aiforge::runtime {

class LocalSourceWorker;
struct OpsObservationMailbox;
class OpsObservationBroker;
class OpsObservationEndpoint;

enum class OpsBrokerError {
  invalid_request,
  closed,
  busy,
  stale_request,
  timed_out,
  cancelled,
  source_failure,
  internal_failure,
  resource_exhausted
};
struct OpsBrokerFailure {
  OpsBrokerError code;
  std::optional<OpsObservationSourceError> source{};
  auto operator==(const OpsBrokerFailure&) const -> bool = default;
};
struct OpsBrokerLimits {
  std::size_t maximum_pending{2};
};

// Runtime-only, issuer-bound identity; never a durable grant or observation.
// Copies share single-consumption semantics. Only the issuing endpoint can
// create receipts. A receipt exposes no evidence, source or private config.
class OpsObservationReceipt final {
 public:
  OpsObservationReceipt(const OpsObservationReceipt&) = default;
  auto operator=(const OpsObservationReceipt&)
      -> OpsObservationReceipt& = default;
  OpsObservationReceipt(OpsObservationReceipt&&) noexcept = default;
  auto operator=(OpsObservationReceipt&&) noexcept
      -> OpsObservationReceipt& = default;

 private:
  friend class OpsObservationBroker;
  friend class OpsObservationEndpoint;
  OpsObservationReceipt(std::weak_ptr<const OpsObservationMailbox> issuer,
                        std::uint64_t ticket);
  std::weak_ptr<const OpsObservationMailbox> m_issuer;
  std::uint64_t m_ticket;
};

// Owning thread-safe mailbox handle. It never owns or calls the worker, source,
// application or UI. Call from an executor thread while the owner pumps
// service. Duplicate IDs are refused while pending. The kernel owns durable
// session-wide invocation uniqueness; this bounded mailbox is not a replay
// ledger.
class OpsObservationEndpoint final {
 public:
  [[nodiscard]] auto observe(const domain::InvocationId& invocation,
                             const domain::OpsObservationRequest& request,
                             std::chrono::steady_clock::time_point deadline,
                             std::stop_token stop = {})
      -> std::expected<OpsObservationReceipt, OpsBrokerFailure>;

 private:
  friend class OpsObservationBroker;
  explicit OpsObservationEndpoint(std::shared_ptr<OpsObservationMailbox> state);
  std::shared_ptr<OpsObservationMailbox> m_state;
};

// All methods except Endpoint::observe are application owner-thread operations.
// One shared worker supplies physical capacity; the mailbox bounds all queued,
// executing and completed-but-unpublished observations independently.
class OpsObservationBroker final {
 public:
  static constexpr std::size_t maximum_pending{8};
  [[nodiscard]] static auto create(std::shared_ptr<LocalSourceWorker> worker,
                                   OpsBrokerLimits limits = {})
      -> std::expected<std::unique_ptr<OpsObservationBroker>, OpsBrokerFailure>;
  ~OpsObservationBroker();
  OpsObservationBroker(const OpsObservationBroker&) = delete;
  auto operator=(const OpsObservationBroker&) -> OpsObservationBroker& = delete;

  // Permanently closes the previous endpoint, including activation of the same
  // session ID. Each activation has a distinct issuer and advancing epoch.
  [[nodiscard]] auto activate_session(domain::SessionId session)
      -> std::expected<std::shared_ptr<OpsObservationEndpoint>,
                       OpsBrokerFailure>;
  // Every selection, including a log-only update, advances generation. For the
  // same target, log revision cannot decrease; changed policy must advance it.
  [[nodiscard]] auto select(domain::OpsObservationAuthority authority,
                            std::shared_ptr<OpsObservationSource> source)
      -> std::expected<void, OpsBrokerFailure>;
  [[nodiscard]] auto service(std::chrono::steady_clock::time_point now =
                                 std::chrono::steady_clock::now())
      -> std::expected<void, OpsBrokerFailure>;
  // Pure owner-thread final publication gate: exact invocation/request and live
  // authority, no IO. Mismatch does not consume another invocation's receipt.
  // The caller records returned evidence atomically; this API does not persist.
  [[nodiscard]] auto take_for_publication(
      const OpsObservationReceipt& receipt,
      const domain::InvocationId& expected_invocation,
      const domain::OpsObservationRequest& expected_request,
      std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now())
      -> std::expected<domain::OpsObservation, OpsBrokerFailure>;
  [[nodiscard]] auto cancel_invocation(const domain::InvocationId& invocation)
      -> std::expected<void, OpsBrokerFailure>;
  [[nodiscard]] auto deactivate_session()
      -> std::expected<void, OpsBrokerFailure>;
  [[nodiscard]] auto close() -> std::expected<void, OpsBrokerFailure>;
  [[nodiscard]] auto pending_work() const
      -> std::expected<bool, OpsBrokerFailure>;

 private:
  struct Impl;
  explicit OpsObservationBroker(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

// Close the session endpoint before destroying/joining its kernel. Closing
// wakes executor waiters and discards physical delivery without joining source
// IO. A source's last shared owner can nevertheless run its destructor here;
// caller-owned source graphs remain the caller's cleanup responsibility. This
// API does not promise that arbitrary source destructors are nonblocking.

} // namespace aiforge::runtime
