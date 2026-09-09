#include <aiforge/runtime/local_source_grants.hpp>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
template <class Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{};
  bool released{};
  std::thread::id thread;
  auto enter() -> void {
    std::unique_lock lock{mutex};
    entered = true;
    thread = std::this_thread::get_id();
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
  }
  auto wait() -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 2s, [&] { return entered; });
  }
  auto release() -> void {
    const std::lock_guard lock{mutex};
    released = true;
    changed.notify_all();
  }
};
struct ReleaseGate {
  std::shared_ptr<Gate> gate;
  ~ReleaseGate() { gate->release(); }
};
struct Observation {
  std::shared_ptr<Gate> destruction;
  std::atomic<bool> revoked{};
  std::atomic<bool> destroyed{};
  std::atomic<int> reads{};
};
class Lease final : public runtime::LocalSourceLease {
 public:
  domain::SessionId owner;
  domain::LocalRootIdentity root{1, std::string(64, 'b')};
  std::uint64_t generation;
  std::shared_ptr<Observation> observation;
  Lease(domain::SessionId session, std::uint64_t value,
        std::shared_ptr<Observation> observed)
      : owner(std::move(session)), generation(value),
        observation(std::move(observed)) {}
  ~Lease() override {
    if (observation->destruction) observation->destruction->enter();
    observation->destroyed = true;
  }
  auto root_identity() const noexcept
      -> const domain::LocalRootIdentity& override {
    return root;
  }
  auto session_id() const noexcept -> const domain::SessionId& override {
    return owner;
  }
  auto lease_generation() const noexcept -> std::uint64_t override {
    return generation;
  }
  auto revoke() noexcept -> void override { observation->revoked = true; }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto list(runtime::LocalListRequest, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    ++observation->reads;
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::unavailable, "unavailable"});
  }
  auto preview(runtime::LocalPreviewRequest, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    ++observation->reads;
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::unavailable, "unavailable"});
  }
  auto revalidate(runtime::LocalRevalidateRequest, std::stop_token)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    ++observation->reads;
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::unavailable, "unavailable"});
  }
};
class Factory final : public runtime::LocalSourceGrantFactory {
 public:
  std::shared_ptr<Observation> observation{std::make_shared<Observation>()};
  std::shared_ptr<Gate> grant_gate;
  std::shared_ptr<Lease> external;
  bool keep_external{};
  bool wrong_root{};
  bool wrong_token{};
  bool wrong_generation{};
  bool fail{};
  bool throws{};
  bool trusted{true};
  std::atomic<int> calls{};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return trusted;
  }
  auto grant(const runtime::LocalFolderGrantRequest& request, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    ++calls;
    if (grant_gate) grant_gate->enter();
    if (throws) throw std::runtime_error{"private source details"};
    if (fail)
      return std::unexpected(domain::LocalSourceError{
          domain::LocalSourceErrorCode::io_failure, "private path"});
    auto lease = std::make_shared<Lease>(
        request.token.session_id,
        request.lease_generation + static_cast<std::uint64_t>(wrong_generation),
        observation);
    if (keep_external) external = lease;
    auto token = request.token;
    if (wrong_token) ++token.request_id;
    auto root = lease->root;
    if (wrong_root) root.binding[0] = 'a';
    return runtime::LocalFolderGrantResult{token, root, std::move(lease)};
  }
};
auto request(std::uint64_t number = 1, std::uint64_t epoch = 1,
             std::string session = "session")
    -> runtime::LocalFolderGrantRequest {
  return {{id<domain::SessionId>(session), epoch, number},
          "/private/folder",
          number,
          {}};
}
auto registry(std::size_t capacity = 1)
    -> std::shared_ptr<runtime::LocalSourceGrants> {
  auto result = runtime::LocalSourceGrants::create(capacity);
  REQUIRE(result);
  REQUIRE((*result)->activate_session(id<domain::SessionId>("session"), 1));
  return *result;
}
auto wait_empty(const std::shared_ptr<runtime::LocalSourceGrants>& grants)
    -> bool {
  const auto end = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < end) {
    if (grants->occupied_slots() == 0) return true;
    std::this_thread::sleep_for(1ms);
  }
  return grants->occupied_slots() == 0;
}
} // namespace

TEST_CASE("local grants refuse invalid capacity scope and repeated reservation "
          "identities") {
  CHECK_FALSE(runtime::LocalSourceGrants::create(0));
  CHECK_FALSE(runtime::LocalSourceGrants::create(17));
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  auto value = request();
  SECTION("foreign session") {
    value.token.session_id = id<domain::SessionId>("foreign");
  }
  SECTION("stale epoch") {
    value.token.session_epoch = 2;
  }
  SECTION("invalid generation") {
    value.lease_generation = 0;
  }
  SECTION("invalid path") {
    value.absolute_path = "relative";
  }
  CHECK_FALSE(grants->reserve(value, factory));
  CHECK(grants->occupied_slots() == 0);
  CHECK(factory->calls == 0);
}

