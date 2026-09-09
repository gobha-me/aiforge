#pragma once

#include <cstddef>
#include <expected>
#include <memory>

#include <aiforge/runtime/local_context_controller.hpp>
#include <aiforge/runtime/local_source_grant.hpp>

namespace aiforge::runtime {

namespace local_grants_detail {
struct State;
struct Record;
} // namespace local_grants_detail

// Opaque reservation and one-shot producer endpoint. Keep this ticket until
// the worker completion is accepted or discarded. Destruction closes dispatch
// immediately; an already-running producer remains charged until it settles.
class LocalSourceGrantReservation final {
 public:
  ~LocalSourceGrantReservation();
  LocalSourceGrantReservation(const LocalSourceGrantReservation&) = delete;
  auto operator=(const LocalSourceGrantReservation&)
      -> LocalSourceGrantReservation& = delete;
  [[nodiscard]] auto factory() const noexcept
      -> std::shared_ptr<LocalSourceGrantFactory>;
  // Only accepts the exact result staged by factory(). Does not adopt arbitrary
  // owning leases. The result already carries a borrow of the reserved owner.
  [[nodiscard]] auto accept(const LocalFolderGrantResult& result)
      -> std::expected<LocalContextGrant, domain::LocalSourceError>;

 private:
  friend class LocalSourceGrants;
  LocalSourceGrantReservation(
      std::shared_ptr<local_grants_detail::State> state,
      std::shared_ptr<local_grants_detail::Record> record,
      std::shared_ptr<LocalSourceGrantFactory> producer);
  std::shared_ptr<local_grants_detail::State> m_state;
  std::shared_ptr<local_grants_detail::Record> m_record;
  std::shared_ptr<LocalSourceGrantFactory> m_producer;
  bool m_accepted{};
};

// Keep one application instance across session switches. Reserved, active,
// retired and currently-destroying physical leases share the same hard bound.
// All lookup/retirement operations are thread-safe and never open folders.
class LocalSourceGrants final : public LocalContextGrantResolver {
 public:
  static constexpr std::size_t maximum_capacity{16};
  [[nodiscard]] static auto create(std::size_t capacity = maximum_capacity)
      -> std::expected<std::shared_ptr<LocalSourceGrants>,
                       domain::LocalSourceError>;
  ~LocalSourceGrants() override;
  LocalSourceGrants(const LocalSourceGrants&) = delete;
  auto operator=(const LocalSourceGrants&) -> LocalSourceGrants& = delete;
  // Epochs, request IDs and lease generations strictly increase
  // application-wide. Switching the session invalidates all previous grants and
  // reservations.
  [[nodiscard]] auto activate_session(const domain::SessionId& session,
                                      std::uint64_t epoch)
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto reserve(
      const LocalFolderGrantRequest& request,
      const std::shared_ptr<LocalSourceGrantFactory>& factory)
      -> std::expected<std::unique_ptr<LocalSourceGrantReservation>,
                       domain::LocalSourceError>;
  [[nodiscard]] auto resolve(const domain::SessionId& session,
                             const domain::LocalRootIdentity& root,
                             std::stop_token stop = {})
      -> std::expected<std::optional<LocalContextGrant>,
                       domain::LocalSourceError> override;
  [[nodiscard]] auto revoke(const domain::SessionId& session,
                            const domain::LocalRootIdentity& root,
                            std::uint64_t lease_generation)
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto invalidate_session(const domain::SessionId& session)
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto occupied_slots() const -> std::size_t;

 private:
  explicit LocalSourceGrants(std::shared_ptr<local_grants_detail::State> state);
  std::shared_ptr<local_grants_detail::State> m_state;
};

// One owning reclaimer thread outlives teardown without an owner-thread join.
// Borrow aliases preserve the underlying reader identity; releasing one only
// notifies its retained slot. The reclaimer waits for all tracked borrows AND
// existing external copies of the physical lease before destroying it
// off-thread. The factory guarantee forbids creating/reacquiring raw physical
// owners after handoff, including weak resurrection; only registry aliases may
// create later borrows.
// Stalled destruction keeps capacity occupied. No OS-call interruption or
// automatic regrant/retry is implied. Caller-owned factories remain the
// caller's lifetime responsibility; physical leases returned through factory()
// are owned by this registry before any result can reach the UI.
} // namespace aiforge::runtime
