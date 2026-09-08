#include <catch2/catch_test_macros.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/local_source_worker.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
using Code = runtime::LocalSourceWorkerErrorCode;
using SourceCode = domain::LocalSourceErrorCode;

auto token(std::uint64_t request = 1) -> runtime::LocalSourceRequestToken {
  return {domain::SessionId::from("session").value(),
          {1, std::string(64, 'a')},
          1,
          request,
          1};
}
auto request(std::uint64_t value = 1) -> runtime::LocalListRequest {
  return {token(value), "", {}};
}
auto source() -> domain::LocalSourceIdentity {
  constexpr std::string_view text = "hello";
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {token().root, "notes.txt", {"sha256", hash.finish(), text.size()}};
}

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{};
  bool released{};
  auto arrive_and_wait() -> void {
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
  }
  auto await_entry() -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 3s, [&] { return entered; });
  }
  auto release() -> void {
    {
      const std::lock_guard lock{mutex};
      released = true;
    }
    changed.notify_all();
  }
};
struct Release {
  std::shared_ptr<Gate> gate;
  ~Release() { gate->release(); }
};

class Reader final : public runtime::LocalSourceReader {
 public:
  enum class Behavior {
    valid,
    foreign,
    malformed,
    failed,
    threw,
    invalid_error
  };
  bool pinned{true};
  Behavior behavior{Behavior::valid};
  std::shared_ptr<Gate> blocked_read;
  std::shared_ptr<Gate> blocked_callback;
  std::atomic<unsigned> calls{};
  std::atomic<bool> stop_observed{};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return pinned;
  }
  auto list(runtime::LocalListRequest value, std::stop_token stop)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    ++calls;
    std::optional<std::stop_callback<std::function<void()>>> callback;
    if (blocked_callback)
      callback.emplace(stop,
                       [gate = blocked_callback] { gate->arrive_and_wait(); });
    if (blocked_read) blocked_read->arrive_and_wait();
    stop_observed = stop.stop_requested();
    if (behavior == Behavior::threw)
      throw std::runtime_error("private failure");
    if (behavior == Behavior::failed)
      return std::unexpected(domain::LocalSourceError{
          SourceCode::unavailable, "private source /credentials/token"});
    if (behavior == Behavior::invalid_error)
      return std::unexpected(domain::LocalSourceError{SourceCode::io_failure,
                                                      std::string(2000, 'x')});
    runtime::LocalListResult result{
        value.token,
        value.directory,
        {{"notes.txt", runtime::LocalEntryKind::regular_file}},
        1,
        runtime::LocalListingState::complete};
    if (behavior == Behavior::foreign) ++result.token.lease_generation;
    if (behavior == Behavior::malformed) result.scanned_entries = 0;
    return result;
  }
  auto preview(runtime::LocalPreviewRequest value, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    ++calls;
    return runtime::LocalPreviewResult{
        value.token,
        value.relative_path,
        5,
        behavior == Behavior::malformed ? "changed" : "hello",
        runtime::LocalPreviewState::complete,
        source()};
  }
  auto revalidate(runtime::LocalRevalidateRequest value, std::stop_token)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    ++calls;
    return runtime::LocalReadResult{value.token, source(),
                                    behavior == Behavior::malformed ? "changed"
                                                                    : "hello"};
  }
};

class UnprovenReader final : public runtime::LocalSourceReader {
 public:
  Reader reader;
  auto list(runtime::LocalListRequest value, std::stop_token stop)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    return reader.list(std::move(value), stop);
  }
  auto preview(runtime::LocalPreviewRequest value, std::stop_token stop)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    return reader.preview(std::move(value), stop);
  }
  auto revalidate(runtime::LocalRevalidateRequest value, std::stop_token stop)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    return reader.revalidate(std::move(value), stop);
  }
};

auto worker(std::size_t capacity = 1)
    -> std::unique_ptr<runtime::LocalSourceWorker> {
  auto created = runtime::LocalSourceWorker::create(capacity);
  REQUIRE(created);
  return std::move(*created);
}
template <typename Predicate> auto until(Predicate predicate) -> bool {
  const auto end = std::chrono::steady_clock::now() + 3s;
  do {
    if (predicate()) return true;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < end);
  return false;
}
auto completed(runtime::LocalSourceWorker& controller,
               const runtime::LocalSourceRequestToken& expected)
    -> runtime::LocalSourceWorkCompletion {
  std::optional<runtime::LocalSourceWorkCompletion> result;
  std::optional<runtime::LocalSourceWorkerError> failure;
  REQUIRE(until([&] {
    auto polled = controller.poll(expected);
    if (!polled) {
      failure = std::move(polled.error());
      return true;
    }
    if (*polled) result = std::move(**polled);
    return result.has_value();
  }));
  INFO((failure ? failure->message : "completed"));
  REQUIRE_FALSE(failure);
  REQUIRE(result);
  return std::move(*result);
}
} // namespace

