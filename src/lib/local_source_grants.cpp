#include <aiforge/runtime/local_source_grants.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace aiforge::runtime {
namespace local_grants_detail {
struct Record {
  LocalFolderGrantRequest request;
  std::shared_ptr<LocalSourceGrantFactory> factory;
  std::shared_ptr<LocalSourceLease> physical;
  std::optional<domain::LocalRootIdentity> root;
  std::size_t borrowers{};
  bool attempted{};
  bool running{};
  bool active{};
  bool retired{};
  bool destroying{};
  Record(LocalFolderGrantRequest value,
         std::shared_ptr<LocalSourceGrantFactory> producer)
      : request(std::move(value)), factory(std::move(producer)) {}
};
struct Scope {
  domain::SessionId session;
  std::uint64_t epoch;
};
struct State {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::shared_ptr<Record>, LocalSourceGrants::maximum_capacity>
      slots;
  std::size_t capacity;
  std::optional<Scope> scope;
  std::uint64_t last_epoch{};
  std::uint64_t last_request{};
  std::uint64_t last_generation{};
  bool closed{};
  explicit State(std::size_t limit) : capacity(limit) {}
};
} // namespace local_grants_detail
namespace {
using namespace local_grants_detail;
using Code = domain::LocalSourceErrorCode;
auto failure(Code code, std::string message)
    -> std::unexpected<domain::LocalSourceError> {
  return std::unexpected(domain::LocalSourceError{code, std::move(message)});
}
auto internal_failure() -> std::unexpected<domain::LocalSourceError> {
  return failure(Code::internal_failure,
                 "local grant registry failed internally");
}
auto current(const State& state, const LocalFolderGrantToken& token) -> bool {
  return !state.closed && state.scope &&
         state.scope->session == token.session_id &&
         state.scope->epoch == token.session_epoch;
}
// LocalSourceLease::revoke is a mark-only, nonblocking port contract.
auto retire(Record& record) noexcept -> void {
  record.retired = true;
  record.active = false;
  if (record.physical) record.physical->revoke();
}
auto discard(const std::shared_ptr<State>& state,
             const std::shared_ptr<Record>& record) noexcept -> void {
  {
    const std::lock_guard lock{state->mutex};
    retire(*record);
  }
  state->changed.notify_all();
}
struct Borrow {
  std::shared_ptr<State> state;
  std::shared_ptr<Record> record;
  bool counted{};
  Borrow(std::shared_ptr<State> owner, std::shared_ptr<Record> value)
      : state(std::move(owner)), record(std::move(value)) {}
  ~Borrow() {
    if (!counted) return;
    {
      const std::lock_guard lock{state->mutex};
      --record->borrowers;
    }
    state->changed.notify_all();
  }
};
// Called under the state mutex. Allocation precedes counting; any failure
// destroys only an uncounted borrow. Aliasing construction itself is noexcept.
template <typename Port>
auto borrow(const std::shared_ptr<State>& state,
            const std::shared_ptr<Record>& record) -> std::shared_ptr<Port> {
  auto owner = std::make_shared<Borrow>(state, record);
  std::shared_ptr<Port> result{owner, record->physical.get()};
  ++record->borrowers;
  owner->counted = true;
  return result;
}
auto resolved(const std::shared_ptr<State>& state,
              const std::shared_ptr<Record>& record,
              const domain::LocalRootIdentity& root) -> LocalContextGrant {
  // Metadata copies precede the counted alias; failure cannot release a counted
  // borrow while the caller holds the registry mutex.
  LocalContextGrant result{record->request.token.session_id,
                           root,
                           record->request.lease_generation,
                           {}};
  result.reader = borrow<LocalSourceReader>(state, record);
  return result;
}

auto reclaim_one(const std::shared_ptr<State>& state,
                 std::unique_lock<std::mutex>& lock) -> bool {
  for (auto& slot : state->slots) {
    if (!slot || !slot->retired || slot->running || slot->destroying) continue;
    auto record = slot;
    // Release producer dependencies off-thread first: a factory may itself
    // retain a copy of its last result. Physical lease ownership remains here.
    if (record->factory) {
      auto factory = std::move(record->factory);
      record->destroying = true;
      lock.unlock();
      factory.reset();
      lock.lock();
      record->destroying = false;
      return true;
    }
    if (record->borrowers != 0 ||
        (record->physical && record->physical.use_count() != 1))
      continue;
    record->destroying = true;
    auto physical = std::move(record->physical);
    lock.unlock();
    physical.reset();
    lock.lock();
    // Capacity remains occupied through completion of the actual destructor.
    slot.reset();
    state->changed.notify_all();
    return true;
  }
  return false;
}
auto reclaim(const std::shared_ptr<State>& state) noexcept -> void {
  for (;;) {
    try {
      std::unique_lock lock{state->mutex};
      if (state->closed &&
          std::ranges::none_of(state->slots,
                               [](const auto& slot) { return bool(slot); }))
        return;
      if (reclaim_one(state, lock)) continue;
      // Raw shared-owner release has no notification hook. A single bounded
      // reclaimer polls that case; all tracked releases also notify directly.
      state->changed.wait_for(lock, std::chrono::milliseconds{20});
    } catch (...) {
      // Never abandon ownership to a UI borrow after a synchronization error.
      std::this_thread::yield();
    }
  }
}

auto stage_result(const std::shared_ptr<State>& state,
                  const std::shared_ptr<Record>& record,
                  LocalFolderGrantResult result)
    -> std::expected<LocalFolderGrantResult, domain::LocalSourceError> {
  const std::lock_guard lock{state->mutex};
  // The reserved cleanup owner receives the physical lease before validation,
  // metadata copies, alias allocation, or any other fallible publication work.
  record->physical = std::move(result.lease);
  const auto& physical = record->physical;
  const bool valid =
      result.token == record->request.token && physical &&
      domain::validate_local_root_identity(result.root) &&
      physical->guarantees_pinned_read_only_sources() &&
      physical->root_identity() == result.root &&
      physical->session_id() == record->request.token.session_id &&
      physical->lease_generation() == record->request.lease_generation;
  if (!valid || record->retired || !current(*state, result.token)) {
    retire(*record);
    return failure(valid ? Code::stale_lease : Code::invalid_result,
                   "local folder grant result is stale or invalid");
  }
  record->root = result.root;
  result.lease = borrow<LocalSourceLease>(state, record);
  return result;
}

class Producer final : public LocalSourceGrantFactory {
 public:
  Producer(std::shared_ptr<State> state, std::shared_ptr<Record> record)
      : m_state(std::move(state)), m_record(std::move(record)) {}
  [[nodiscard]] auto guarantees_pinned_read_only_sources() const noexcept
      -> bool override {
    return true;
  }
  auto grant(const LocalFolderGrantRequest& request, std::stop_token stop)
      -> std::expected<LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    std::shared_ptr<LocalSourceGrantFactory> factory;
    bool dispatched{};
    try {
      {
        const std::lock_guard lock{m_state->mutex};
        if (m_record->attempted || m_record->retired ||
            !current(*m_state, request.token) ||
            request.token != m_record->request.token ||
            request.absolute_path != m_record->request.absolute_path ||
            request.lease_generation != m_record->request.lease_generation ||
            request.limits != m_record->request.limits)
          return failure(
              Code::stale_lease,
              "local folder reservation cannot dispatch this request");
        m_record->attempted = true;
        m_record->running = true;
        dispatched = true;
        factory = m_record->factory;
      }
      auto result = invoke(factory, request, stop);
      finish(!result);
      return result;
    } catch (...) {
      if (dispatched) finish(true);
      return internal_failure();
    }
  }

