// Failure matrix: invalid limits/source/session/generation/log revisions and
// malformed/oversized requests make zero source calls. Mailbox and physical
// saturation fail once without retry. Cancellation before admission/dispatch,
// during a blocked read, and after receipt delivery invalidates publication.
// Foreign broker/old-session/wrong-invocation/wrong-request receipts never
// publish or consume another receipt; duplicate consumption fails. Selection,
// log-policy revocation and expiration invalidate completed receipts with zero
// additional IO. Completed receipts retain mailbox capacity. Source failures,
// malformed results and worker failures remain typed. Closing wakes waiters
// without waiting for stalled IO; exact retired physical slots remain occupied.
// Headless owner service is sufficient; a final smoke proves one collection and
// one neutral publication without kernel, network or rendering dependencies.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_observation_broker.hpp>

namespace {
using namespace aiforge;
using Code = runtime::OpsBrokerError;
using Clock = std::chrono::steady_clock;
using Receipt = runtime::OpsObservationReceipt;
using Failure = runtime::OpsBrokerFailure;
template <typename T> auto id(std::string value) -> T {
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
auto request(unsigned sequence = 1) -> domain::OpsObservationRequest {
  const auto spec = specification();
  return {spec.owner_id,
          spec.session_id,
          id<domain::OpsRequestId>("request-" + std::to_string(sequence)),
          spec.target,
          spec.selection_generation,
          domain::OpsObservationOperation::linux_health,
          {},
          spec.logs.revision,
          {}};
}
auto invocation(unsigned sequence = 1) -> domain::InvocationId {
  return id<domain::InvocationId>("invocation-" + std::to_string(sequence));
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool released{};
  auto wait() -> void {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return released; });
  }
  auto release() -> void {
    {
      const std::lock_guard lock(mutex);
      released = true;
    }
    changed.notify_all();
  }
};
struct Release {
  std::shared_ptr<Gate> gate;
  ~Release() { gate->release(); }
};
class Source final : public runtime::OpsObservationSource {
 public:
  domain::OpsTargetBinding target{specification().target};
  std::atomic<unsigned> calls{};
  std::shared_ptr<Gate> gate;
  bool guaranteed{true};
  bool malformed{};
  std::optional<runtime::OpsObservationSourceError> failure{};
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return guaranteed;
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return target;
  }
  auto observe(const domain::OpsObservationRequest& value, std::stop_token)
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++calls;
    if (gate) gate->wait();
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
    if (malformed) result.unsupported_entries = 1;
    if (value.operation == domain::OpsObservationOperation::linux_service_logs)
      result.payload = domain::OpsLogObservation{
          value.resource,
          {{result.completed_at, "bounded application evidence"}}};
    return result;
  }
};
template <typename Predicate> auto until(Predicate predicate) -> bool {
  const auto deadline = Clock::now() + std::chrono::seconds{3};
  while (!predicate()) {
    if (Clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}
struct Call {
  std::promise<std::expected<Receipt, Failure>> promise;
  std::future<std::expected<Receipt, Failure>> future{promise.get_future()};
  std::jthread thread;
  Call(std::shared_ptr<runtime::OpsObservationEndpoint> endpoint,
       domain::OpsObservationRequest value = request(), unsigned sequence = 1,
       Clock::time_point deadline = Clock::now() + std::chrono::seconds{5})
      : thread([this, endpoint = std::move(endpoint), value = std::move(value),
                sequence, deadline](std::stop_token stop) {
          promise.set_value(
              endpoint->observe(invocation(sequence), value, deadline, stop));
        }) {}
  auto ready() -> bool {
    return future.wait_for(std::chrono::milliseconds{0}) ==
           std::future_status::ready;
  }
};
struct Fixture {
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::shared_ptr<Source> source{std::make_shared<Source>()};
  std::unique_ptr<runtime::OpsObservationBroker> broker;
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint;
  explicit Fixture(std::size_t capacity = 1)
      : broker(
            runtime::OpsObservationBroker::create(worker, {capacity}).value()),
        endpoint(broker->activate_session(specification().session_id).value()) {
    REQUIRE(broker->select(
        domain::OpsObservationAuthority::create(specification()).value(),
        source));
  }
  auto pump(Call& call) -> std::expected<Receipt, Failure> {
    REQUIRE(until([&] {
      const auto serviced = broker->service();
      return !serviced || call.ready();
    }));
    return call.future.get();
  }
};
} // namespace

TEST_CASE("Ops broker refuses invalid setup and mutable authority",
          "[ops-broker]") {
  REQUIRE_FALSE(runtime::OpsObservationBroker::create(nullptr));
  auto worker = std::shared_ptr<runtime::LocalSourceWorker>{
      runtime::LocalSourceWorker::create().value()};
  REQUIRE_FALSE(runtime::OpsObservationBroker::create(worker, {0}));
  REQUIRE_FALSE(runtime::OpsObservationBroker::create(worker, {9}));
  Fixture fixture;
  auto unsafe = fixture.broker->activate_session(
      id<domain::SessionId>(std::string(1, static_cast<char>(0xff))));
  REQUIRE_FALSE(unsafe);
  REQUIRE(unsafe.error().code == Code::invalid_request);
  auto spec = specification();
  REQUIRE_FALSE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  ++spec.selection_generation;
  fixture.source->guaranteed = false;
  REQUIRE_FALSE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  fixture.source->guaranteed = true;
  spec.session_id = id<domain::SessionId>("foreign");
  REQUIRE_FALSE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  spec = specification();
  ++spec.selection_generation;
  ++spec.logs.revision;
  REQUIRE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  ++spec.selection_generation;
  --spec.logs.revision;
  REQUIRE_FALSE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  REQUIRE(fixture.source->calls == 0);
}

TEST_CASE("Ops endpoint validates before queue and stop before dispatch",
          "[ops-broker]") {
  Fixture fixture;
  auto unsafe = fixture.endpoint->observe(
      id<domain::InvocationId>(std::string(1, static_cast<char>(0xff))),
      request(), Clock::now() + std::chrono::seconds{1});
  REQUIRE_FALSE(unsafe);
  REQUIRE(unsafe.error().code == Code::invalid_request);
  auto value = request();
  value.limits.timeout = std::chrono::milliseconds{0};
  auto invalid = fixture.endpoint->observe(
      invocation(), value, Clock::now() + std::chrono::seconds{1});
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code == Code::invalid_request);
  REQUIRE_FALSE(fixture.broker->pending_work().value());
  auto expired =
      fixture.endpoint->observe(invocation(), request(), Clock::now());
  REQUIRE_FALSE(expired);
  REQUIRE(expired.error().code == Code::timed_out);
  std::stop_source stop;
  stop.request_stop();
  auto cancelled = fixture.endpoint->observe(
      invocation(), request(), Clock::now() + std::chrono::seconds{1},
      stop.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code == Code::cancelled);
  Call call{fixture.endpoint};
  REQUIRE(until([&] { return fixture.broker->pending_work().value(); }));
  call.thread.request_stop();
  REQUIRE(until([&] { return call.ready(); }));
  REQUIRE(call.future.get().error().code == Code::cancelled);
  REQUIRE(fixture.broker->service());
  REQUIRE_FALSE(fixture.broker->pending_work().value());
  REQUIRE(fixture.source->calls == 0);
}

