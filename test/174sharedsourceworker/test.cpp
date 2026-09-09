// Failure matrix written before implementation:
// Null shared worker and invalid factory/limits are refused without work.
// Allocation uses both allocation/admission high-water marks without slots,
// reservations, rollback or wrapping; explicit higher admission stales old IDs.
// Busy and abandoned allocations remain consumed; exhaustion is permanent.
// Browser and Ops/explicit work interleave in the same bounded pool.
// Browser activation/deactivation/destruction cancels only its exact jobs and
// revokes its grants, never unrelated blocked Ops work in the same session.
// Application global invalidation stales both controllers' work immediately
// while blocked physical work retains capacity until RAII-released completion.
// Existing single-owner thread constraints remain unchanged.

#include "../159foldergrant/fixture.hpp"
#include "../169repositorywork/fixture.hpp"
#include <aiforge/surfaces/local_source_browser.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace aiforge;
using folder_grant_test::Gate;
using folder_grant_test::Release;
using folder_grant_test::until;
using repository_work_test::returns_before_release;
using Worker = runtime::LocalSourceWorker;
using Browser = surfaces::LocalSourceBrowser;
using Code = runtime::LocalSourceWorkerErrorCode;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto ops_spec() -> domain::OpsObservationAuthoritySpec {
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
auto ops_work(std::uint64_t sequence) -> runtime::OpsObservationWorkRequest {
  const auto spec = ops_spec();
  domain::OpsObservationRequest observation{
      spec.owner_id,
      spec.session_id,
      id<domain::OpsRequestId>("ops-" + std::to_string(sequence)),
      spec.target,
      1,
      domain::OpsObservationOperation::linux_health,
      {},
      spec.logs.revision,
      {}};
  return {{spec.session_id, 1, sequence, observation},
          domain::OpsObservationAuthority::create(spec).value()};
}
// Both gates must release before joining the assertion helper on failure.
template <class Action>
auto returns_before_release_both(Action action,
                                 const std::shared_ptr<Gate>& first,
                                 const std::shared_ptr<Gate>& second) -> void {
  std::atomic<bool> returned{};
  std::jthread owner{[&] {
    action();
    returned = true;
  }};
  Release release_first{first};
  Release release_second{second};
  REQUIRE(until([&] { return returned.load(); }));
  owner.join();
  release_first.enabled = false;
  release_second.enabled = false;
}
class OpsSource final : public runtime::OpsObservationSource {
 public:
  domain::OpsTargetBinding target{ops_spec().target};
  std::shared_ptr<Gate> block;
  std::atomic<unsigned> calls{};
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return target;
  }
  auto observe(const domain::OpsObservationRequest& request, std::stop_token)
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++calls;
    if (block) block->wait();
    return domain::OpsObservation{
        request,
        domain::EventTimestamp{std::chrono::milliseconds{1}},
        domain::EventTimestamp{std::chrono::milliseconds{2}},
        domain::OpsObservationCompleteness::complete,
        0,
        0,
        {},
        domain::LinuxHealthObservation{
            domain::OpsHealthState::healthy, 1, {}, 0, 0}};
  }
};
class Factory final : public runtime::LocalSourceGrantFactory {
 public:
  bool guaranteed{true};
  std::shared_ptr<Gate> block;
  std::atomic<unsigned> calls{};
  std::atomic<std::uint64_t> last_request{};
  std::weak_ptr<folder_grant_test::Lease> lease;
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return guaranteed;
  }
  auto grant(const runtime::LocalFolderGrantRequest& request, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    ++calls;
    last_request = request.token.request_id;
    if (block) block->wait();
    auto owned = std::make_shared<folder_grant_test::Lease>();
    owned->session = request.token.session_id;
    owned->generation = request.lease_generation;
    lease = owned;
    return runtime::LocalFolderGrantResult{request.token, owned->root,
                                           std::move(owned)};
  }
};
struct Fixture {
  std::shared_ptr<Worker> worker;
  std::shared_ptr<Factory> factory = std::make_shared<Factory>();
  std::shared_ptr<OpsSource> source = std::make_shared<OpsSource>();
  std::unique_ptr<Browser> browser;
  explicit Fixture(std::size_t capacity = 2)
      : worker(Worker::create(capacity).value()) {
    auto created = Browser::create_with_worker(factory, worker);
    REQUIRE(created);
    browser = std::move(*created);
    REQUIRE(browser->activate_session(ops_spec().session_id));
  }
  auto finish_ops(const runtime::OpsObservationWorkToken& token) -> void {
    REQUIRE(until([&] { return worker->ready_results() == 1; }));
    auto result = worker->poll(token);
    REQUIRE(result);
    REQUIRE(*result);
    REQUIRE((**result).result);
  }
  auto grant() -> void {
    REQUIRE(browser->add_folder("/private/folder"));
    REQUIRE(until([&] {
      auto result = browser->poll();
      return result && !browser->state().granting;
    }));
    REQUIRE(browser->state().folders.size() == 1);
    REQUIRE(until([&] { return worker->occupied_slots() == 0; }));
  }
};
} // namespace

