#include "../159foldergrant/fixture.hpp"
#include <aiforge/runtime/local_source_worker.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
thread_local bool fail_owner_allocations{};
struct AllocationFailure {
  AllocationFailure() { fail_owner_allocations = true; }
  ~AllocationFailure() { fail_owner_allocations = false; }
};
} // namespace
// Only the calling test thread fails allocation. Producer/relay cleanup remains
// ordinary; this proves public errors do not allocate while handling bad_alloc.
void* operator new(std::size_t size) {
  if (fail_owner_allocations) throw std::bad_alloc{};
  if (void* value = std::malloc(size == 0 ? 1 : size)) return value;
  throw std::bad_alloc{};
}
void operator delete(void* value) noexcept {
  std::free(value);
}
void operator delete(void* value, std::size_t) noexcept {
  std::free(value);
}

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
using folder_grant_test::Gate;
using folder_grant_test::Release;
using folder_grant_test::until;
using Error = runtime::OpsObservationSourceError;
using Code = runtime::LocalSourceWorkerErrorCode;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto identity() -> runtime::OpsSourcePreparationIdentity {
  return {id<domain::OpsTargetId>("target"),
          id<domain::OpsConfigurationRevision>("revision"),
          domain::OpsTargetKind::linux_local};
}
auto request(std::uint64_t sequence = 1)
    -> runtime::OpsSourcePreparationRequest {
  return {{id<domain::SessionId>("session"), 1, sequence, identity()},
          std::chrono::steady_clock::now() + 5s};
}
auto binding() -> domain::OpsTargetBinding {
  const auto selected = identity();
  return {selected.target_id, selected.configuration_revision,
          domain::LinuxOpsIdentity{domain::LinuxExecutionScope::container,
                                   "12345678-1234-1234-1234-123456789abc", 42,
                                   43}};
}
struct State {
  std::atomic<unsigned> calls{}, observations{}, destroyed{}, callbacks{},
      factory_destroyed{};
  std::shared_ptr<Gate> block, destroy_block, callback, factory_destroy_block;
  std::atomic<std::thread::id> producer, destructor;
  std::optional<Error> failure;
  enum class Mode {
    valid,
    throws,
    null_source,
    false_guarantee,
    foreign_target,
    foreign_revision,
    wrong_kind,
    invalid_binding
  } mode{Mode::valid};
};
class Source final : public runtime::OpsObservationSource {
 public:
  explicit Source(std::shared_ptr<State> state) : m_state(std::move(state)) {
    switch (m_state->mode) {
      case State::Mode::foreign_target:
        target.target_id = id<domain::OpsTargetId>("foreign");
        break;
      case State::Mode::foreign_revision:
        target.configuration_revision =
            id<domain::OpsConfigurationRevision>("foreign");
        break;
      case State::Mode::wrong_kind:
        target.identity = domain::CephOpsIdentity{"cluster"};
        break;
      case State::Mode::invalid_binding:
        std::get<domain::LinuxOpsIdentity>(target.identity).boot_id = "bad";
        break;
      default: break;
    }
  }
  ~Source() override {
    m_state->destructor = std::this_thread::get_id();
    if (m_state->destroy_block) m_state->destroy_block->wait();
    ++m_state->destroyed;
  }
  domain::OpsTargetBinding target{binding()};
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return m_state->mode != State::Mode::false_guarantee;
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return target;
  }
  auto observe(const domain::OpsObservationRequest&, std::stop_token)
      -> std::expected<domain::OpsObservation, Error> override {
    ++m_state->observations;
    return std::unexpected(Error::unavailable);
  }

 private:
  std::shared_ptr<State> m_state;
};
class Factory final : public runtime::OpsSourcePreparationFactory {
 public:
  std::shared_ptr<State> state = std::make_shared<State>();
  runtime::OpsSourcePreparationIdentity selected{identity()};
  bool guaranteed{true};
  ~Factory() override {
    if (state->factory_destroy_block) state->factory_destroy_block->wait();
    ++state->factory_destroyed;
  }