 private:
  auto invoke(const std::shared_ptr<LocalSourceGrantFactory>& factory,
              const LocalFolderGrantRequest& request, std::stop_token stop)
      -> std::expected<LocalFolderGrantResult, domain::LocalSourceError> {
    if (stop.stop_requested())
      return failure(Code::cancelled, "local folder grant cancelled");
    auto result = factory->grant(request, stop);
    if (!result)
      return failure(Code::unavailable,
                     "local folder grant could not be created");
    // Cancellation never leaves an owning completion on the calling/UI side.
    if (stop.stop_requested()) discard(m_state, m_record);
    return stage_result(m_state, m_record, std::move(*result));
  }
  auto finish(bool failed) noexcept -> void {
    {
      const std::lock_guard lock{m_state->mutex};
      if (failed) retire(*m_record);
      m_record->running = false;
    }
    m_state->changed.notify_all();
  }
  std::shared_ptr<State> m_state;
  std::shared_ptr<Record> m_record;
};

auto duplicate_root(const State& state, const Record& record) -> bool {
  return std::ranges::any_of(state.slots, [&](const auto& slot) {
    return slot && slot.get() != &record && slot->active && !slot->retired &&
           slot->request.token.session_id == record.request.token.session_id &&
           slot->root == record.root;
  });
}
} // namespace

