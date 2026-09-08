#include "../159foldergrant/fixture.hpp"
#include <aiforge/runtime/local_source_worker.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <pthread.h>
#include <stdexcept>

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
using namespace folder_grant_test;
using Code = runtime::LocalSourceWorkerErrorCode;
class Factory final : public runtime::LocalSourceGrantFactory {
 public:
  enum class Mode { valid, failed, threw, foreign, malformed, unpinned };
  Mode mode{Mode::valid};
  bool pinned{true};
  std::shared_ptr<Gate> block, callback;
  std::shared_ptr<Destruction> destruction = std::make_shared<Destruction>();
  std::atomic<unsigned> calls{};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return pinned;
  }
  auto grant(const runtime::LocalFolderGrantRequest& value,
             std::stop_token stop)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    ++calls;
    std::optional<std::stop_callback<std::function<void()>>> stopped;
    if (callback) stopped.emplace(stop, [gate = callback] { gate->wait(); });
    if (block) block->wait();
    if (mode == Mode::threw)
      throw std::runtime_error("secret /private/factory");
    if (mode == Mode::failed)
      return std::unexpected(
          domain::LocalSourceError{domain::LocalSourceErrorCode::unavailable,
                                   "secret /private/factory"});
    auto lease = std::make_shared<Lease>();
    lease->destruction = destruction;
    lease->session = value.token.session_id;
    lease->generation = value.lease_generation;
    if (mode == Mode::unpinned) lease->pinned = false;
    runtime::LocalFolderGrantResult result{value.token, lease->root,
                                           std::move(lease)};
    if (mode == Mode::foreign) ++result.token.session_epoch;
    if (mode == Mode::malformed) result.root.binding = "bad";
    return result;
  }
};
// A regression that joins or invokes a blocked destructor must fail rather
// than hang: release the gate before the owner activity joins on assertion
// unwind.
template <typename Action>
auto returns_before_release(Action action, const std::shared_ptr<Gate>& gate)
    -> void {
  std::atomic<bool> returned{};
  std::jthread owner{[&] {
    action();
    returned = true;
  }};
  Release release_before_join{gate};
  REQUIRE(until([&] { return returned.load(); }));
  owner.join();
  release_before_join.enabled = false;
}
auto worker() -> std::unique_ptr<runtime::LocalSourceWorker> {
  auto result = runtime::LocalSourceWorker::create(1);
  REQUIRE(result);
  return std::move(*result);
}
auto completion(runtime::LocalSourceWorker& worker,
                const runtime::LocalFolderGrantToken& token)
    -> runtime::LocalFolderGrantCompletion {
  REQUIRE(until([&] { return worker.ready_results() == 1; }));
  auto result = worker.poll(token);
  REQUIRE(result);
  REQUIRE(*result);
  return std::move(**result);
}
} // namespace
TEST_CASE(
    "folder worker validates requests before factory authority or allocation") {
  auto controller = worker();
  auto factory = std::make_shared<Factory>();
  auto value = request();
  SECTION("null") {
    factory.reset();
  }
  SECTION("unpinned") {
    factory->pinned = false;
  }
  SECTION("epoch") {
    value.token.session_epoch = 0;
  }
  SECTION("path bound") {
    value.absolute_path = "/" + std::string(4096, 'x');
  }
  SECTION("limits") {
    value.limits.maximum_depth = 0;
  }
  auto result = controller->submit(factory, value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_request);
  CHECK(controller->occupied_slots() == 0);
  if (factory) CHECK(factory->calls == 0);
}
TEST_CASE("cancelled blocking grants retain the shared slot and discard late "
          "leases") {
  auto controller = worker();
  auto factory = std::make_shared<Factory>();
  factory->block = std::make_shared<Gate>();
  Release release{factory->block};
  REQUIRE(controller->submit(factory, request()));
  REQUIRE(factory->block->await());
  REQUIRE(controller->invalidate_session(request().token.session_id));
  REQUIRE_FALSE(controller->poll(request().token));
  auto reader = std::make_shared<Lease>();
  runtime::LocalListRequest listing{
      {request().token.session_id, reader->root, 1, 2, 1}, "", {}};
  const auto busy = controller->submit(reader, listing);
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  CHECK(controller->occupied_slots() == 1);
  factory->block->release();
  REQUIRE(until([&] { return controller->occupied_slots() == 0; }));
  REQUIRE(factory->destruction->done);
  CHECK(factory->destruction->thread != std::this_thread::get_id());
  REQUIRE(controller->submit(reader, listing));
  REQUIRE(until([&] { return controller->ready_results() == 1; }));
  REQUIRE(controller->poll(listing.token).value());
  REQUIRE_FALSE(controller->submit(factory, request(2)));
}
TEST_CASE("completed unclaimed grant cleanup never blocks cancel or teardown") {
  auto controller = worker();
  auto factory = std::make_shared<Factory>();
  factory->destruction->gate = std::make_shared<Gate>();
  Release release{factory->destruction->gate};
  REQUIRE(controller->submit(factory, request()));
  REQUIRE(until([&] { return controller->ready_results() == 1; }));
  CHECK(controller->occupied_slots() == 1);
  REQUIRE_FALSE(controller->submit(factory, request(2)));
  SECTION("cancel") {
    bool cancelled{};
    returns_before_release(
        [&] { cancelled = controller->cancel(request().token).has_value(); },
        factory->destruction->gate);
    REQUIRE(cancelled);
    REQUIRE(factory->destruction->gate->await());
    CHECK(controller->occupied_slots() == 1);
    const auto busy = controller->submit(factory, request(2));
    REQUIRE_FALSE(busy);
    CHECK(busy.error().code == Code::busy);
    REQUIRE_FALSE(controller->poll(request().token));
    factory->destruction->gate->release();
    REQUIRE(until([&] { return controller->occupied_slots() == 0; }));
  }
  SECTION("teardown") {
    returns_before_release([&] { controller.reset(); },
                           factory->destruction->gate);
    REQUIRE(factory->destruction->gate->await());
    CHECK_FALSE(factory->destruction->done);
    factory->destruction->gate->release();
    REQUIRE(until([&] { return factory->destruction->done.load(); }));
  }
  CHECK(factory->destruction->thread != std::this_thread::get_id());
}
TEST_CASE("blocking factory teardown returns immediately and late cleanup "
          "stays off owner") {
  auto controller = worker();
  auto factory = std::make_shared<Factory>();
  factory->block = std::make_shared<Gate>();
  Release release{factory->block};
  REQUIRE(controller->submit(factory, request()));
  REQUIRE(factory->block->await());
  returns_before_release([&] { controller.reset(); }, factory->block);
  CHECK_FALSE(factory->destruction->done);
  factory->block->release();
  REQUIRE(until([&] { return factory->destruction->done.load(); }));
  CHECK(factory->destruction->thread != std::this_thread::get_id());
}
TEST_CASE("grant cancellation callback stalls only bounded relay activity") {
  auto controller = worker();
  auto factory = std::make_shared<Factory>();
  factory->block = std::make_shared<Gate>();
  Release release{factory->block};
  factory->callback = std::make_shared<Gate>();
  Release release_callback{factory->callback};
  REQUIRE(controller->submit(factory, request()));
  REQUIRE(factory->block->await());
  bool cancelled{};
  returns_before_release(
      [&] { cancelled = controller->cancel(request().token).has_value(); },
      factory->callback);
  REQUIRE(cancelled);
  REQUIRE(factory->callback->await());
  factory->block->release();
  CHECK(controller->occupied_slots() == 1);
  REQUIRE_FALSE(controller->submit(factory, request(2)));
  controller.reset();
  factory->callback->release();
  REQUIRE(until([&] { return factory->destruction->done.load(); }));
}
TEST_CASE(
    "grant completion rejects forged exact token and canonicalizes failures") {
  auto controller = worker();
  auto factory = std::make_shared<Factory>();
  SECTION("failure") {
    factory->mode = Factory::Mode::failed;
  }
  SECTION("exception") {
    factory->mode = Factory::Mode::threw;
  }
  SECTION("foreign result") {
    factory->mode = Factory::Mode::foreign;
  }
  SECTION("malformed result") {
    factory->mode = Factory::Mode::malformed;
  }
  SECTION("unpinned lease") {
    factory->mode = Factory::Mode::unpinned;
  }
  REQUIRE(controller->submit(factory, request()));
  auto foreign = request().token;
  ++foreign.session_epoch;
  REQUIRE_FALSE(controller->poll(foreign));
  REQUIRE_FALSE(controller->cancel(foreign));
  auto other = request().token;
  other.session_id = domain::SessionId::from("other").value();
  REQUIRE_FALSE(controller->poll(other));
  auto result = completion(*controller, request().token);
  REQUIRE_FALSE(result.result);
  CHECK(result.result.error().message.find("secret") == std::string::npos);
  CHECK(result.result.error().message.find("/private") == std::string::npos);
  REQUIRE(until([&] { return controller->occupied_slots() == 0; }));
}
TEST_CASE("first and second thread creation failures preserve caller ownership "
          "and retire slots") {
  auto controller = worker();
  auto implementation = std::make_shared<Factory>();
  std::shared_ptr<runtime::LocalSourceGrantFactory> factory = implementation;
  int failure = 1;
  SECTION("first activity") {
  }
  SECTION("second activity") {
    failure = 2;
  }
  struct Reset {
    ~Reset() { fail_thread = 0; }
  } reset;
  fail_thread = failure;
  const auto refused = controller->submit(std::move(factory), request());
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == Code::internal_failure);
  REQUIRE(factory);
  CHECK(implementation->calls == 0);
  REQUIRE(until([&] { return controller->occupied_slots() == 0; }));
  REQUIRE_FALSE(controller->submit(factory, request()));
  REQUIRE(controller->submit(factory, request(2)));
  const auto result = completion(*controller, request(2).token);
  REQUIRE(result.result);
}
TEST_CASE("successful grant transfers lease once across repeated bounded "
          "capacity reuse") {
  auto controller = worker();
  auto factory = std::make_shared<Factory>();
  for (std::uint64_t index = 1; index <= 8; ++index) {
    const auto value = request(index);
    REQUIRE(controller->submit(factory, value));
    auto result = completion(*controller, value.token);
    REQUIRE(result.result);
    REQUIRE(runtime::validate_local_folder_grant_result(value, *result.result));
    REQUIRE_FALSE(controller->poll(value.token));
    REQUIRE(until([&] { return controller->occupied_slots() == 0; }));
    CHECK_FALSE(result.result->lease->root_identity().binding.empty());
    result.result->lease->revoke();
  }
}