  auto guarantees_owned_read_only_preparation() const noexcept
      -> bool override {
    return guaranteed;
  }
  auto preparation_identity() const noexcept
      -> const runtime::OpsSourcePreparationIdentity& override {
    return selected;
  }
  auto prepare(const runtime::OpsSourcePreparationRequest&,
               std::stop_token stop)
      -> std::expected<runtime::PreparedOpsSource, Error> override {
    state->producer = std::this_thread::get_id();
    ++state->calls;
    std::optional<std::stop_callback<std::function<void()>>> callback;
    if (state->callback)
      callback.emplace(stop, [state = state] {
        ++state->callbacks;
        state->callback->wait();
      });
    if (state->block) state->block->wait();
    if (state->mode == State::Mode::throws)
      throw std::runtime_error("credential-sentinel /private/config");
    if (state->failure) return std::unexpected(*state->failure);
    if (state->mode == State::Mode::null_source)
      return runtime::PreparedOpsSource{};
    return runtime::PreparedOpsSource{std::make_shared<Source>(state)};
  }
};
struct Fixture {
  std::shared_ptr<Factory> factory = std::make_shared<Factory>();
  std::unique_ptr<runtime::LocalSourceWorker> worker =
      runtime::LocalSourceWorker::create(1).value();
  auto complete(const runtime::OpsSourcePreparationToken& token)
      -> runtime::OpsSourcePreparationCompletion {
    REQUIRE(until([&] { return worker->ready_results() == 1; }));
    auto result = worker->poll(token);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
  }
};
} // namespace