TEST_CASE(
    "reservation capacity precedes dispatch and stale producers cannot open") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto producer = (*ticket)->factory();
  CHECK(grants->occupied_slots() == 1);
  CHECK_FALSE(grants->reserve(request(2), factory));
  REQUIRE(grants->invalidate_session(id<domain::SessionId>("session")));
  CHECK_FALSE(producer->grant(request()));
  CHECK(factory->calls == 0);
  ticket->reset();
  CHECK(wait_empty(grants));
  REQUIRE(grants->activate_session(id<domain::SessionId>("session"), 2));
  CHECK_FALSE(grants->reserve(request(1, 2), factory));
  CHECK(grants->reserve(request(2, 2), factory));
}

TEST_CASE("late grant remains reserved across session switches until "
          "background disposal") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  factory->grant_gate = std::make_shared<Gate>();
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto producer = (*ticket)->factory();
  auto outcome = std::async(std::launch::async,
                            [producer] { return producer->grant(request()); });
  const ReleaseGate release{factory->grant_gate};
  REQUIRE(factory->grant_gate->wait());
  for (std::uint64_t epoch = 2; epoch < 20; ++epoch) {
    REQUIRE(grants->activate_session(id<domain::SessionId>("next"), epoch));
    CHECK_FALSE(grants->reserve(request(epoch, epoch, "next"), factory));
    CHECK(grants->occupied_slots() == 1);
  }
  ticket->reset();
  factory->grant_gate->release();
  CHECK_FALSE(outcome.get());
  CHECK(wait_empty(grants));
  CHECK(factory->observation->destroyed);
  CHECK(factory->observation->revoked);
}

TEST_CASE(
    "forged grant outputs are retired without publication or source lookup") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  SECTION("wrong root") {
    factory->wrong_root = true;
  }
  SECTION("wrong request") {
    factory->wrong_token = true;
  }
  SECTION("wrong generation") {
    factory->wrong_generation = true;
  }
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto result = (*ticket)->factory()->grant(request());
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find("private") == std::string::npos);
  CHECK(wait_empty(grants));
  CHECK(factory->observation->destroyed);
  const auto absent = grants->resolve(id<domain::SessionId>("session"),
                                      {1, std::string(64, 'b')});
  REQUIRE(absent);
  CHECK_FALSE(*absent);
  CHECK(factory->observation->reads == 0);
}

TEST_CASE(
    "borrow release and UI teardown never destroy a gated physical lease") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  auto observed = factory->observation;
  observed->destruction = std::make_shared<Gate>();
  const ReleaseGate release{observed->destruction};
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto outcome = (*ticket)->factory()->grant(request());
  REQUIRE(outcome);
  auto accepted = (*ticket)->accept(*outcome);
  REQUIRE(accepted);
  auto resolved = grants->resolve(request().token.session_id, outcome->root);
  REQUIRE(resolved);
  REQUIRE(*resolved);
  CHECK((*resolved)->reader.get() == accepted->reader.get());
  CHECK(accepted->reader.get() == outcome->lease.get());
  REQUIRE(grants->revoke(request().token.session_id, outcome->root, 1));
  CHECK(observed->revoked);
  CHECK_FALSE(*grants->resolve(request().token.session_id, outcome->root));
  outcome->lease.reset();
  accepted->reader.reset();
  CHECK_FALSE(observed->destroyed);
  CHECK(grants->occupied_slots() == 1);
  grants.reset();
  ticket->reset();
  (*resolved)->reader.reset();
  REQUIRE(observed->destruction->wait());
  CHECK(observed->destruction->thread != std::this_thread::get_id());
  CHECK_FALSE(observed->destroyed);
  observed->destruction->release();
}

TEST_CASE(
    "stalled destruction and external raw owners retain registry capacity") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  factory->keep_external = true;
  auto observed = factory->observation;
  observed->destruction = std::make_shared<Gate>();
  const ReleaseGate release{observed->destruction};
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto outcome = (*ticket)->factory()->grant(request());
  REQUIRE(outcome);
  outcome->lease.reset();
  ticket->reset();
  CHECK(grants->occupied_slots() == 1);
  CHECK_FALSE(observed->destroyed);
  CHECK_FALSE(grants->reserve(request(2), factory));
  factory->external.reset();
  REQUIRE(observed->destruction->wait());
  CHECK(grants->occupied_slots() == 1);
  CHECK_FALSE(grants->reserve(request(2), factory));
  observed->destruction->release();
  CHECK(wait_empty(grants));
}