TEST_CASE("Ops queue deadline expires without an owner pump", "[ops-broker]") {
  Fixture fixture;
  auto value = request();
  value.limits.timeout = std::chrono::milliseconds{1};
  Call call{fixture.endpoint, value};
  REQUIRE(until([&] { return call.ready(); }));
  REQUIRE(call.future.get().error().code == Code::timed_out);
  REQUIRE(fixture.broker->service());
  REQUIRE_FALSE(fixture.broker->pending_work().value());
  REQUIRE(fixture.source->calls == 0);
}

TEST_CASE("Ops duplicate pending IDs are refused before dispatch",
          "[ops-broker]") {
  Fixture fixture{2};
  Call first{fixture.endpoint};
  REQUIRE(until([&] { return fixture.broker->pending_work().value(); }));
  auto duplicate_invocation = fixture.endpoint->observe(
      invocation(), request(2), Clock::now() + std::chrono::seconds{1});
  REQUIRE_FALSE(duplicate_invocation);
  REQUIRE(duplicate_invocation.error().code == Code::stale_request);
  auto duplicate_request = fixture.endpoint->observe(
      invocation(2), request(), Clock::now() + std::chrono::seconds{1});
  REQUIRE_FALSE(duplicate_request);
  REQUIRE(duplicate_request.error().code == Code::stale_request);
  REQUIRE(fixture.source->calls == 0);
  REQUIRE(fixture.broker->cancel_invocation(invocation()));
  REQUIRE(until([&] { return first.ready(); }));
  REQUIRE(first.future.get().error().code == Code::cancelled);
  REQUIRE(fixture.broker->service());
  REQUIRE(fixture.source->calls == 0);
}