TEST_CASE(
    "Shared source browser refuses absent pool and invalid factory or limits") {
  auto factory = std::make_shared<Factory>();
  auto missing = Browser::create_with_worker(factory, {});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == domain::LocalSourceErrorCode::invalid_request);
  std::shared_ptr<Worker> worker{Worker::create().value()};
  CHECK_FALSE(Browser::create_with_worker({}, worker));
  factory->guaranteed = false;
  CHECK_FALSE(Browser::create_with_worker(factory, worker));
  factory->guaranteed = true;
  auto invalid = domain::LocalSourceLimits{};
  invalid.maximum_roots = 0;
  CHECK_FALSE(Browser::create_with_worker(factory, worker, invalid));
  CHECK(factory->calls == 0);
  CHECK(worker->occupied_slots() == 0);
}
TEST_CASE("Shared allocator accounts for abandoned IDs and explicit higher "
          "admission") {
  Fixture f;
  REQUIRE(f.worker->allocate_request_id() == 1);
  REQUIRE(f.worker->allocate_request_id() == 2);
  CHECK(f.worker->occupied_slots() == 0);
  // Allocation does not reserve an ID against a higher admitted explicit job.
  REQUIRE(f.worker->submit(f.source, ops_work(20)));
  f.finish_ops(ops_work(20).token);
  auto old = f.worker->submit(f.source, ops_work(2));
  REQUIRE_FALSE(old);
  CHECK(old.error().code == Code::stale_request);
  REQUIRE(f.worker->allocate_request_id() == 21);
  REQUIRE(f.worker->allocate_request_id() == 22);
  auto malformed = ops_work(100);
  malformed.token.session_epoch = 0;
  CHECK_FALSE(f.worker->submit(f.source, malformed));
  REQUIRE(f.worker->allocate_request_id() == 23);
}
TEST_CASE("Busy shared allocations never roll back or consume a worker slot") {
  Fixture f{1};
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.source->block = gate;
  const auto first = f.worker->allocate_request_id();
  REQUIRE(first);
  REQUIRE(f.worker->submit(f.source, ops_work(*first)));
  REQUIRE(gate->await());
  auto second = f.worker->allocate_request_id();
  REQUIRE(second);
  CHECK(*second == 2);
  auto busy = f.worker->submit(f.source, ops_work(*second));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  REQUIRE(f.worker->allocate_request_id() == 3);
  CHECK(f.worker->occupied_slots() == 1);
  gate->release();
  f.finish_ops(ops_work(*first).token);
  REQUIRE(f.worker->allocate_request_id() == 4);
}
TEST_CASE(
    "Shared request identity exhaustion never wraps or mints browser work") {
  for (bool exhaust_by_allocation : {false, true}) {
    Fixture f;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto admitted = exhaust_by_allocation ? maximum - 1 : maximum;
    REQUIRE(f.worker->submit(f.source, ops_work(admitted)));
    f.finish_ops(ops_work(admitted).token);
    if (exhaust_by_allocation)
      REQUIRE(f.worker->allocate_request_id() == maximum);
    for (int attempt = 0; attempt < 2; ++attempt) {
      auto exhausted = f.worker->allocate_request_id();
      REQUIRE_FALSE(exhausted);
      CHECK(exhausted.error().code == Code::resource_exhausted);
    }
    auto browser = f.browser->add_folder("/private/folder");
    REQUIRE_FALSE(browser);
    CHECK(browser.error().code ==
          domain::LocalSourceErrorCode::resource_exhausted);
    CHECK(f.factory->calls == 0);
    CHECK(f.worker->occupied_slots() == 0);
  }
}
TEST_CASE(
    "Browser and Ops interleave allocation with explicit repository work") {
  Fixture f;
  f.grant();
  CHECK(f.factory->last_request == 1);
  auto ops = f.worker->allocate_request_id();
  REQUIRE(ops);
  CHECK(*ops == 2);
  REQUIRE(f.worker->submit(f.source, ops_work(*ops)));
  f.finish_ops(ops_work(*ops).token);
  auto repository_source = std::make_shared<repository_work_test::Source>();
  auto repository = runtime::RepositoryContextController::create_owned(
      repository_source, repository_source->snapshot.root);
  REQUIRE(repository);
  auto explicit_work = repository_work_test::request(50);
  REQUIRE(f.worker->submit(*repository, explicit_work));
  REQUIRE(until([&] { return f.worker->ready_result(explicit_work.token); }));
  auto result = f.worker->poll(explicit_work.token);
  REQUIRE(result);
  REQUIRE(*result);
  REQUIRE((**result).result);
  REQUIRE(f.browser->navigate(f.browser->state().folders.front().root));
  REQUIRE(until([&] {
    auto polled = f.browser->poll();
    return polled && f.browser->state().listing.has_value();
  }));
  CHECK(f.browser->state().listing->token.request_id == 51);
  REQUIRE(f.worker->allocate_request_id() == 52);
}
TEST_CASE("Browser lifecycle revokes its grants without cancelling unrelated "
          "same-session Ops") {
  Fixture f;
  f.grant();
  auto lease = f.factory->lease.lock();
  REQUIRE(lease);
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.source->block = gate;
  auto allocated = f.worker->allocate_request_id();
  REQUIRE(allocated);
  const auto work = ops_work(*allocated);
  REQUIRE(f.worker->submit(f.source, work));
  REQUIRE(gate->await());
  SECTION("activate another session") {
    bool changed{};
    returns_before_release(
        [&] {
          changed = f.browser->activate_session(id<domain::SessionId>("next"))
                        .has_value();
        },
        gate);
    REQUIRE(changed);
    CHECK(f.browser->state().session_id == id<domain::SessionId>("next"));
  }
  SECTION("deactivate") {
    bool ended{};
    returns_before_release(
        [&] { ended = f.browser->deactivate_session().has_value(); }, gate);
    REQUIRE(ended);
    CHECK_FALSE(f.browser->state().session_id);
  }
  SECTION("destroy browser") {
    returns_before_release([&] { f.browser.reset(); }, gate);
    CHECK_FALSE(f.browser);
  }
  CHECK(lease->revoked.load());
  auto pending = f.worker->poll(work.token);
  REQUIRE(pending);
  CHECK_FALSE(*pending);
  CHECK(f.worker->occupied_slots() == 1);
  gate->release();
  f.finish_ops(work.token);
}
TEST_CASE("Failed browser activation preserves prior session and unrelated "
          "observation") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.source->block = gate;
  REQUIRE(f.worker->submit(f.source, ops_work(10)));
  REQUIRE(gate->await());
  // Id accepts non-ASCII bytes; the session authority validator rejects unsafe
  // UTF-8.
  auto invalid = id<domain::SessionId>(std::string(1, static_cast<char>(0xff)));
  REQUIRE_FALSE(f.browser->activate_session(invalid));
  CHECK(f.browser->state().session_id == ops_spec().session_id);
  auto pending = f.worker->poll(ops_work(10).token);
  REQUIRE(pending);
  CHECK_FALSE(*pending);
  gate->release();
  f.finish_ops(ops_work(10).token);
}
TEST_CASE("Application invalidation stales both controllers without freeing "
          "blocked physical slots") {
  Fixture f;
  auto grant = std::make_shared<Gate>();
  auto observe = std::make_shared<Gate>();
  Release release_grant{grant};
  Release release_observe{observe};
  f.factory->block = grant;
  f.source->block = observe;
  REQUIRE(f.browser->add_folder("/private/folder"));
  REQUIRE(grant->await());
  auto next = f.worker->allocate_request_id();
  REQUIRE(next);
  const auto work = ops_work(*next);
  REQUIRE(f.worker->submit(f.source, work));
  REQUIRE(observe->await());
  bool invalidated{};
  returns_before_release_both(
      [&] {
        invalidated =
            f.worker->invalidate_session(ops_spec().session_id).has_value();
      },
      grant, observe);
  REQUIRE(invalidated);
  CHECK_FALSE(f.worker->poll(work.token));
  CHECK_FALSE(f.browser->poll());
  CHECK(f.browser->state().folders.empty());
  CHECK(f.worker->occupied_slots() == 2);
  auto busy = f.worker->submit(f.source, ops_work(*next + 1));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  REQUIRE(f.browser->deactivate_session());
  grant->release();
  observe->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  CHECK_FALSE(f.worker->poll(work.token));
}
TEST_CASE("Shared source convenience construction remains usable without "
          "injected ownership") {
  auto factory = std::make_shared<Factory>();
  auto created = Browser::create(factory);
  REQUIRE(created);
  auto browser = std::move(*created);
  REQUIRE(browser->activate_session(ops_spec().session_id));
  REQUIRE(browser->add_folder("/private/folder"));
  REQUIRE(until([&] {
    auto result = browser->poll();
    return result && !browser->state().granting;
  }));
  REQUIRE(browser->state().folders.size() == 1);
  CHECK(factory->last_request == 1);
}
