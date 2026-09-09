#include <aiforge/runtime/ops_observation_broker.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/local_source_worker.hpp>

namespace aiforge::runtime {
namespace {
using Clock = std::chrono::steady_clock;
using Code = OpsBrokerError;
auto failure(Code code) -> std::unexpected<OpsBrokerFailure> {
  return std::unexpected(OpsBrokerFailure{code});
}
auto valid_identity(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 128 &&
         detail::is_safe_utf8_text(value) &&
         std::ranges::none_of(value, [](unsigned char byte) {
           return byte < 32 || byte == 127;
         });
}
struct Entry {
  std::uint64_t ticket;
  domain::InvocationId invocation;
  domain::OpsObservationRequest request;
  Clock::time_point deadline;
  std::stop_token stop;
  bool claimed{};
  std::optional<OpsObservationWorkToken> token{};
  std::optional<domain::OpsObservation> result{};
  std::optional<OpsBrokerFailure> error{};
};
auto worker_failure(const LocalSourceWorkerError& error) -> OpsBrokerFailure {
  switch (error.code) {
    case LocalSourceWorkerErrorCode::busy: return {Code::busy};
    case LocalSourceWorkerErrorCode::stale_request:
      return {Code::stale_request};
    case LocalSourceWorkerErrorCode::resource_exhausted:
      return {Code::resource_exhausted};
    case LocalSourceWorkerErrorCode::invalid_request:
      return {Code::invalid_request};
    case LocalSourceWorkerErrorCode::internal_failure:
      return {Code::internal_failure};
  }
  return {Code::internal_failure};
}
} // namespace

struct OpsObservationMailbox {
  explicit OpsObservationMailbox(domain::SessionId id, std::size_t capacity)
      : session(std::move(id)), capacity(capacity) {}
  std::mutex mutex;
  std::condition_variable_any changed;
  domain::SessionId session;
  std::size_t capacity;
  std::uint64_t last_ticket{};
  bool closed{};
  std::atomic<bool> failed{};
  std::vector<std::shared_ptr<Entry>> entries;
};

namespace {
// This latch does not depend on successful mutex cleanup. An exceptional
// mailbox cannot admit or publish again; owner service retires retained jobs.
auto fail_closed(const std::shared_ptr<OpsObservationMailbox>& state) noexcept
    -> void {
  if (!state) return;
  state->failed.store(true);
  state->changed.notify_all();
}
} // namespace

OpsObservationReceipt::OpsObservationReceipt(
    std::weak_ptr<const OpsObservationMailbox> issuer, std::uint64_t ticket)
    : m_issuer(std::move(issuer)), m_ticket(ticket) {
}
OpsObservationEndpoint::OpsObservationEndpoint(
    std::shared_ptr<OpsObservationMailbox> state)
    : m_state(std::move(state)) {
}

auto OpsObservationEndpoint::observe(
    const domain::InvocationId& invocation,
    const domain::OpsObservationRequest& request, Clock::time_point deadline,
    std::stop_token stop)
    -> std::expected<OpsObservationReceipt, OpsBrokerFailure> {
  std::shared_ptr<Entry> entry;
  try {
    if (!valid_identity(invocation.value()) ||
        !domain::validate_recorded_ops_request(request))
      return failure(Code::invalid_request);
    if (stop.stop_requested()) return failure(Code::cancelled);
    const auto now = Clock::now();
    if (deadline <= now) return failure(Code::timed_out);
    const auto bounded = now + request.limits.timeout;
    deadline = std::min(deadline, bounded);
    std::unique_lock lock(m_state->mutex);
    if (m_state->failed.load()) return failure(Code::internal_failure);
    if (m_state->closed) return failure(Code::closed);
    if (request.session_id != m_state->session)
      return failure(Code::invalid_request);
    if (m_state->entries.size() >= m_state->capacity)
      return failure(Code::busy);
    if (std::ranges::any_of(m_state->entries, [&](const auto& entry) {
          return entry->invocation == invocation ||
                 entry->request.request_id == request.request_id;
        }))
      return failure(Code::stale_request);
    if (m_state->last_ticket == std::numeric_limits<std::uint64_t>::max())
      return failure(Code::resource_exhausted);
    const auto ticket = ++m_state->last_ticket;
    entry =
        std::make_shared<Entry>(ticket, invocation, request, deadline, stop);
    m_state->entries.push_back(entry);
    // condition_variable_any owns a notification-only stop callback. Neither
    // cancellation nor waiting calls the owner or physical source.
    static_cast<void>(m_state->changed.wait_until(lock, stop, deadline, [&] {
      return m_state->failed.load() || m_state->closed || entry->error ||
             entry->result;
    }));
    if (m_state->failed.load()) return failure(Code::internal_failure);
    if (m_state->closed) return failure(Code::closed);
    if (stop.stop_requested())
      entry->error = OpsBrokerFailure{Code::cancelled};
    else if (Clock::now() >= deadline)
      entry->error = OpsBrokerFailure{Code::timed_out};
    if (entry->error) return std::unexpected(*entry->error);
    if (!entry->result) return failure(Code::internal_failure);
    return OpsObservationReceipt{m_state, ticket};
  } catch (...) {
    fail_closed(m_state);
    return failure(Code::internal_failure);
  }
}

struct OpsObservationBroker::Impl {
  std::shared_ptr<LocalSourceWorker> worker;
  OpsBrokerLimits limits;
  std::shared_ptr<OpsObservationMailbox> state;
  std::optional<domain::OpsObservationAuthority> authority;
  std::shared_ptr<OpsObservationSource> source;
  std::uint64_t epoch{};
  bool closed{};

