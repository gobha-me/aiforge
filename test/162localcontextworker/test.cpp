#include "../159foldergrant/fixture.hpp"
#include <aiforge/runtime/local_source_worker.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>

namespace {
using namespace folder_grant_test;
using Code = runtime::LocalSourceWorkerErrorCode;
using ContextCode = domain::LocalContextErrorCode;
auto source(std::string path = "notes.txt") -> domain::LocalSourceIdentity {
  return {{1, std::string(64, 'a')},
          std::move(path),
          {"sha256",
           "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
           5}};
}
auto context_token(std::uint64_t request_id = 1)
    -> runtime::LocalContextWorkToken {
  return {request().token.session_id, 2, request_id, 3};
}
auto preparation(std::uint64_t request_id = 1)
    -> runtime::LocalContextWorkRequest {
  return {context_token(request_id),
          runtime::LocalContextRequest{
              request().token.session_id, 3, {source()}, 9}};
}
class Reader final : public runtime::LocalSourceReader {
 public:
  enum class Mode { valid, changed, foreign, failed };
  Mode mode{Mode::valid};
  std::shared_ptr<Gate> block, callback;
  std::atomic<unsigned> calls{};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto list(runtime::LocalListRequest value, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    return runtime::LocalListResult{value.token,
                                    value.directory,
                                    {},
                                    0,
                                    runtime::LocalListingState::complete};
  }
  auto preview(runtime::LocalPreviewRequest, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::unavailable, "unused"});
  }
  auto revalidate(runtime::LocalRevalidateRequest value, std::stop_token stop)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    ++calls;
    std::optional<std::stop_callback<std::function<void()>>> stopped;
    if (callback) stopped.emplace(stop, [gate = callback] { gate->wait(); });
    if (block) block->wait();
    if (mode == Mode::failed)
      return std::unexpected(domain::LocalSourceError{
          domain::LocalSourceErrorCode::io_failure, "secret /private/source"});
    auto result =
        runtime::LocalReadResult{value.token, value.expected_source, "hello"};
    if (mode == Mode::changed) result.text = "jello";
    if (mode == Mode::foreign) ++result.token.selection_revision;
    return result;
  }
};
class Resolver final : public runtime::LocalContextGrantResolver {
 public:
  std::shared_ptr<Reader> reader = std::make_shared<Reader>();
  std::shared_ptr<Destruction> destruction;
  std::shared_ptr<Gate> block;
  std::atomic<unsigned> calls{};
  bool absent{}, threw{};
  ~Resolver() override {
    if (destruction) {
      destruction->thread = std::this_thread::get_id();
      if (destruction->gate) destruction->gate->wait();
      destruction->done = true;
    }
  }
  auto resolve(const domain::SessionId& session,
               const domain::LocalRootIdentity& root, std::stop_token)
      -> std::expected<std::optional<runtime::LocalContextGrant>,
                       domain::LocalSourceError> override {
    ++calls;
    if (block) block->wait();
    if (threw) throw std::runtime_error("secret /private/resolver");
    if (absent) return std::nullopt;
    return runtime::LocalContextGrant{session, root, 1, reader};
  }
};
struct Fixture {
  std::shared_ptr<Resolver> resolver = std::make_shared<Resolver>();
  std::shared_ptr<runtime::LocalContextController> controller =
      std::make_shared<runtime::LocalContextController>(resolver);
  std::unique_ptr<runtime::LocalSourceWorker> worker =
      runtime::LocalSourceWorker::create(1).value();
  auto completed(runtime::LocalContextWorkToken token = context_token())
      -> runtime::LocalContextWorkCompletion {
    REQUIRE(until([&] { return worker->ready_results() == 1; }));
    auto result = worker->poll(token);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
  }
  auto original() -> domain::LocalContextAdmission {
    auto prepared = controller->prepare(
        {request().token.session_id, 3, {source(), source("omitted.txt")}, 9});
    REQUIRE(prepared);
    auto admission = prepared->admission;
    admission.capacity = {100, 10, 0};
    admission.evidence.back().decision =
        domain::LocalContextDecision::omitted_budget;
    REQUIRE(domain::seal_local_context_admission(admission));
    resolver->calls = 0;
    resolver->reader->calls = 0;
    return admission;
  }
};
template <typename Action>
auto returns_before_release(Action action, const std::shared_ptr<Gate>& gate)
    -> void {
  std::atomic<bool> returned{};
  std::jthread owner{[&] {
    action();
    returned = true;
  }};
  Release release{gate};
  REQUIRE(until([&] { return returned.load(); }));
  owner.join();
  release.enabled = false;
}
} // namespace
TEST_CASE(
    "context worker rejects invalid scope bounds and sealed proof before IO") {
  Fixture fixture;
  auto value = preparation();
  SECTION("null controller") {
    fixture.controller.reset();
  }
  SECTION("zero epoch") {
    value.token.session_epoch = 0;
  }
  SECTION("zero request") {
    value.token.request_id = 0;
  }
  SECTION("zero selection revision") {
    value.token.selection_revision = 0;
  }
  SECTION("foreign session") {
    value.token.session_id = domain::SessionId::from("other").value();
  }
  SECTION("foreign revision") {
    ++value.token.selection_revision;
  }
  SECTION("zero order") {
    std::get<runtime::LocalContextRequest>(value.operation).first_order = 0;
  }
  SECTION("too many sources") {
    std::get<runtime::LocalContextRequest>(value.operation)
        .sources.resize(65, source());
  }
  SECTION("order overflow") {
    auto& operation = std::get<runtime::LocalContextRequest>(value.operation);
    operation.first_order = std::numeric_limits<std::uint64_t>::max();
    operation.sources.push_back(source("second.txt"));
  }
  SECTION("unsealed original") {
    auto original = fixture.original();
    original.admission_digest.reset();
    value.operation = original;
  }
  SECTION("changed sealed original") {
    auto original = fixture.original();
    ++original.evidence.front().order;
    value.operation = original;
  }
  const auto result = fixture.worker->submit(fixture.controller, value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_request);
  CHECK(fixture.resolver->calls == 0);
  CHECK(fixture.resolver->reader->calls == 0);
  CHECK(fixture.worker->occupied_slots() == 0);
}
TEST_CASE("context jobs share capacity and cancel stale selection without new "
          "authority") {
  Fixture fixture;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  fixture.resolver->reader->block = gate;
  REQUIRE(fixture.worker->submit(fixture.controller, preparation()));
  REQUIRE(gate->await());
  auto foreign = context_token();
  ++foreign.selection_revision;
  REQUIRE_FALSE(fixture.worker->poll(foreign));
  REQUIRE_FALSE(fixture.worker->cancel(foreign));
  foreign = context_token();
  ++foreign.session_epoch;
  REQUIRE_FALSE(fixture.worker->poll(foreign));
  REQUIRE(fixture.worker->invalidate_session(context_token().session_id));
  REQUIRE_FALSE(fixture.worker->poll(context_token()));
  runtime::LocalListRequest listing{
      {context_token().session_id, source().root, 1, 2, 3}, "", {}};
  const auto busy = fixture.worker->submit(fixture.resolver->reader, listing);
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code == Code::busy);
  CHECK(fixture.worker->occupied_slots() == 1);
  gate->release();
  REQUIRE(until([&] { return fixture.worker->occupied_slots() == 0; }));
  REQUIRE(fixture.worker->submit(fixture.resolver->reader, listing));
  REQUIRE(until([&] { return fixture.worker->ready_results() == 1; }));
  REQUIRE(fixture.worker->poll(listing.token).value());
  REQUIRE_FALSE(fixture.worker->submit(fixture.controller, preparation(2)));
}
TEST_CASE(
    "context worker releases owned resolver off owner before slot retirement") {
  Fixture fixture;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  fixture.resolver->block = gate;
  auto destruction = std::make_shared<Destruction>();
  destruction->gate = std::make_shared<Gate>();
  Release cleanup{destruction->gate};
  fixture.resolver->destruction = destruction;
  REQUIRE(fixture.worker->submit(fixture.controller, preparation()));
  REQUIRE(gate->await());
  fixture.controller.reset();
  fixture.resolver.reset();
  REQUIRE(fixture.worker->cancel(context_token()));
  gate->release();
  REQUIRE(destruction->gate->await());
  CHECK(fixture.worker->occupied_slots() == 1);
  CHECK(fixture.worker->ready_results() == 0);
  CHECK(destruction->thread != std::this_thread::get_id());
  returns_before_release([&] { fixture.worker.reset(); }, destruction->gate);
  destruction->gate->release();
  REQUIRE(until([&] { return destruction->done.load(); }));
}
TEST_CASE("context worker teardown and blocked stop callbacks stay off owner") {
  Fixture fixture;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  fixture.resolver->reader->block = gate;
  auto callback = std::make_shared<Gate>();
  Release release_callback{callback};
  fixture.resolver->reader->callback = callback;
  REQUIRE(fixture.worker->submit(fixture.controller, preparation()));
  REQUIRE(gate->await());
  bool cancelled{};
  returns_before_release(
      [&] { cancelled = fixture.worker->cancel(context_token()).has_value(); },
      callback);
  REQUIRE(cancelled);
  REQUIRE(callback->await());
  gate->release();
  CHECK(fixture.worker->occupied_slots() == 1);
  returns_before_release([&] { fixture.worker.reset(); }, callback);
  callback->release();
}
TEST_CASE("context worker delivers canonical errors for absent changed and "
          "malformed sources") {
  Fixture fixture;
  SECTION("absent lease") {
    fixture.resolver->absent = true;
  }
  SECTION("throwing resolver") {
    fixture.resolver->threw = true;
  }
  SECTION("changed bytes") {
    fixture.resolver->reader->mode = Reader::Mode::changed;
  }
  SECTION("foreign read token") {
    fixture.resolver->reader->mode = Reader::Mode::foreign;
  }
  SECTION("reader diagnostic") {
    fixture.resolver->reader->mode = Reader::Mode::failed;
  }
  REQUIRE(fixture.worker->submit(fixture.controller, preparation()));
  auto result = fixture.completed();
  REQUIRE_FALSE(result.result);
  CHECK(result.result.error().message.find("secret") == std::string::npos);
  CHECK(result.result.error().message.find("/private") == std::string::npos);
  CHECK(fixture.worker->occupied_slots() == 0);
}
TEST_CASE("context recovery preserves exact original omissions sources order "
          "capacity and seal") {
  Fixture fixture;
  const auto original = fixture.original();
  const auto token = context_token();
  REQUIRE(fixture.worker->submit(fixture.controller, {token, original}));
  auto result = fixture.completed();
  REQUIRE(result.result);
  CHECK(result.result->admission == original);
  REQUIRE(result.result->candidates.size() == original.evidence.size());
  CHECK(result.result->admission.evidence.back().decision ==
        domain::LocalContextDecision::omitted_budget);
  for (std::size_t index = 0; index < original.evidence.size(); ++index) {
    const auto& actual = result.result->candidates[index].content;
    const auto& ref = original.evidence[index];
    CHECK(actual.entry_id == ref.entry_id);
    CHECK(actual.order == ref.order);
    CHECK(actual.message.message_id == ref.message_id);
    CHECK(actual.provenance.source_id == ref.source_id);
    CHECK(actual.message.content ==
          std::vector<domain::ContentBlock>{domain::TextBlock{"hello"}});
  }
  CHECK(fixture.resolver->reader->calls == 2);
  CHECK(fixture.resolver->calls == 2);
  REQUIRE_FALSE(fixture.worker->poll(token));
}
TEST_CASE("context prepare returns unsealed owning candidates and holds "
          "unconsumed capacity") {
  Fixture fixture;
  REQUIRE(fixture.worker->submit(fixture.controller, preparation()));
  REQUIRE(until([&] { return fixture.worker->ready_results() == 1; }));
  CHECK(fixture.worker->occupied_slots() == 1);
  REQUIRE_FALSE(fixture.worker->submit(fixture.controller, preparation(2)));
  auto result = fixture.completed();
  REQUIRE(result.result);
  CHECK(fixture.worker->occupied_slots() == 0);
  CHECK(result.token == context_token());
  CHECK_FALSE(result.result->admission.admission_digest);
  CHECK(result.result->admission.capacity == domain::ContextCapacity{});
  REQUIRE(result.result->candidates.size() == 1);
  CHECK(result.result->candidates.front().content.order == 9);
  fixture.controller.reset();
  fixture.resolver.reset();
  CHECK(result.result->candidates.front().content.message.content ==
        std::vector<domain::ContentBlock>{domain::TextBlock{"hello"}});
}