TEST_CASE("local worker rejects invalid requests without dispatch") {
  CHECK_FALSE(runtime::LocalSourceWorker::create(0));
  CHECK_FALSE(runtime::LocalSourceWorker::create(9));
  auto controller = worker();
  auto reader = std::make_shared<Reader>();
  auto invalid = request();
  SECTION("null reader") {
    reader.reset();
  }
  SECTION("unproven reader") {
    reader->pinned = false;
  }
  SECTION("zero generation") {
    invalid.token.lease_generation = 0;
  }
  SECTION("traversal") {
    invalid.directory = "../outside";
  }
  SECTION("unbounded limits") {
    invalid.limits.maximum_list_entries = 1000000;
  }
  auto rejected = controller->submit(reader, invalid);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == Code::invalid_request);
  CHECK(controller->occupied_slots() == 0);
  if (reader) CHECK(reader->calls == 0);
}

TEST_CASE("default unproven local reader cannot dispatch work") {
  auto controller = worker();
  auto reader = std::make_shared<UnprovenReader>();
  const auto result = controller->submit(reader, request());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_request);
  CHECK(reader->reader.calls == 0);
  CHECK(controller->occupied_slots() == 0);
  CHECK(controller->ready_results() == 0);
}

TEST_CASE("cancelled blocked reads occupy capacity and discard late results") {
  auto controller = worker();
  auto reader = std::make_shared<Reader>();
  auto gate = std::make_shared<Gate>();
  reader->blocked_read = gate;
  Release release{gate};
  REQUIRE(controller->submit(reader, request()));
  REQUIRE(gate->await_entry());
  REQUIRE(controller->cancel(token()));
  CHECK_FALSE(controller->poll(token()));
  CHECK(controller->occupied_slots() == 1);
  const auto busy = controller->submit(reader, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  CHECK(reader->calls == 1);
  gate->release();
  REQUIRE(until([&] { return controller->occupied_slots() == 0; }));
  CHECK_FALSE(controller->poll(token()));
  REQUIRE(controller->submit(reader, request(2)));
  const auto result = completed(*controller, token(2));
  REQUIRE(result.result);
  CHECK(reader->calls == 2);
  CHECK_FALSE(controller->submit(reader, request(1)));
}

TEST_CASE("worker teardown does not join blocked filesystem reads or release "
          "their port") {
  auto controller = worker();
  auto reader = std::make_shared<Reader>();
  const std::weak_ptr<Reader> lifetime = reader;
  auto gate = std::make_shared<Gate>();
  reader->blocked_read = gate;
  REQUIRE(controller->submit(reader, request()));
  reader.reset();
  // Release on failed entry assertion as well as after teardown.
  Release initial_release{gate};
  REQUIRE(gate->await_entry());
  std::atomic<bool> destroyed{};
  std::jthread owner{[&] {
    controller.reset();
    destroyed = true;
  }};
  // This guard unwinds before owner.join, even if the assertion fails.
  Release release_before_join{gate};
  REQUIRE(until([&] { return destroyed.load(); }));
  CHECK_FALSE(lifetime.expired());
  gate->release();
  REQUIRE(until([&] { return lifetime.expired(); }));
}

TEST_CASE("blocking stop callbacks neither block cancellation nor escape "
          "retired capacity") {
  auto controller = worker();
  auto reader = std::make_shared<Reader>();
  auto read_gate = std::make_shared<Gate>();
  auto callback_gate = std::make_shared<Gate>();
  reader->blocked_read = read_gate;
  reader->blocked_callback = callback_gate;
  Release read_release{read_gate};
  Release callback_release{callback_gate};
  REQUIRE(controller->submit(reader, request()));
  REQUIRE(read_gate->await_entry());
  std::atomic<bool> cancelled{};
  std::jthread owner{[&] {
    const auto result = controller->cancel(token());
    cancelled = result.has_value();
  }};
  Release read_before_join{read_gate};
  Release callback_before_join{callback_gate};
  REQUIRE(until([&] { return cancelled.load(); }));
  owner.join();
  REQUIRE(callback_gate->await_entry());
  read_gate->release();
  CHECK(controller->occupied_slots() == 1);
  const auto busy = controller->submit(reader, request(2));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  std::atomic<bool> destroyed{};
  std::jthread teardown{[&] {
    controller.reset();
    destroyed = true;
  }};
  Release callback_before_teardown_join{callback_gate};
  REQUIRE(until([&] { return destroyed.load(); }));
  callback_gate->release();
}

TEST_CASE(
    "local completion lookup rejects foreign tokens and consumed identities") {
  auto controller = worker();
  auto reader = std::make_shared<Reader>();
  REQUIRE(controller->submit(reader, request()));
  auto foreign = token();
  SECTION("session") {
    foreign.session_id = domain::SessionId::from("other").value();
  }
  SECTION("root") {
    foreign.root.binding[0] = 'b';
  }
  SECTION("generation") {
    ++foreign.lease_generation;
  }
  SECTION("selection revision") {
    ++foreign.selection_revision;
  }
  SECTION("request identity") {
    ++foreign.request_id;
  }
  CHECK_FALSE(controller->poll(foreign));
  CHECK_FALSE(controller->cancel(foreign));
  REQUIRE(completed(*controller, token()).result);
  CHECK_FALSE(controller->poll(token()));
  CHECK_FALSE(controller->submit(reader, request()));
  CHECK(reader->calls == 1);
}

TEST_CASE("session invalidation discards only matching work without resetting "
          "capacity") {
  auto controller = worker(2);
  auto reader = std::make_shared<Reader>();
  auto gate = std::make_shared<Gate>();
  reader->blocked_read = gate;
  Release release{gate};
  REQUIRE(controller->submit(reader, request()));
  auto other = request(2);
  other.token.session_id = domain::SessionId::from("other").value();
  REQUIRE(controller->submit(reader, other));
  REQUIRE(gate->await_entry());
  REQUIRE(controller->invalidate_session(token().session_id));
  CHECK_FALSE(controller->poll(token()));
  CHECK(controller->occupied_slots() == 2);
  CHECK_FALSE(controller->submit(reader, request(3)));
  gate->release();
  REQUIRE(completed(*controller, other.token).result);
  REQUIRE(until([&] { return controller->occupied_slots() == 0; }));
  REQUIRE(controller->submit(reader, request(3)));
  REQUIRE(completed(*controller, token(3)).result);
}

TEST_CASE("reader failures exceptions and malformed values become bounded "
          "completion errors") {
  auto controller = worker();
  auto reader = std::make_shared<Reader>();
  SourceCode expected{SourceCode::internal_failure};
  SECTION("reader failure") {
    reader->behavior = Reader::Behavior::failed;
    expected = SourceCode::unavailable;
  }
  SECTION("exception") {
    reader->behavior = Reader::Behavior::threw;
  }
  SECTION("foreign generation") {
    reader->behavior = Reader::Behavior::foreign;
    expected = SourceCode::stale_lease;
  }
  SECTION("malformed accounting") {
    reader->behavior = Reader::Behavior::malformed;
    expected = SourceCode::resource_exhausted;
  }
  SECTION("oversized error") {
    reader->behavior = Reader::Behavior::invalid_error;
    expected = SourceCode::invalid_result;
  }
  REQUIRE(controller->submit(reader, request()));
  const auto result = completed(*controller, token());
  REQUIRE_FALSE(result.result);
  CHECK(result.result.error().code == expected);
  CHECK(result.result.error().message.size() <= 1024);
  CHECK(result.result.error().message.find("/credentials") ==
        std::string::npos);
  CHECK(result.result.error().message.find("private") == std::string::npos);
  CHECK(result.token == token());
  CHECK(controller->occupied_slots() == 0);
}

TEST_CASE("preview and exact read bytes are validated before worker delivery") {
  auto controller = worker();
  auto reader = std::make_shared<Reader>();
  reader->behavior = Reader::Behavior::malformed;
  runtime::LocalSourceWorkRequest value = runtime::LocalPreviewRequest{
      token(), "notes.txt", runtime::LocalPreviewMode::exact, {}};
  SECTION("preview") {
  }
  SECTION("exact revalidation") {
    value = runtime::LocalRevalidateRequest{token(), source(), {}};
  }
  REQUIRE(controller->submit(reader, value));
  const auto result = completed(*controller, token());
  REQUIRE_FALSE(result.result);
  CHECK(result.result.error().code == SourceCode::invalid_result);
}

TEST_CASE("all typed local operations deliver exact validated immutable values "
          "once") {
  auto controller = worker(3);
  auto reader = std::make_shared<Reader>();
  REQUIRE(controller->submit(reader, request()));
  REQUIRE(controller->submit(
      reader,
      runtime::LocalPreviewRequest{
          token(2), "notes.txt", runtime::LocalPreviewMode::exact, {}}));
  REQUIRE(controller->submit(
      reader, runtime::LocalRevalidateRequest{token(3), source(), {}}));
  REQUIRE(until([&] { return controller->ready_results() == 3; }));
  CHECK(reader->calls == 3);
  CHECK(controller->occupied_slots() == 3);
  const auto busy = controller->submit(reader, request(4));
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  const auto listing = completed(*controller, token());
  const auto preview = completed(*controller, token(2));
  const auto exact = completed(*controller, token(3));
  REQUIRE(listing.result);
  REQUIRE(preview.result);
  REQUIRE(exact.result);
  CHECK(std::get<runtime::LocalListResult>(*listing.result).entries.size() ==
        1);
  CHECK(std::get<runtime::LocalPreviewResult>(*preview.result).text == "hello");
  CHECK(std::get<runtime::LocalReadResult>(*exact.result).source == source());
  CHECK(controller->occupied_slots() == 0);
}
