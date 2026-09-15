#include "fixture.hpp"
#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_observation_broker.hpp>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace {
using namespace aiforge;
using namespace fixture;
using namespace std::chrono_literals;
template <class Predicate> auto until(Predicate predicate) -> bool {
  const auto end = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < end) {
    if (predicate()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}
} // namespace
TEST_CASE("A stalled native TLS initialization retains the physical source "
          "slot after deadline") {
  Peer peer{{response(list())}};
  const auto owner = source(configuration(peer.port()));
  auto req = request(*owner);
  req.limits.timeout = 150ms;
  auto authority =
      domain::OpsObservationAuthority::create({req.owner_id,
                                               req.session_id,
                                               req.target,
                                               req.selection_generation,
                                               {req.operation},
                                               {},
                                               req.limits});
  REQUIRE(authority);
  auto created = runtime::LocalSourceWorker::create(1);
  REQUIRE(created);
  std::shared_ptr<runtime::LocalSourceWorker> worker{std::move(*created)};
  auto broker = runtime::OpsObservationBroker::create(worker);
  REQUIRE(broker);
  auto endpoint = (*broker)->activate_session(req.session_id);
  REQUIRE(endpoint);
  REQUIRE((*broker)->select(*authority, owner));
  const auto invocation = domain::InvocationId::from("native-stall").value();
  TlsPause pause;
  std::optional<
      std::expected<runtime::OpsObservationReceipt, runtime::OpsBrokerFailure>>
      result;
  std::atomic<bool> complete{};
  std::jthread observer{[&](std::stop_token stop) {
    result.emplace((*endpoint)->observe(
        invocation, req, std::chrono::steady_clock::now() + req.limits.timeout,
        stop));
    complete.store(true);
  }};
  REQUIRE(until([&] {
    REQUIRE((*broker)->service());
    return pause.entered();
  }));
  REQUIRE(until([&] { return complete.load(); }));
  observer.join();
  REQUIRE(result);
  REQUIRE_FALSE(*result);
  CHECK(result->error().code == runtime::OpsBrokerError::timed_out);
  REQUIRE((*broker)->service());
  CHECK(worker->occupied_slots() == 1);
  CHECK(peer.accepted() == 0);
  const auto next_id = worker->allocate_request_id();
  REQUIRE(next_id);
  const runtime::OpsObservationWorkToken next{req.session_id, 1, *next_id, req};
  CHECK_FALSE(worker->submit(
      owner, runtime::OpsObservationWorkRequest{next, *authority}));
  pause.release();
  REQUIRE(until([&] { return worker->occupied_slots() == 0; }));
  CHECK(peer.accepted() == 0);
}

TEST_CASE("Kubernetes watcher startup failure refuses before any connection") {
  Peer peer{{response(list())}};
  const auto owner = source(configuration(peer.port()));
  const auto req = request(*owner);
  struct Reset {
    ~Reset() { fail_next_thread_start(false); }
  } reset;
  fail_next_thread_start(true);
  const auto result = owner->observe(req);
  fail_next_thread_start(false);
  REQUIRE_FALSE(result);
  CHECK(result.error() == Error::internal_failure);
  CHECK(peer.accepted() == 0);
}
