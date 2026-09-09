#include "../169repositorywork/fixture.hpp"
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
using namespace repository_work_test;
TEST_CASE("Cancelled repository observation retains shared capacity and "
          "rejects foreign tokens") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  f.source->block = gate;
  Release release{gate};
  REQUIRE(f.worker->submit(f.controller, request()));
  REQUIRE(gate->await());
  auto foreign = token();
  ++foreign.session_epoch;
  CHECK_FALSE(f.worker->cancel(foreign));
  CHECK_FALSE(f.worker->poll(foreign));
  bool cancelled{};
  returns_before_release(
      [&] { cancelled = f.worker->cancel(token()).has_value(); }, gate);
  REQUIRE(cancelled);
  CHECK(f.worker->occupied_slots() == 1);
  CHECK_FALSE(f.worker->poll(token()));
  auto busy = f.worker->submit(f.controller, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  // A local read cannot bypass a retired repository job's slot either.
  auto reader = std::make_shared<folder_grant_test::Lease>();
  runtime::LocalListRequest listing{
      {token().session_id, reader->root, 1, 2, 1}, {}, {}};
  auto local_busy =
      f.worker->submit(reader, runtime::LocalSourceWorkRequest{listing});
  REQUIRE_FALSE(local_busy);
  CHECK(local_busy.error().code == Code::busy);
  gate->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
  REQUIRE(f.worker->submit(f.controller, request(2)));
  REQUIRE(f.completed(token(2)).result);
  CHECK_FALSE(f.worker->submit(f.controller, request(2)));
}
TEST_CASE("Repository worker teardown and session invalidation do not join a "
          "blocked source") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  f.source->block = gate;
  Release release{gate};
  REQUIRE(f.worker->submit(f.controller, request()));
  REQUIRE(gate->await());
  SECTION("owner destruction") {
    returns_before_release([&] { f.worker.reset(); }, gate);
    CHECK_FALSE(f.worker);
  }
  SECTION("session replacement") {
    bool invalidated{};
    returns_before_release(
        [&] {
          invalidated =
              f.worker->invalidate_session(token().session_id).has_value();
        },
        gate);
    CHECK(invalidated);
    CHECK_FALSE(f.worker->poll(token()));
    auto next = request(2);
    next.token.session_id = id<domain::SessionId>("next");
    ++next.token.session_epoch;
    auto busy = f.worker->submit(f.controller, next);
    REQUIRE_FALSE(busy);
    CHECK(busy.error().code == Code::busy);
  }
  gate->release();
}
TEST_CASE("Repository cancellation callback cannot run on owner or release "
          "occupied slot early") {
  Fixture f;
  auto read = std::make_shared<Gate>();
  auto callback = std::make_shared<Gate>();
  f.source->block = read;
  f.source->callback = callback;
  Release release_read{read};
  Release release_callback{callback};
  REQUIRE(f.worker->submit(f.controller, request()));
  REQUIRE(read->await());
  bool cancelled{};
  returns_before_release(
      [&] { cancelled = f.worker->cancel(token()).has_value(); }, callback);
  REQUIRE(cancelled);
  REQUIRE(callback->await());
  read->release();
  CHECK(f.worker->occupied_slots() == 1);
  auto busy = f.worker->submit(f.controller, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  callback->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
}
TEST_CASE("Last owning repository source is destroyed on producer before slot "
          "becomes reusable") {
  Fixture f;
  auto read = std::make_shared<Gate>();
  auto destruction_gate = std::make_shared<Gate>();
  auto destruction = std::make_shared<Destruction>();
  destruction->gate = destruction_gate;
  f.source->block = read;
  f.source->destruction = destruction;
  Release release_read{read};
  Release release_destructor{destruction_gate};
  REQUIRE(f.worker->submit(f.controller, request()));
  REQUIRE(read->await());
  f.controller.reset();
  f.source.reset();
  read->release();
  REQUIRE(destruction_gate->await());
  CHECK(destruction->thread != std::this_thread::get_id());
  CHECK_FALSE(destruction->done.load());
  CHECK(f.worker->ready_results() == 0);
  REQUIRE(f.worker->poll(token()));
  CHECK_FALSE(*f.worker->poll(token()));
  auto replacement_source = std::make_shared<Source>();
  auto replacement = runtime::RepositoryContextController::create_owned(
      replacement_source, replacement_source->snapshot.root);
  REQUIRE(replacement);
  auto busy = f.worker->submit(*replacement, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  destruction_gate->release();
  REQUIRE(until([&] { return destruction->done.load(); }));
  auto completed = f.completed();
  REQUIRE(completed.result);
  REQUIRE(f.worker->submit(*replacement, request(2)));
  REQUIRE(f.completed(token(2)).result);
}
TEST_CASE("Completed unconsumed repository values retain slots until exact "
          "one-time delivery") {
  Fixture f;
  for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
    REQUIRE(f.worker->submit(f.controller, request(sequence)));
    REQUIRE(until([&] { return f.worker->ready_results() == 1; }));
    CHECK(f.worker->ready_result(token(sequence)));
    auto foreign = token(sequence);
    ++foreign.session_epoch;
    CHECK_FALSE(f.worker->ready_result(foreign));
    CHECK_FALSE(f.worker->ready_result(token(sequence + 1)));
    auto busy = f.worker->submit(f.controller, request(sequence + 1));
    REQUIRE_FALSE(busy);
    CHECK(busy.error().code == Code::busy);
    auto completed = f.completed(token(sequence));
    REQUIRE(completed.result);
    CHECK_FALSE(f.worker->poll(token(sequence)));
    CHECK_FALSE(f.worker->ready_result(token(sequence)));
    CHECK(f.worker->occupied_slots() == 0);
  }
}
TEST_CASE("Both repository worker startup failures retain caller-owned "
          "controller without source calls") {
  for (int failed = 1; failed <= 2; ++failed) {
    Fixture f;
    const auto* original = f.controller.get();
    fail_thread = failed;
    auto result = f.worker->submit(std::move(f.controller), request());
    const auto remaining = fail_thread.exchange(0);
    INFO(failed);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::internal_failure);
    CHECK(remaining == 0);
    CHECK(f.controller.get() == original);
    CHECK(f.source->observations == 0);
    REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
    CHECK_FALSE(f.worker->submit(f.controller, request()));
    REQUIRE(f.worker->submit(f.controller, request(2)));
    REQUIRE(f.completed(token(2)).result);
  }
}
