#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/testing/scripted_backend.hpp>

namespace {

using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto request(std::string model = "model") -> backend::BackendRequest {
  const domain::ContextCapacity capacity{1024, 128, 0};
  const domain::ConstructedContext context{
      {domain::ContextEntry{make_id<domain::ContextEntryId>("context-runtime"),
                            domain::ContextEntryKind::instruction,
                            domain::InstructionLayer::application_runtime,
                            domain::Message{make_id<domain::MessageId>("runtime"),
                                            domain::Role::system,
                                            {domain::TextBlock{"runtime contract"}},
                                            std::nullopt},
                            {make_id<domain::ContextSourceId>("runtime-source"),
                             std::nullopt, std::nullopt},
                            0,
                            1,
                            1},
       domain::ContextEntry{make_id<domain::ContextEntryId>("context-user"),
                            domain::ContextEntryKind::conversation,
                            std::nullopt,
                            domain::Message{make_id<domain::MessageId>("user"),
                                            domain::Role::user,
                                            {domain::TextBlock{"hello"}},
                                            std::nullopt},
                            {make_id<domain::ContextSourceId>("event-user"),
                             std::nullopt, std::nullopt},
                            0,
                            2,
                            1}},
      {{make_id<domain::ContextEntryId>("context-runtime"),
        domain::ContextDecision::admitted, std::nullopt},
       {make_id<domain::ContextEntryId>("context-user"),
        domain::ContextDecision::admitted, std::nullopt}},
      capacity,
      2};
  return backend::BackendRequest{
      make_id<domain::InferenceId>("inference"),
      make_id<domain::MessageId>("assistant"),
      make_id<domain::ModelId>(std::move(model)),
      context,
      {},
      {0.25, 128, 42, {}},
  };
}

auto event_step(backend::BackendEvent event) -> testing::ScriptedStep {
  return testing::ScriptedStep{std::move(event)};
}

}  // namespace

TEST_CASE("scripted backend rejects mismatches without consuming the exchange",
          "[backend][failure]") {
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      request(), testing::StreamScript{{testing::EndOfStream{}}}}}};

  const auto mismatch = fake.start(request("other-model"), {});
  REQUIRE_FALSE(mismatch);
  REQUIRE(mismatch.error().kind == backend::BackendErrorKind::script_mismatch);
  REQUIRE(fake.remaining_exchanges() == 1);
  REQUIRE(fake.recorded_requests().size() == 1);

  REQUIRE(fake.start(request(), {}));
  REQUIRE(fake.remaining_exchanges() == 0);
  REQUIRE(fake.recorded_requests().size() == 2);
}

TEST_CASE("scripted backend reports exhausted and startup-failure scripts",
          "[backend][failure]") {
  testing::ScriptedBackend empty{{}};
  const auto exhausted = empty.start(request(), {});
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().kind == backend::BackendErrorKind::script_exhausted);

  const backend::BackendError unavailable{backend::BackendErrorKind::unavailable,
                                          "provider unavailable", true, 503};
  testing::ScriptedBackend failing{{testing::ScriptedExchange{request(), unavailable}}};
  const auto failed = failing.start(request(), {});
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error() == unavailable);
}

TEST_CASE("scripted stream preserves event order and clean end-of-stream", "[backend]") {
  const auto message = make_id<domain::MessageId>("assistant");
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      request(),
      testing::StreamScript{{
          event_step(backend::ResponseStarted{"response"}),
          event_step(backend::ContentDelta{message, domain::TextBlock{"hello"}}),
          event_step(backend::UsageObserved{{1, 2, 0, 0}}),
          event_step(backend::ResponseFinished{domain::FinishReason::stop}),
          testing::EndOfStream{},
      }}}}};

  auto started = fake.start(request(), {});
  REQUIRE(started);
  auto& stream = **started;

  const auto first = stream.next({});
  const auto second = stream.next({});
  const auto third = stream.next({});
  const auto fourth = stream.next({});
  const auto end = stream.next({});

  REQUIRE(first);
  REQUIRE(*first);
  REQUIRE(std::holds_alternative<backend::ResponseStarted>(**first));
  REQUIRE(second);
  REQUIRE(*second);
  REQUIRE(std::holds_alternative<backend::ContentDelta>(**second));
  REQUIRE(third);
  REQUIRE(*third);
  REQUIRE(std::holds_alternative<backend::UsageObserved>(**third));
  REQUIRE(fourth);
  REQUIRE(*fourth);
  REQUIRE(std::holds_alternative<backend::ResponseFinished>(**fourth));
  REQUIRE(end);
  REQUIRE_FALSE(*end);
  REQUIRE(fake.recorded_requests() == std::vector<backend::BackendRequest>{request()});
}

TEST_CASE("scripted stream exposes mid-stream errors and premature EOF",
          "[backend][failure]") {
  const backend::BackendError protocol{backend::BackendErrorKind::protocol,
                                       "malformed provider stream", false, std::nullopt};
  testing::ScriptedBackend failing{{testing::ScriptedExchange{
      request(), testing::StreamScript{{event_step(backend::ResponseStarted{"response"}),
                                        protocol}}}}};

  auto started = failing.start(request(), {});
  REQUIRE(started);
  REQUIRE((*started)->next({}));
  const auto failed = (*started)->next({});
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error() == protocol);

  testing::ScriptedBackend premature{{testing::ScriptedExchange{
      request(), testing::StreamScript{{event_step(backend::ResponseStarted{"response"})}}}}};
  auto short_stream = premature.start(request(), {});
  REQUIRE(short_stream);
  REQUIRE((*short_stream)->next({}));
  const auto eof = (*short_stream)->next({});
  REQUIRE(eof);
  REQUIRE_FALSE(*eof);
}

TEST_CASE("cancellation is deterministic before start and between events",
          "[backend][failure]") {
  const testing::ScriptedExchange exchange{
      request(), testing::StreamScript{{event_step(backend::ResponseStarted{"response"}),
                                        event_step(backend::ResponseFinished{
                                            domain::FinishReason::stop})}}};

  testing::ScriptedBackend pre_cancelled{{exchange}};
  std::stop_source before;
  before.request_stop();
  const auto did_not_start = pre_cancelled.start(request(), before.get_token());
  REQUIRE_FALSE(did_not_start);
  REQUIRE(did_not_start.error().kind == backend::BackendErrorKind::cancelled);
  REQUIRE(pre_cancelled.remaining_exchanges() == 1);
  REQUIRE(pre_cancelled.recorded_requests().empty());

  testing::ScriptedBackend mid_cancelled{{exchange}};
  auto started = mid_cancelled.start(request(), {});
  REQUIRE(started);
  REQUIRE((*started)->next({}));

  std::stop_source after_one;
  after_one.request_stop();
  const auto cancelled = (*started)->next(after_one.get_token());
  REQUIRE(cancelled);
  REQUIRE(std::holds_alternative<backend::ResponseCancelled>(**cancelled));
  const auto end = (*started)->next(after_one.get_token());
  REQUIRE(end);
  REQUIRE_FALSE(*end);
}