TEST_CASE("exact staged accept rejects stale or altered metadata and supports "
          "explicit regrant") {
  auto grants = registry(2);
  auto factory = std::make_shared<Factory>();
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto result = (*ticket)->factory()->grant(request());
  REQUIRE(result);
  SECTION("wrong root") {
    result->root.binding[0] = 'a';
    CHECK_FALSE((*ticket)->accept(*result));
  }
  SECTION("old epoch") {
    REQUIRE(grants->activate_session(request().token.session_id, 2));
    CHECK_FALSE((*ticket)->accept(*result));
  }
  SECTION("explicit new grant while old reader remains borrowed") {
    auto first = (*ticket)->accept(*result);
    REQUIRE(first);
    REQUIRE(grants->revoke(request().token.session_id, result->root, 1));
    auto second_factory = std::make_shared<Factory>();
    auto second = grants->reserve(request(2), second_factory);
    REQUIRE(second);
    auto output = (*second)->factory()->grant(request(2));
    REQUIRE(output);
    auto new_grant = (*second)->accept(*output);
    REQUIRE(new_grant);
    CHECK(new_grant->lease_generation == 2);
    CHECK(new_grant->reader.get() != first->reader.get());
    CHECK(grants->occupied_slots() == 2);
    CHECK_FALSE((*second)->factory()->grant(request(2)));
  }
}

TEST_CASE("registry factory failures cancellation and foreign dispatch never "
          "publish") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  std::stop_source stop;
  auto value = request();
  SECTION("factory error") {
    factory->fail = true;
  }
  SECTION("factory exception") {
    factory->throws = true;
  }
  SECTION("cancelled before factory") {
    stop.request_stop();
  }
  SECTION("foreign dispatch path") {
    value.absolute_path = "/other";
  }
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto result = (*ticket)->factory()->grant(value, stop.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find("private") == std::string::npos);
  ticket->reset();
  CHECK(wait_empty(grants));
}

TEST_CASE("registry rejects untrusted factories before reserving a slot") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  factory->trusted = false;
  CHECK_FALSE(grants->reserve(request(), factory));
  CHECK(grants->occupied_slots() == 0);
  CHECK(factory->calls == 0);
}

TEST_CASE("concurrent local grant lookups preserve the one physical reader "
          "identity") {
  auto grants = registry();
  auto factory = std::make_shared<Factory>();
  auto ticket = grants->reserve(request(), factory);
  REQUIRE(ticket);
  auto result = (*ticket)->factory()->grant(request());
  REQUIRE(result);
  auto active = (*ticket)->accept(*result);
  REQUIRE(active);
  const auto* reader = active->reader.get();
  std::array<std::future<bool>, 4> jobs;
  for (auto& job : jobs) {
    job = std::async(std::launch::async, [grants, root = result->root, reader] {
      for (int attempt = 0; attempt < 64; ++attempt) {
        auto lookup = grants->resolve(id<domain::SessionId>("session"), root);
        if (!lookup || !*lookup || (*lookup)->reader.get() != reader)
          return false;
      }
      return true;
    });
  }
  for (auto& job : jobs)
    CHECK(job.get());
  CHECK_FALSE(*grants->resolve(id<domain::SessionId>("foreign"), result->root));
  CHECK_FALSE(
      grants->revoke(id<domain::SessionId>("session"), result->root, 2));
  CHECK(*grants->resolve(id<domain::SessionId>("session"), result->root));
  CHECK(factory->calls == 1);
  CHECK(factory->observation->reads == 0);
}

TEST_CASE("duplicate active roots cannot replace a usable grant") {
  auto grants = registry(2);
  auto factory = std::make_shared<Factory>();
  auto first = grants->reserve(request(), factory);
  REQUIRE(first);
  auto original = (*first)->factory()->grant(request());
  REQUIRE(original);
  auto active = (*first)->accept(*original);
  REQUIRE(active);
  auto other_factory = std::make_shared<Factory>();
  auto second = grants->reserve(request(2), other_factory);
  REQUIRE(second);
  auto duplicate = (*second)->factory()->grant(request(2));
  REQUIRE(duplicate);
  CHECK_FALSE((*second)->accept(*duplicate));
  CHECK((*grants->resolve(request().token.session_id, original->root))
            ->reader.get() == active->reader.get());
  CHECK_FALSE(factory->observation->revoked);
  CHECK(other_factory->observation->revoked);
}