LocalSourceGrantReservation::LocalSourceGrantReservation(
    std::shared_ptr<State> state, std::shared_ptr<Record> record,
    std::shared_ptr<LocalSourceGrantFactory> producer)
    : m_state(std::move(state)), m_record(std::move(record)),
      m_producer(std::move(producer)) {
}
LocalSourceGrantReservation::~LocalSourceGrantReservation() {
  if (!m_accepted) discard(m_state, m_record);
}
auto LocalSourceGrantReservation::factory() const noexcept
    -> std::shared_ptr<LocalSourceGrantFactory> {
  return m_producer;
}
auto LocalSourceGrantReservation::accept(const LocalFolderGrantResult& result)
    -> std::expected<LocalContextGrant, domain::LocalSourceError> {
  try {
    const std::lock_guard lock{m_state->mutex};
    if (m_accepted || m_record->retired || m_record->running ||
        !m_record->attempted || !m_record->physical || !m_record->root ||
        !current(*m_state, result.token) ||
        result.token != m_record->request.token ||
        result.root != m_record->root ||
        result.lease.get() != m_record->physical.get() ||
        duplicate_root(*m_state, *m_record)) {
      if (!m_accepted) retire(*m_record);
      m_state->changed.notify_all();
      return failure(Code::stale_lease,
                     "local folder grant cannot be accepted");
    }
    auto grant = resolved(m_state, m_record, result.root);
    m_record->active = true;
    m_accepted = true;
    return grant;
  } catch (...) {
    discard(m_state, m_record);
    return internal_failure();
  }
}

