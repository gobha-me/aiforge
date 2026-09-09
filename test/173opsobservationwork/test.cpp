// Failure matrix (written before the worker extension):
// Reject unclaimed/null sources, wrong target, forged authority/token, invalid
// limits and log policy before observe, refresh or cancellation callbacks.
// Reject malformed/foreign observations, unknown source errors and exceptions.
// Exact tokens and global request IDs isolate stale/foreign/consumed results.
// Running, cancelled, completed-unpolled and physically retiring Ops work
// shares one bounded capacity with local and repository work.
// Owner cancel, session invalidation and teardown never join blocked observe,
// stop callbacks or final source destruction. Gate releases survive failures.
// Both thread-start failures retain caller ownership and make zero source
// calls. Successful bounded completion is immutable and consumed exactly once.

#include "../169repositorywork/fixture.hpp"
#include <aiforge/runtime/ops_observation_source.hpp>
#include <cerrno>
#include <pthread.h>

namespace {
std::atomic<int> fail_thread{};
}
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                     void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread,
                                     const pthread_attr_t* attributes,
                                     void* (*start)(void*), void* argument) {
  auto remaining = fail_thread.load();
  while (remaining > 0) {
    if (fail_thread.compare_exchange_weak(remaining, remaining - 1)) {
      if (remaining == 1) return EAGAIN;
      break;
    }
  }
  return __real_pthread_create(thread, attributes, start, argument);
}
namespace {
using namespace aiforge;
using folder_grant_test::Destruction;
using folder_grant_test::Gate;
using folder_grant_test::Release;
using folder_grant_test::until;
using repository_work_test::returns_before_release;
using Code = runtime::LocalSourceWorkerErrorCode;
using Error = runtime::OpsObservationSourceError;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto specification() -> domain::OpsObservationAuthoritySpec {
  return {id<domain::OpsOwnerId>("owner"),
          id<domain::SessionId>("session"),
          {id<domain::OpsTargetId>("target"),
           id<domain::OpsConfigurationRevision>("revision"),
           domain::LinuxOpsIdentity{domain::LinuxExecutionScope::container,
                                    "12345678-1234-1234-1234-123456789abc", 42,
                                    43}},
          1,
          {domain::OpsObservationOperation::linux_health},
          {},
          {}};
}
auto request(std::uint64_t sequence = 1) -> runtime::OpsObservationWorkRequest {
  const auto spec = specification();
  domain::OpsObservationRequest observation{
      spec.owner_id,
      spec.session_id,
      id<domain::OpsRequestId>("observation-" + std::to_string(sequence)),
      spec.target,
      spec.selection_generation,
      domain::OpsObservationOperation::linux_health,
      {},
      spec.logs.revision,
      {}};
  return {{spec.session_id, 1, sequence, std::move(observation)},
          domain::OpsObservationAuthority::create(spec).value()};
}
auto token(std::uint64_t sequence = 1) -> runtime::OpsObservationWorkToken {
  return request(sequence).token;
}
class UnclaimedSource : public runtime::OpsObservationSource {
 public:
  domain::OpsTargetBinding target{specification().target};
  std::atomic<unsigned> calls{}, callback_calls{};
  std::shared_ptr<Gate> block, callback;
  std::shared_ptr<Destruction> destruction;
  std::optional<Error> failure{};
  enum class Mode {
    valid,
    foreign,
    malformed,
    oversized,
    throws
  } mode{Mode::valid};
  ~UnclaimedSource() override {
    if (destruction) {
      destruction->thread = std::this_thread::get_id();
      if (destruction->gate) destruction->gate->wait();
      destruction->done = true;
    }
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return target;
  }
  auto observe(const domain::OpsObservationRequest& value, std::stop_token stop)
      -> std::expected<domain::OpsObservation, Error> override {
    ++calls;
    std::optional<std::stop_callback<std::function<void()>>> stopped;
    if (callback)
      stopped.emplace(stop, [this, gate = callback] {
        ++callback_calls;
        gate->wait();
      });
    if (block) block->wait();
    if (mode == Mode::throws)
      throw std::runtime_error("secret /private/config token");
    if (failure) return std::unexpected(*failure);
    domain::OpsObservation result{
        value,
        domain::EventTimestamp{std::chrono::milliseconds{1000}},
        domain::EventTimestamp{std::chrono::milliseconds{1001}},
        domain::OpsObservationCompleteness::complete,
        0,
        0,
        {},
        domain::LinuxHealthObservation{
            domain::OpsHealthState::healthy, 12, {}, 1, 0}};
    if (mode == Mode::foreign) ++result.request.selection_generation;
    if (mode == Mode::malformed) result.unsupported_entries = 1;
    if (mode == Mode::oversized) result.source_version = std::string(129, 'x');
    return result;
  }
};
class Source final : public UnclaimedSource {
 public:
  bool guaranteed{true};
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return guaranteed;
  }
};
struct Fixture {
  std::shared_ptr<Source> source = std::make_shared<Source>();
  std::unique_ptr<runtime::LocalSourceWorker> worker =
      runtime::LocalSourceWorker::create(1).value();
  auto completed(runtime::OpsObservationWorkToken work = token())
      -> runtime::OpsObservationWorkCompletion {
    REQUIRE(until([&] { return worker->ready_results() == 1; }));
    auto result = worker->poll(work);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
  }
};
} // namespace