TEST_CASE("Preparation rejects invalid metadata before producer work",
          "[ops][preparation]") {
  Fixture f;
  auto value = request();
  SECTION("null") {
    f.factory.reset();
  }
  SECTION("guarantee") {
    f.factory->guaranteed = false;
  }
  SECTION("epoch") {
    value.token.session_epoch = 0;
  }
  SECTION("request") {
    value.token.request_id = 0;
  }
  SECTION("kind") {
    value.token.selection.kind = static_cast<domain::OpsTargetKind>(99);
  }
  SECTION("target") {
    value.token.selection.target_id = id<domain::OpsTargetId>("other");
  }
  SECTION("revision") {
    value.token.selection.configuration_revision =
        id<domain::OpsConfigurationRevision>("other");
  }
  SECTION("invalid UTF8") {
    value.token.session_id =
        id<domain::SessionId>(std::string(1, static_cast<char>(0xff)));
  }
  SECTION("expired") {
    value.deadline = std::chrono::steady_clock::time_point::min();
  }
  SECTION("too far") {
    value.deadline = std::chrono::steady_clock::time_point::max();
  }
  auto result = f.worker->submit(f.factory, value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_request);
  CHECK(f.worker->occupied_slots() == 0);
  if (f.factory) CHECK(f.factory->state->calls == 0);
}
TEST_CASE("Preparation public error boundaries survive persistent owner "
          "allocation failure",
          "[ops][preparation]") {
  Fixture f;
  auto value = request();
  value.token.session_id = id<domain::SessionId>(std::string(128, 's'));
  SECTION("submission error") {
    f.factory->guaranteed = false;
    auto failed = [&] {
      AllocationFailure active;
      return f.worker->submit(f.factory, std::move(value));
    }();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == Code::internal_failure);
    CHECK(failed.error().message.empty());
  }
  SECTION("poll token copy") {
    auto failed = [&] {
      AllocationFailure active;
      return f.worker->poll(value.token);
    }();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == Code::internal_failure);
    CHECK(failed.error().message.empty());
  }
  SECTION("cancel token copy") {
    auto failed = [&] {
      AllocationFailure active;
      return f.worker->cancel(value.token);
    }();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == Code::internal_failure);
    CHECK(failed.error().message.empty());
  }
  CHECK(f.worker->occupied_slots() == 0);
  CHECK(f.factory->state->calls == 0);
}
TEST_CASE(
    "Preparation deadline validation handles exact bounds without overflow",
    "[ops][preparation]") {
  auto value = request();
  const auto now = std::chrono::steady_clock::time_point{10s};
  value.deadline = now + 5s;
  CHECK(runtime::validate_ops_source_preparation_request(value, now));
  value.deadline += 1ns;
  CHECK_FALSE(runtime::validate_ops_source_preparation_request(value, now));
  value.deadline = now;
  auto expired = runtime::validate_ops_source_preparation_request(value, now);
  REQUIRE_FALSE(expired);
  CHECK(expired.error() == Error::timed_out);
  value.deadline = std::chrono::steady_clock::time_point::max();
  CHECK_FALSE(runtime::validate_ops_source_preparation_request(
      value, value.deadline - 1ns));
}
TEST_CASE("Preparation rejects invalid resource results on their producer",
          "[ops][preparation]") {
  Fixture f;
  const auto mode =
      GENERATE(State::Mode::throws, State::Mode::null_source,
               State::Mode::false_guarantee, State::Mode::foreign_target,
               State::Mode::foreign_revision, State::Mode::wrong_kind,
               State::Mode::invalid_binding);
  f.factory->state->mode = mode;
  const auto value = request();
  REQUIRE(f.worker->submit(f.factory, value));
  auto done = f.complete(value.token);
  REQUIRE_FALSE(done.result);
  CHECK(done.result.error() == (mode == State::Mode::throws
                                    ? Error::internal_failure
                                    : Error::invalid_result));
  CHECK(f.factory->state->observations == 0);
  if (mode != State::Mode::throws && mode != State::Mode::null_source) {
    CHECK(f.factory->state->destroyed == 1);
    CHECK(f.factory->state->destructor.load() ==
          f.factory->state->producer.load());
    CHECK(f.factory->state->destructor.load() != std::this_thread::get_id());
  }
}
TEST_CASE("Preparation normalizes closed failures and unknown enum values",
          "[ops][preparation]") {
  Fixture f;
  auto failure = GENERATE(Error::unavailable, Error::permission_denied,
                          Error::timed_out, static_cast<Error>(99));
  f.factory->state->failure = failure;
  const auto value = request();
  REQUIRE(f.worker->submit(f.factory, value));
  auto done = f.complete(value.token);
  REQUIRE_FALSE(done.result);
  CHECK(done.result.error() ==
        (failure == static_cast<Error>(99) ? Error::invalid_result : failure));
  CHECK(f.factory->state->destroyed == 0);
}
TEST_CASE("Preparation uses full tokens and transfers each ready resource once",
          "[ops][preparation]") {
  Fixture f;
  const auto value = request();
  REQUIRE(f.worker->submit(f.factory, value));
  REQUIRE(until([&] { return f.worker->ready_results() == 1; }));
  auto foreign = value.token;
  SECTION("session") {
    foreign.session_id = id<domain::SessionId>("other");
  }
  SECTION("epoch") {
    ++foreign.session_epoch;
  }
  SECTION("target") {
    foreign.selection.target_id = id<domain::OpsTargetId>("other");
  }
  SECTION("revision") {
    foreign.selection.configuration_revision =
        id<domain::OpsConfigurationRevision>("other");
  }
  SECTION("kind") {
    foreign.selection.kind = domain::OpsTargetKind::kubernetes;
  }
  auto missing = f.worker->poll(foreign);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == Code::stale_request);
  CHECK_FALSE(f.worker->cancel(foreign));
  CHECK(f.worker->ready_results() == 1);
  CHECK(f.factory->state->destroyed == 0);
  auto done = f.complete(value.token);
  REQUIRE(done.result);
  CHECK_FALSE(f.worker->poll(value.token));
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  done.result->source.reset();
  CHECK(f.factory->state->destroyed == 1);
  CHECK(f.factory->state->destructor.load() == std::this_thread::get_id());
}
TEST_CASE("Discarded ready resources retain physical capacity during producer "
          "cleanup",
          "[ops][preparation]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.factory->state->destroy_block = gate;
  const auto value = request();
  REQUIRE(f.worker->submit(f.factory, value));
  REQUIRE(until([&] { return f.worker->ready_results() == 1; }));
  REQUIRE(f.worker->cancel(value.token));
  REQUIRE(gate->await());
  CHECK(f.worker->ready_results() == 0);
  CHECK(f.worker->occupied_slots() == 1);
  CHECK(f.factory->state->destroyed == 0);
  auto busy = f.worker->submit(f.factory, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  CHECK(f.factory->state->calls == 1);
  gate->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  CHECK(f.factory->state->destroyed == 1);
  CHECK(f.factory->state->destructor.load() ==
        f.factory->state->producer.load());
}
TEST_CASE("Cancellation and teardown never join blocked preparation or its "
          "stop callback",
          "[ops][preparation]") {
  Fixture f;
  auto block = std::make_shared<Gate>();
  Release release_block{block};
  auto callback = std::make_shared<Gate>();
  Release release_callback{callback};
  f.factory->state->block = block;
  f.factory->state->callback = callback;
  const auto value = request();
  REQUIRE(f.worker->submit(f.factory, value));
  REQUIRE(block->await());
  SECTION("cancel") {
    REQUIRE(f.worker->cancel(value.token));
  }
  SECTION("invalidate") {
    REQUIRE(f.worker->invalidate_session(value.token.session_id));
  }
  SECTION("destroy") {
    f.worker.reset();
  }
  REQUIRE(callback->await());
  CHECK(f.factory->state->calls == 1);
  CHECK(f.factory->state->callbacks == 1);
  if (f.worker) {
    CHECK(f.worker->occupied_slots() == 1);
    CHECK_FALSE(f.worker->poll(value.token));
  }
  callback->release();
  block->release();
  REQUIRE(until([&] { return f.factory->state->destroyed == 1; }));
  if (f.worker) REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  CHECK(f.factory->state->destructor.load() ==
        f.factory->state->producer.load());
}
TEST_CASE("Worker teardown discards an unclaimed source on the producer",
          "[ops][preparation]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.factory->state->destroy_block = gate;
  REQUIRE(f.worker->submit(f.factory, request()));
  REQUIRE(until([&] { return f.worker->ready_results() == 1; }));
  f.worker.reset();
  REQUIRE(gate->await());
  CHECK(f.factory->state->destroyed == 0);
  gate->release();
  REQUIRE(until([&] { return f.factory->state->destroyed == 1; }));
  CHECK(f.factory->state->destructor.load() ==
        f.factory->state->producer.load());
}
TEST_CASE("Logical expiry refuses a late prepared source without early slot "
          "replacement",
          "[ops][preparation]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.factory->state->block = gate;
  auto value = request();
  value.deadline = std::chrono::steady_clock::now() + 50ms;
  REQUIRE(f.worker->submit(f.factory, value));
  REQUIRE(gate->await());
  std::this_thread::sleep_until(value.deadline + 1ms);
  CHECK(f.worker->occupied_slots() == 1);
  CHECK(f.worker->ready_results() == 0);
  CHECK_FALSE(f.worker->submit(f.factory, request(2)));
  gate->release();
  auto done = f.complete(value.token);
  REQUIRE_FALSE(done.result);
  CHECK(done.result.error() == Error::timed_out);
  CHECK(f.factory->state->destroyed == 1);
  CHECK(f.factory->state->destructor.load() ==
        f.factory->state->producer.load());
}
TEST_CASE(
    "Preparation shares monotonic identities and capacity with browser work",
    "[ops][preparation]") {
  Fixture f;
  auto lease = std::make_shared<folder_grant_test::Lease>();
  runtime::LocalListRequest listing{
      {lease->session, lease->root, 1, 1, 1}, {}, {}};
  REQUIRE(f.worker->submit(lease, runtime::LocalSourceWorkRequest{listing}));
  auto busy = f.worker->submit(f.factory, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  CHECK(f.factory->state->calls == 0);
  REQUIRE(f.worker->cancel(listing.token));
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  auto stale = f.worker->submit(f.factory, request(1));
  REQUIRE_FALSE(stale);
  CHECK(stale.error().code == Code::stale_request);
  const auto allocated = f.worker->allocate_request_id();
  REQUIRE(allocated);
  REQUIRE(*allocated == 2);
  const auto next = request(*allocated);
  REQUIRE(f.worker->submit(f.factory, next));
  auto done = f.complete(next.token);
  REQUIRE(done.result);
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  auto highest = request(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(f.worker->submit(f.factory, highest));
  CHECK_FALSE(f.worker->allocate_request_id());
  REQUIRE(f.worker->cancel(highest.token));
}
TEST_CASE("Expired unclaimed preparation never transfers a physical source",
          "[ops][preparation]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.factory->state->destroy_block = gate;
  auto value = request();
  value.deadline = std::chrono::steady_clock::now() + 100ms;
  REQUIRE(f.worker->submit(f.factory, value));
  REQUIRE(until([&] { return f.worker->ready_results() == 1; }));
  REQUIRE(f.factory->state->destroyed == 0);
  std::this_thread::sleep_until(value.deadline + 1ms);
  auto done = f.complete(value.token);
  REQUIRE_FALSE(done.result);
  CHECK(done.result.error() == Error::timed_out);
  REQUIRE(gate->await());
  CHECK(f.worker->occupied_slots() == 1);
  CHECK_FALSE(f.worker->poll(value.token));
  gate->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  CHECK(f.factory->state->destructor.load() ==
        f.factory->state->producer.load());
}
TEST_CASE(
    "Producer factory cleanup retains the prepared resource and occupied slot",
    "[ops][preparation]") {
  Fixture f;
  const auto state = f.factory->state;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  state->factory_destroy_block = gate;
  const auto value = request();
  REQUIRE(f.worker->submit(f.factory, value));
  f.factory.reset();
  REQUIRE(gate->await());
  CHECK(state->factory_destroyed == 0);
  CHECK(state->destroyed == 0);
  CHECK(f.worker->occupied_slots() == 1);
  CHECK(f.worker->ready_results() == 0);
  REQUIRE(f.worker->cancel(value.token));
  gate->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  CHECK(state->factory_destroyed == 1);
  CHECK(state->destroyed == 1);
  CHECK(state->destructor.load() == state->producer.load());
}
TEST_CASE("Prepared source smoke does no observation and retains exact binding",
          "[ops][preparation]") {
  Fixture f;
  const auto value = request();
  REQUIRE(f.worker->submit(f.factory, value));
  auto done = f.complete(value.token);
  REQUIRE(done.result);
  CHECK(done.token == value.token);
  CHECK(done.result->source->target_binding() == binding());
  CHECK(f.factory->state->calls == 1);
  CHECK(f.factory->state->observations == 0);
  CHECK(f.factory->state->producer.load() != std::this_thread::get_id());
}