LocalSourceGrants::LocalSourceGrants(std::shared_ptr<State> state)
    : m_state(std::move(state)) {
}
auto LocalSourceGrants::create(std::size_t capacity)
    -> std::expected<std::shared_ptr<LocalSourceGrants>,
                     domain::LocalSourceError> {
  if (capacity == 0 || capacity > maximum_capacity)
    return failure(Code::invalid_request,
                   "local grant registry capacity is invalid");
  try {
    auto state = std::make_shared<State>(capacity);
    auto owner =
        std::shared_ptr<LocalSourceGrants>{new LocalSourceGrants{state}};
    std::thread worker{[state] { reclaim(state); }};
    try {
      worker.detach();
    } catch (...) {
      {
        const std::lock_guard lock{state->mutex};
        state->closed = true;
      }
      state->changed.notify_all();
      worker.join(); // Creation failed before any lease/reservation existed.
      throw;
    }
    return owner;
  } catch (...) {
    return internal_failure();
  }
}
LocalSourceGrants::~LocalSourceGrants() {
  {
    const std::lock_guard lock{m_state->mutex};
    m_state->closed = true;
    m_state->scope.reset();
    for (const auto& slot : m_state->slots)
      if (slot) retire(*slot);
  }
  m_state->changed.notify_all();
}
auto LocalSourceGrants::activate_session(const domain::SessionId& session,
                                         std::uint64_t epoch)
    -> std::expected<void, domain::LocalSourceError> {
  try {
    Scope next{session, epoch};
    const std::lock_guard lock{m_state->mutex};
    if (m_state->closed || epoch == 0 || epoch <= m_state->last_epoch)
      return failure(Code::stale_lease, "local session epoch is stale");
    for (const auto& slot : m_state->slots)
      if (slot) retire(*slot);
    m_state->scope = std::move(next);
    m_state->last_epoch = epoch;
    m_state->changed.notify_all();
    return {};
  } catch (...) {
    return internal_failure();
  }
}
auto LocalSourceGrants::reserve(
    const LocalFolderGrantRequest& request,
    const std::shared_ptr<LocalSourceGrantFactory>& factory)
    -> std::expected<std::unique_ptr<LocalSourceGrantReservation>,
                     domain::LocalSourceError> {
  try {
    if (!validate_local_folder_grant_request(request) || !factory ||
        !factory->guarantees_pinned_read_only_sources())
      return failure(Code::invalid_request,
                     "local folder reservation is invalid");
    const std::lock_guard lock{m_state->mutex};
    if (!current(*m_state, request.token) ||
        request.token.request_id <= m_state->last_request ||
        request.lease_generation <= m_state->last_generation)
      return failure(Code::stale_lease,
                     "local folder reservation identity is stale");
    const auto end =
        m_state->slots.begin() + static_cast<std::ptrdiff_t>(m_state->capacity);
    const auto free = std::find(m_state->slots.begin(), end, nullptr);
    if (free == end)
      return failure(Code::resource_exhausted,
                     "local grant capacity remains occupied");
    auto record = std::make_shared<Record>(request, factory);
    auto producer = std::make_shared<Producer>(m_state, record);
    auto ticket = std::unique_ptr<LocalSourceGrantReservation>{
        new LocalSourceGrantReservation{m_state, record, producer}};
    *free = record;
    m_state->last_request = request.token.request_id;
    m_state->last_generation = request.lease_generation;
    return ticket;
  } catch (...) {
    return internal_failure();
  }
}
auto LocalSourceGrants::resolve(const domain::SessionId& session,
                                const domain::LocalRootIdentity& root,
                                std::stop_token stop)
    -> std::expected<std::optional<LocalContextGrant>,
                     domain::LocalSourceError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "local grant lookup cancelled");
    if (!domain::validate_local_root_identity(root))
      return failure(Code::invalid_request, "local root identity is invalid");
    const std::lock_guard lock{m_state->mutex};
    for (const auto& slot : m_state->slots)
      if (slot && slot->active && !slot->retired &&
          current(*m_state, slot->request.token) &&
          slot->request.token.session_id == session && slot->root == root)
        return resolved(m_state, slot, root);
    return std::nullopt;
  } catch (...) {
    return internal_failure();
  }
}
auto LocalSourceGrants::revoke(const domain::SessionId& session,
                               const domain::LocalRootIdentity& root,
                               std::uint64_t lease_generation)
    -> std::expected<void, domain::LocalSourceError> {
  try {
    if (!domain::validate_local_root_identity(root) || lease_generation == 0)
      return failure(Code::invalid_request,
                     "local grant revocation is invalid");
    const std::lock_guard lock{m_state->mutex};
    for (const auto& slot : m_state->slots) {
      if (!slot || !slot->active || slot->retired ||
          slot->request.token.session_id != session || slot->root != root ||
          slot->request.lease_generation != lease_generation)
        continue;
      retire(*slot);
      m_state->changed.notify_all();
      return {};
    }
    return failure(Code::stale_lease, "local grant is no longer active");
  } catch (...) {
    return internal_failure();
  }
}
auto LocalSourceGrants::invalidate_session(const domain::SessionId& session)
    -> std::expected<void, domain::LocalSourceError> {
  try {
    const std::lock_guard lock{m_state->mutex};
    for (const auto& slot : m_state->slots)
      if (slot && slot->request.token.session_id == session) retire(*slot);
    if (m_state->scope && m_state->scope->session == session)
      m_state->scope.reset();
    m_state->changed.notify_all();
    return {};
  } catch (...) {
    return internal_failure();
  }
}
auto LocalSourceGrants::occupied_slots() const -> std::size_t {
  const std::lock_guard lock{m_state->mutex};
  return static_cast<std::size_t>(std::ranges::count_if(
      m_state->slots, [](const auto& slot) { return bool(slot); }));
}
} // namespace aiforge::runtime