TEST_CASE("Ops structurally valid stale authority is rejected at dispatch",
          "[ops-broker]") {
  Fixture fixture;
  auto value = request();
  ++value.selection_generation;
  REQUIRE(domain::validate_recorded_ops_request(value));
  Call call{fixture.endpoint, value};
  auto result = fixture.pump(call);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == Code::stale_request);
  REQUIRE(fixture.source->calls == 0);
  REQUIRE_FALSE(fixture.broker->pending_work().value());
}

TEST_CASE("Ops enabled log receipt is invalid after consent changes",
          "[ops-broker]") {
  Fixture fixture;
  auto spec = specification();
  ++spec.selection_generation;
  spec.operations.push_back(
      domain::OpsObservationOperation::linux_service_logs);
  spec.logs.enabled = true;
  ++spec.logs.revision;
  spec.logs.permitted_sources.push_back(domain::LinuxServiceIdentity{
      "application.service", id<domain::OpsResourceUid>("invocation")});
  REQUIRE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  auto value = request();
  value.selection_generation = spec.selection_generation;
  value.log_policy_revision = spec.logs.revision;
  value.operation = domain::OpsObservationOperation::linux_service_logs;
  value.resource = spec.logs.permitted_sources.front();
  Call call{fixture.endpoint, value};
  auto receipt = fixture.pump(call);
  REQUIRE(receipt);
  ++spec.selection_generation;
  spec.logs.enabled = false;
  REQUIRE_FALSE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  ++spec.logs.revision;
  REQUIRE(fixture.broker->select(
      domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  REQUIRE_FALSE(
      fixture.broker->take_for_publication(*receipt, invocation(), value));
  REQUIRE(fixture.source->calls == 1);
}

TEST_CASE("Ops completed receipt retains mailbox and refuses foreign proof",
          "[ops-broker]") {
  Fixture fixture;
  Call call{fixture.endpoint};
  auto receipt = fixture.pump(call);
  REQUIRE(receipt);
  REQUIRE(fixture.broker->pending_work().value());
  auto busy = fixture.endpoint->observe(invocation(2), request(2),
                                        Clock::now() + std::chrono::seconds{1});
  REQUIRE_FALSE(busy);
  REQUIRE(busy.error().code == Code::busy);
  Fixture foreign;
  REQUIRE_FALSE(
      foreign.broker->take_for_publication(*receipt, invocation(), request()));
  REQUIRE_FALSE(
      fixture.broker->take_for_publication(*receipt, invocation(2), request()));
  auto wrong = request();
  ++wrong.log_policy_revision;
  REQUIRE_FALSE(
      fixture.broker->take_for_publication(*receipt, invocation(), wrong));
  REQUIRE(fixture.broker->pending_work().value());
  REQUIRE(
      fixture.broker->take_for_publication(*receipt, invocation(), request()));
  REQUIRE_FALSE(
      fixture.broker->take_for_publication(*receipt, invocation(), request()));
  REQUIRE_FALSE(fixture.broker->pending_work().value());
  REQUIRE(fixture.source->calls == 1);
  REQUIRE(foreign.source->calls == 0);
}

TEST_CASE("Ops stale target and same session reactivation invalidate receipts",
          "[ops-broker]") {
  Fixture fixture;
  Call call{fixture.endpoint};
  auto receipt = fixture.pump(call);
  REQUIRE(receipt);
  SECTION("selection changes") {
    auto spec = specification();
    ++spec.selection_generation;
    ++spec.logs.revision;
    REQUIRE(fixture.broker->select(
        domain::OpsObservationAuthority::create(spec).value(), fixture.source));
  }
  SECTION("same session has new issuer") {
    auto endpoint =
        fixture.broker->activate_session(specification().session_id);
    REQUIRE(endpoint);
    auto closed = fixture.endpoint->observe(
        invocation(2), request(2), Clock::now() + std::chrono::seconds{1});
    REQUIRE_FALSE(closed);
    REQUIRE(closed.error().code == Code::closed);
  }
  REQUIRE_FALSE(
      fixture.broker->take_for_publication(*receipt, invocation(), request()));
  REQUIRE(fixture.source->calls == 1);
}

TEST_CASE("Ops cancellation and deadline also cover publication gap",
          "[ops-broker]") {
  Fixture fixture;
  Call call{fixture.endpoint};
  auto receipt = fixture.pump(call);
  REQUIRE(receipt);
  SECTION("stop after receipt") {
    call.thread.request_stop();
  }
  SECTION("explicit owner cancel") {
    REQUIRE(fixture.broker->cancel_invocation(invocation()));
  }
  SECTION("deadline after receipt") {
    auto expired = fixture.broker->take_for_publication(
        *receipt, invocation(), request(),
        Clock::now() + std::chrono::seconds{10});
    REQUIRE_FALSE(expired);
    REQUIRE(expired.error().code == Code::timed_out);
  }
  REQUIRE_FALSE(
      fixture.broker->take_for_publication(*receipt, invocation(), request()));
  REQUIRE(fixture.source->calls == 1);
}

TEST_CASE("Ops source failures stay typed and no retry occurs",
          "[ops-broker]") {
  Fixture fixture;
  SECTION("source failure") {
    fixture.source->failure = runtime::OpsObservationSourceError::trust_failed;
  }
  SECTION("malformed result") {
    fixture.source->malformed = true;
  }
  Call call{fixture.endpoint};
  auto result = fixture.pump(call);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == Code::source_failure);
  REQUIRE(result.error().source ==
          (fixture.source->malformed
               ? runtime::OpsObservationSourceError::invalid_result
               : runtime::OpsObservationSourceError::trust_failed));
  REQUIRE(fixture.broker->service());
  REQUIRE_FALSE(fixture.broker->pending_work().value());
  REQUIRE(fixture.source->calls == 1);
}

TEST_CASE("Ops close wakes waiter while physical read remains blocked",
          "[ops-broker]") {
  Fixture fixture;
  auto gate = std::make_shared<Gate>();
  fixture.source->gate = gate;
  Release release{gate};
  Call call{fixture.endpoint};
  REQUIRE(until([&] { return fixture.broker->pending_work().value(); }));
  REQUIRE(fixture.broker->service());
  REQUIRE(until([&] { return fixture.source->calls == 1; }));
  REQUIRE(fixture.broker->close());
  REQUIRE(until([&] { return call.ready(); }));
  REQUIRE(call.future.get().error().code == Code::closed);
  REQUIRE(fixture.worker->occupied_slots() == 1);
  REQUIRE_FALSE(fixture.broker->activate_session(specification().session_id));
}

TEST_CASE("Ops shared physical capacity remains occupied after cancellation",
          "[ops-broker]") {
  Fixture fixture;
  auto gate = std::make_shared<Gate>();
  fixture.source->gate = gate;
  Release release{gate};
  Call first{fixture.endpoint};
  REQUIRE(until([&] { return fixture.broker->pending_work().value(); }));
  REQUIRE(fixture.broker->service());
  REQUIRE(until([&] { return fixture.source->calls == 1; }));
  REQUIRE(fixture.broker->cancel_invocation(invocation()));
  REQUIRE(until([&] { return first.ready(); }));
  REQUIRE(first.future.get().error().code == Code::cancelled);
  Call second{fixture.endpoint, request(2), 2};
  auto result = fixture.pump(second);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == Code::busy);
  REQUIRE(fixture.source->calls == 1);
  REQUIRE(fixture.worker->occupied_slots() == 1);
}

TEST_CASE("Ops owner service publishes one bounded observation",
          "[ops-broker]") {
  Fixture fixture;
  Call call{fixture.endpoint};
  auto receipt = fixture.pump(call);
  REQUIRE(receipt);
  auto result =
      fixture.broker->take_for_publication(*receipt, invocation(), request());
  REQUIRE(result);
  REQUIRE(result->request == request());
  REQUIRE(fixture.source->calls == 1);
  REQUIRE_FALSE(fixture.broker->pending_work().value());
}