TEST_CASE(
    "Ops worker rejects unclaimed sources and invalid snapshots before IO") {
  Fixture f;
  auto callback = std::make_shared<Gate>();
  Release release{callback};
  f.source->callback = callback;
  auto unclaimed = std::make_shared<UnclaimedSource>();
  CHECK_FALSE(f.worker->submit(unclaimed, request()));
  CHECK(unclaimed->calls == 0);
  const std::shared_ptr<runtime::OpsObservationSource> absent;
  CHECK_FALSE(f.worker->submit(absent, request()));
  for (unsigned invalid = 0; invalid < 15; ++invalid) {
    auto work = request();
    switch (invalid) {
      case 0: f.source->guaranteed = false; break;
      case 1:
        f.source->target.configuration_revision =
            id<domain::OpsConfigurationRevision>("foreign");
        break;
      case 2: work.token.session_epoch = 0; break;
      case 3: work.token.request_id = 0; break;
      case 4: work.token.session_id = id<domain::SessionId>("foreign"); break;
      case 5: ++work.token.observation.selection_generation; break;
      case 6: ++work.token.observation.log_policy_revision; break;
      case 7:
        work.token.observation.owner_id = id<domain::OpsOwnerId>("foreign");
        break;
      case 8: work.token.observation.limits.maximum_bytes = 0; break;
      case 9:
        work.token.observation.operation =
            domain::OpsObservationOperation::linux_service_logs;
        break;
      case 10: ++work.token.observation.limits.maximum_bytes; break;
      case 11:
      case 12: {
        auto foreign = specification();
        if (invalid == 11)
          foreign.owner_id = id<domain::OpsOwnerId>("other");
        else
          foreign.session_id = id<domain::SessionId>("other");
        work.authority =
            domain::OpsObservationAuthority::create(foreign).value();
        break;
      }
      case 13:
      case 14: {
        auto logs = specification();
        logs.operations = {domain::OpsObservationOperation::linux_service_logs};
        const domain::LinuxServiceIdentity selected{
            "selected.service", id<domain::OpsResourceUid>("invocation")};
        if (invalid == 14) {
          logs.logs.enabled = true;
          logs.logs.permitted_sources = {selected};
        }
        work.authority = domain::OpsObservationAuthority::create(logs).value();
        work.token.observation.operation = logs.operations.front();
        work.token.observation.resource =
            invalid == 13
                ? selected
                : domain::LinuxServiceIdentity{
                      "foreign.service", id<domain::OpsResourceUid>("other")};
        break;
      }
    }
    auto refused = f.worker->submit(f.source, work);
    INFO(invalid);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == Code::invalid_request);
    CHECK(f.source->calls == 0);
    CHECK(f.source->callback_calls == 0);
    CHECK(f.worker->occupied_slots() == 0);
    f.source->guaranteed = true;
    f.source->target = specification().target;
  }
  // Invalid preflight never consumes the global numeric request identity.
  REQUIRE(f.worker->submit(f.source, request()));
  REQUIRE(f.completed().result);
  CHECK(f.source->callback_calls == 0);
}
TEST_CASE("Ops worker returns only typed validated errors and complete "
          "observations") {
  for (auto mode :
       {UnclaimedSource::Mode::foreign, UnclaimedSource::Mode::malformed,
        UnclaimedSource::Mode::oversized, UnclaimedSource::Mode::throws}) {
    Fixture f;
    f.source->mode = mode;
    REQUIRE(f.worker->submit(f.source, request()));
    const auto result = f.completed();
    REQUIRE_FALSE(result.result);
    CHECK(result.result.error() == (mode == UnclaimedSource::Mode::throws
                                        ? Error::internal_failure
                                        : Error::invalid_result));
    CHECK(result.token == token());
  }
  for (int error = -1; error <= 12; ++error) {
    Fixture f;
    f.source->failure = static_cast<Error>(error);
    REQUIRE(f.worker->submit(f.source, request()));
    const auto result = f.completed();
    REQUIRE_FALSE(result.result);
    CHECK(result.result.error() == (error < 0 || error == 12
                                        ? Error::invalid_result
                                        : static_cast<Error>(error)));
  }
}
TEST_CASE("Ops cancellation discards late delivery while retaining mixed "
          "source capacity") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.source->block = gate;
  REQUIRE(f.worker->submit(f.source, request()));
  REQUIRE(gate->await());
  for (unsigned change = 0; change < 5; ++change) {
    auto foreign = token();
    switch (change) {
      case 0: ++foreign.session_epoch; break;
      case 1: ++foreign.request_id; break;
      case 2: foreign.session_id = id<domain::SessionId>("foreign"); break;
      case 3: ++foreign.observation.log_policy_revision; break;
      case 4:
        foreign.observation.target.configuration_revision =
            id<domain::OpsConfigurationRevision>("other");
        break;
    }
    CHECK_FALSE(f.worker->poll(foreign));
    CHECK_FALSE(f.worker->cancel(foreign));
  }
  bool cancelled{};
  returns_before_release(
      [&] { cancelled = f.worker->cancel(token()).has_value(); }, gate);
  REQUIRE(cancelled);
  CHECK_FALSE(f.worker->poll(token()));
  auto busy = f.worker->submit(f.source, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  auto reader = std::make_shared<folder_grant_test::Lease>();
  runtime::LocalListRequest listing{
      {token().session_id, reader->root, 1, 2, 1}, {}, {}};
  auto local_busy =
      f.worker->submit(reader, runtime::LocalSourceWorkRequest{listing});
  REQUIRE_FALSE(local_busy);
  CHECK(local_busy.error().code == Code::busy);
  auto repository_source = std::make_shared<repository_work_test::Source>();
  auto repository = runtime::RepositoryContextController::create_owned(
      repository_source, repository_source->snapshot.root);
  REQUIRE(repository);
  auto repository_busy =
      f.worker->submit(*repository, repository_work_test::request(2));
  REQUIRE_FALSE(repository_busy);
  CHECK(repository_busy.error().code == Code::busy);
  CHECK(repository_source->observations == 0);
  gate->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  CHECK_FALSE(f.worker->poll(token()));
  REQUIRE(f.worker->submit(f.source, request(2)));
  REQUIRE(f.completed(token(2)).result);
}
TEST_CASE("Ops worker teardown and session invalidation never join a blocked "
          "source") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.source->block = gate;
  REQUIRE(f.worker->submit(f.source, request()));
  REQUIRE(gate->await());
  SECTION("owner destruction") {
    returns_before_release([&] { f.worker.reset(); }, gate);
    CHECK_FALSE(f.worker);
  }
  SECTION("session switch") {
    bool invalidated{};
    returns_before_release(
        [&] {
          invalidated =
              f.worker->invalidate_session(token().session_id).has_value();
        },
        gate);
    REQUIRE(invalidated);
    CHECK_FALSE(f.worker->poll(token()));
    CHECK(f.worker->occupied_slots() == 1);
  }
  gate->release();
}
TEST_CASE("Ops stop callback runs on relay and holds retired capacity until "
          "finished") {
  Fixture f;
  auto read = std::make_shared<Gate>();
  auto callback = std::make_shared<Gate>();
  Release release_read{read};
  Release release_callback{callback};
  f.source->block = read;
  f.source->callback = callback;
  REQUIRE(f.worker->submit(f.source, request()));
  REQUIRE(read->await());
  bool cancelled{};
  returns_before_release(
      [&] { cancelled = f.worker->cancel(token()).has_value(); }, callback);
  REQUIRE(cancelled);
  REQUIRE(callback->await());
  CHECK(f.source->callback_calls == 1);
  read->release();
  CHECK(f.worker->occupied_slots() == 1);
  auto busy = f.worker->submit(f.source, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  callback->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
}
TEST_CASE("Ops physical source destruction precedes delivery and slot reuse") {
  Fixture f;
  auto read = std::make_shared<Gate>();
  auto destruction_gate = std::make_shared<Gate>();
  auto destruction = std::make_shared<Destruction>();
  destruction->gate = destruction_gate;
  Release release_read{read};
  Release release_destructor{destruction_gate};
  f.source->block = read;
  f.source->destruction = destruction;
  REQUIRE(f.worker->submit(f.source, request()));
  REQUIRE(read->await());
  f.source.reset();
  read->release();
  REQUIRE(destruction_gate->await());
  CHECK(destruction->thread != std::this_thread::get_id());
  CHECK_FALSE(destruction->done.load());
  CHECK(f.worker->ready_results() == 0);
  auto pending = f.worker->poll(token());
  REQUIRE(pending);
  CHECK_FALSE(*pending);
  auto replacement = std::make_shared<Source>();
  auto busy = f.worker->submit(replacement, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  SECTION("cancel during physical cleanup") {
    bool cancelled{};
    returns_before_release(
        [&] { cancelled = f.worker->cancel(token()).has_value(); },
        destruction_gate);
    REQUIRE(cancelled);
    CHECK_FALSE(f.worker->poll(token()));
    destruction_gate->release();
    REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  }
  SECTION("normal completion waits for cleanup") {
    destruction_gate->release();
    REQUIRE(f.completed().result);
  }
  REQUIRE(destruction->done.load());
  REQUIRE(f.worker->submit(replacement, request(2)));
  REQUIRE(f.completed(token(2)).result);
}
TEST_CASE("Ops completed unpolled values retain capacity and exact "
          "consume-once identity") {
  Fixture f;
  for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
    REQUIRE(f.worker->submit(f.source, request(sequence)));
    REQUIRE(until([&] { return f.worker->ready_results() == 1; }));
    CHECK(f.worker->occupied_slots() == 1);
    auto busy = f.worker->submit(f.source, request(sequence + 1));
    REQUIRE_FALSE(busy);
    CHECK(busy.error().code == Code::busy);
    const auto result = f.completed(token(sequence));
    REQUIRE(result.result);
    CHECK(result.result->request == token(sequence).observation);
    CHECK_FALSE(f.worker->poll(token(sequence)));
    CHECK_FALSE(f.worker->submit(f.source, request(sequence)));
    CHECK(f.worker->occupied_slots() == 0);
  }
}
TEST_CASE("Both Ops startup failures preserve caller source without IO or "
          "capacity leak") {
  for (int failed = 1; failed <= 2; ++failed) {
    Fixture f;
    std::shared_ptr<runtime::OpsObservationSource> port = f.source;
    const auto* original = port.get();
    fail_thread = failed;
    auto result = f.worker->submit(std::move(port), request());
    const auto remaining = fail_thread.exchange(0);
    INFO(failed);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::internal_failure);
    CHECK(remaining == 0);
    CHECK(port.get() == original);
    CHECK(f.source->calls == 0);
    REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
    CHECK_FALSE(f.worker->submit(f.source, request()));
    REQUIRE(f.worker->submit(f.source, request(2)));
    REQUIRE(f.completed(token(2)).result);
  }
}
TEST_CASE(
    "Ops worker success preserves exact request and explicit typed health") {
  Fixture f;
  REQUIRE(f.worker->submit(f.source, request()));
  const auto result = f.completed();
  REQUIRE(result.result);
  CHECK(result.token == token());
  CHECK(result.result->request == token().observation);
  CHECK(std::get<domain::LinuxHealthObservation>(result.result->payload)
            .kernel_uptime_seconds == 12);
  CHECK(f.source->calls == 1);
}