  auto discard(const std::shared_ptr<Entry>& entry, OpsBrokerFailure error)
      -> void {
    {
      const std::lock_guard lock(state->mutex);
      entry->error = error;
      entry->result.reset();
      std::erase(state->entries, entry);
    }
    state->changed.notify_all();
    if (entry->token) {
      [[maybe_unused]] const auto cancelled = worker->cancel(*entry->token);
    }
  }
  auto discard_all(Code code, bool close_state) -> void {
    if (!state) return;
    std::vector<std::shared_ptr<Entry>> entries;
    {
      const std::lock_guard lock(state->mutex);
      state->closed = close_state;
      entries.swap(state->entries);
      for (const auto& entry : entries) {
        entry->error = OpsBrokerFailure{code};
        entry->result.reset();
      }
    }
    state->changed.notify_all();
    for (const auto& entry : entries)
      if (entry->token) {
        [[maybe_unused]] const auto cancelled = worker->cancel(*entry->token);
      }
  }
  [[nodiscard]] auto current(const domain::OpsObservationRequest& request) const
      -> bool {
    return !closed && state && !state->failed.load() && !state->closed &&
           authority && authority->validate(request).has_value();
  }
  [[nodiscard]] auto submit_entry(const std::shared_ptr<Entry>& entry) -> bool {
    if (!authority) {
      discard(entry, {Code::stale_request});
      return false;
    }
    auto identity = worker->allocate_request_id();
    if (!identity) {
      discard(entry, worker_failure(identity.error()));
      return false;
    }
    entry->token.emplace(OpsObservationWorkToken{
        entry->request.session_id, epoch, *identity, entry->request});
    auto submitted = worker->submit(source, {*entry->token, *authority});
    if (!submitted) {
      discard(entry, worker_failure(submitted.error()));
      return false;
    }
    return true;
  }
  auto service_entry(const std::shared_ptr<Entry>& entry, Clock::time_point now)
      -> void {
    std::optional<OpsBrokerFailure> rejected;
    bool dispatch{};
    {
      const std::lock_guard lock(state->mutex);
      rejected = entry->error;
      if (entry->stop.stop_requested())
        rejected = OpsBrokerFailure{Code::cancelled};
      else if (now >= entry->deadline)
        rejected = OpsBrokerFailure{Code::timed_out};
      else if (!current(entry->request))
        rejected = OpsBrokerFailure{Code::stale_request};
      if (!rejected && !entry->claimed) {
        entry->claimed = true;
        dispatch = true;
      }
    }
    if (rejected) {
      discard(entry, *rejected);
      return;
    }
    if (dispatch && !submit_entry(entry)) return;
    if (!entry->token) return;
    auto completed = worker->poll(*entry->token);
    if (!completed) {
      discard(entry, worker_failure(completed.error()));
      return;
    }
    if (!*completed) return;
    entry->token.reset();
    const auto& result = (**completed).result;
    if (!result) {
      discard(entry, {Code::source_failure, result.error()});
      return;
    }
    if (!authority || !current(entry->request) ||
        !domain::validate_ops_observation(*authority, entry->request,
                                          *result)) {
      discard(entry, {Code::stale_request});
      return;
    }
    {
      const std::lock_guard lock(state->mutex);
      if (!entry->error)
        entry->result = std::move((**completed).result.value());
    }
    state->changed.notify_all();
  }
};

OpsObservationBroker::OpsObservationBroker(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}
OpsObservationBroker::~OpsObservationBroker() {
  [[maybe_unused]] const auto closed = close();
}
auto OpsObservationBroker::create(std::shared_ptr<LocalSourceWorker> worker,
                                  OpsBrokerLimits limits)
    -> std::expected<std::unique_ptr<OpsObservationBroker>, OpsBrokerFailure> {
  try {
    if (!worker || limits.maximum_pending == 0 ||
        limits.maximum_pending > maximum_pending)
      return failure(Code::invalid_request);
    auto impl = std::make_unique<Impl>();
    impl->worker = std::move(worker);
    impl->limits = limits;
    return std::unique_ptr<OpsObservationBroker>{
        new OpsObservationBroker{std::move(impl)}};
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto OpsObservationBroker::activate_session(domain::SessionId session)
    -> std::expected<std::shared_ptr<OpsObservationEndpoint>,
                     OpsBrokerFailure> {
  try {
    if (m_impl->closed) return failure(Code::closed);
    if (!valid_identity(session.value())) return failure(Code::invalid_request);
    if (m_impl->epoch == std::numeric_limits<std::uint64_t>::max())
      return failure(Code::resource_exhausted);
    auto state = std::make_shared<OpsObservationMailbox>(
        std::move(session), m_impl->limits.maximum_pending);
    auto endpoint = std::shared_ptr<OpsObservationEndpoint>{
        new OpsObservationEndpoint{state}};
    m_impl->discard_all(Code::closed, true);
    ++m_impl->epoch;
    m_impl->state = std::move(state);
    m_impl->authority.reset();
    m_impl->source.reset();
    return endpoint;
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto OpsObservationBroker::select(domain::OpsObservationAuthority authority,
                                  std::shared_ptr<OpsObservationSource> source)
    -> std::expected<void, OpsBrokerFailure> {
  try {
    if (m_impl->closed || !m_impl->state || m_impl->state->closed)
      return failure(Code::closed);
    if (m_impl->state->failed.load()) return failure(Code::internal_failure);
    const auto& next = authority.specification();
    if (next.session_id != m_impl->state->session || !source ||
        !source->guarantees_bound_read_only_observations() ||
        source->target_binding() != next.target)
      return failure(Code::invalid_request);
    if (m_impl->authority) {
      const auto& previous = m_impl->authority->specification();
      if (next.owner_id != previous.owner_id ||
          next.selection_generation <= previous.selection_generation)
        return failure(Code::stale_request);
      if (next.target.target_id == previous.target.target_id &&
          (next.logs.revision < previous.logs.revision ||
           (next.logs != previous.logs &&
            next.logs.revision == previous.logs.revision)))
        return failure(Code::stale_request);
    }
    m_impl->discard_all(Code::stale_request, false);
    m_impl->authority = std::move(authority);
    m_impl->source = std::move(source);
    return {};
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto OpsObservationBroker::service(Clock::time_point now)
    -> std::expected<void, OpsBrokerFailure> {
  try {
    if (m_impl->closed) return failure(Code::closed);
    if (!m_impl->state) return {};
    if (m_impl->state->failed.load()) {
      m_impl->discard_all(Code::internal_failure, false);
      return failure(Code::internal_failure);
    }
    std::vector<std::shared_ptr<Entry>> entries;
    {
      const std::lock_guard lock(m_impl->state->mutex);
      entries = m_impl->state->entries;
    }
    for (const auto& entry : entries)
      m_impl->service_entry(entry, now);
    return {};
  } catch (...) {
    fail_closed(m_impl->state);
    try {
      m_impl->discard_all(Code::internal_failure, false);
    } catch (...) {
      // Keep the failure latch set even if physical cancellation cleanup fails.
      return failure(Code::internal_failure);
    }
    return failure(Code::internal_failure);
  }
}
auto OpsObservationBroker::take_for_publication(
    const OpsObservationReceipt& receipt,
    const domain::InvocationId& expected_invocation,
    const domain::OpsObservationRequest& expected_request,
    Clock::time_point now)
    -> std::expected<domain::OpsObservation, OpsBrokerFailure> {
  try {
    if (m_impl->closed || !m_impl->state) return failure(Code::closed);
    if (m_impl->state->failed.load()) return failure(Code::internal_failure);
    const auto issuer = receipt.m_issuer.lock();
    if (!issuer || issuer != m_impl->state) return failure(Code::stale_request);
    const std::lock_guard lock(m_impl->state->mutex);
    const auto found =
        std::ranges::find_if(m_impl->state->entries, [&](const auto& entry) {
          return entry->ticket == receipt.m_ticket;
        });
    if (found == m_impl->state->entries.end())
      return failure(Code::stale_request);
    const auto& entry = *found;
    if (entry->invocation != expected_invocation ||
        entry->request != expected_request)
      return failure(Code::invalid_request);
    std::optional<OpsBrokerFailure> rejected = entry->error;
    if (entry->stop.stop_requested())
      rejected = OpsBrokerFailure{Code::cancelled};
    else if (now >= entry->deadline)
      rejected = OpsBrokerFailure{Code::timed_out};
    else if (!m_impl->current(expected_request))
      rejected = OpsBrokerFailure{Code::stale_request};
    if (rejected) {
      m_impl->state->entries.erase(found);
      return std::unexpected(*rejected);
    }
    if (!entry->result) return failure(Code::stale_request);
    auto result = std::move(*entry->result);
    m_impl->state->entries.erase(found);
    return result;
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto OpsObservationBroker::cancel_invocation(
    const domain::InvocationId& invocation)
    -> std::expected<void, OpsBrokerFailure> {
  try {
    if (m_impl->closed) return failure(Code::closed);
    if (!m_impl->state) return failure(Code::stale_request);
    std::shared_ptr<Entry> entry;
    {
      const std::lock_guard lock(m_impl->state->mutex);
      const auto found = std::ranges::find_if(
          m_impl->state->entries, [&](const auto& candidate) {
            return candidate->invocation == invocation;
          });
      if (found == m_impl->state->entries.end())
        return failure(Code::stale_request);
      entry = *found;
    }
    m_impl->discard(entry, {Code::cancelled});
    return {};
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto OpsObservationBroker::deactivate_session()
    -> std::expected<void, OpsBrokerFailure> {
  try {
    m_impl->discard_all(Code::closed, true);
    m_impl->authority.reset();
    m_impl->source.reset();
    return {};
  } catch (...) {
    fail_closed(m_impl->state);
    return failure(Code::internal_failure);
  }
}
auto OpsObservationBroker::close() -> std::expected<void, OpsBrokerFailure> {
  m_impl->closed = true;
  return deactivate_session();
}
auto OpsObservationBroker::pending_work() const
    -> std::expected<bool, OpsBrokerFailure> {
  try {
    if (!m_impl->state) return false;
    if (m_impl->state->failed.load()) return failure(Code::internal_failure);
    const std::lock_guard lock(m_impl->state->mutex);
    return !m_impl->state->entries.empty();
  } catch (...) {
    fail_closed(m_impl->state);
    return failure(Code::internal_failure);
  }
}
} // namespace aiforge::runtime
