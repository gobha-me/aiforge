#include <catch2/catch_test_macros.hpp>

#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_session_store.hpp>

namespace {
using namespace aiforge;

template <typename T> auto id(const char* value) -> T {
  return T::from(value).value();
}

struct Fixture {
  domain::SessionEventLog log{id<domain::SessionId>("session")};

  auto append(domain::RunEventPayload payload, std::uint32_t schema = 1,
              const char* run = "control") -> void {
    const auto sequence = log.last_sequence() + 1;
    REQUIRE(log.append(
        {{domain::EventId::from("event-" + std::to_string(sequence)).value(),
          id<domain::RunId>(run), sequence, schema, domain::EventTimestamp{},
          std::nullopt, std::nullopt, std::nullopt},
         std::move(payload)}));
  }

  auto start(domain::RunPurpose purpose = domain::RunPurpose::control) -> void {
    domain::RunStarted started{
        id<domain::SurfaceId>("chat"), id<domain::WorkspaceId>("chat"),
        id<domain::PermissionProfileId>("observe"), std::nullopt};
    started.purpose = purpose;
    append(std::move(started), 3);
  }

  auto policy(std::uint64_t previous = 0, std::uint64_t revision = 1) -> void {
    append(domain::ConversationPolicySet{
        previous, {revision, domain::ConversationMode::rolling, {}}});
  }
};

auto change(const char* run = "policy", std::uint64_t revision = 0)
    -> runtime::ConversationPolicyChange {
  return {id<domain::RunId>(run),
          {id<domain::SurfaceId>("chat"), id<domain::WorkspaceId>("chat"),
           id<domain::PermissionProfileId>("observe"), std::nullopt,
           std::nullopt, domain::RunPurpose::control},
          revision,
          domain::ConversationMode::rolling,
          {}};
}
} // namespace

TEST_CASE("Conversation policy requires a complete control transaction") {
  Fixture fixture;
  SECTION("missing start") {
    fixture.policy();
    fixture.append(domain::RunCompleted{});
  }
  SECTION("missing completion") {
    fixture.start();
    fixture.policy();
  }
  SECTION("ordinary conversation cannot change policy") {
    fixture.start(domain::RunPurpose::conversation);
    fixture.policy();
    fixture.append(domain::RunCompleted{});
  }
  SECTION("failed transaction") {
    fixture.start();
    fixture.policy();
    fixture.append(
        domain::RunFailed{{domain::ErrorCode::unavailable, "failure", false}});
  }
  SECTION("foreign completion") {
    fixture.start();
    fixture.policy();
    fixture.append(domain::RunCompleted{}, 1, "other");
  }
  const auto result = runtime::recorded_conversation_policy(fixture.log);
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        domain::ConversationAdmissionErrorCode::invalid_policy);
}

TEST_CASE("Conversation policy refuses stale or invalid revisions") {
  Fixture fixture;
  fixture.start();
  SECTION("stale base") {
    fixture.policy(1, 2);
  }
  SECTION("skipped revision") {
    fixture.policy(0, 2);
  }
  SECTION("zero revision") {
    fixture.policy(0, 0);
  }
  SECTION("unknown mode") {
    fixture.append(domain::ConversationPolicySet{
        0, {1, static_cast<domain::ConversationMode>(255), {}}});
  }
  SECTION("duplicate pins") {
    fixture.append(domain::ConversationPolicySet{
        0,
        {1,
         domain::ConversationMode::rolling,
         {id<domain::RunId>("source"), id<domain::RunId>("source")}}});
  }
  fixture.append(domain::RunCompleted{});
  REQUIRE_FALSE(runtime::recorded_conversation_policy(fixture.log));
}

TEST_CASE("Unsupported conversation policy cannot become implicit full") {
  Fixture fixture;
  fixture.append(domain::UnknownEvent{"session.conversation_policy_set"}, 9);
  const auto result = runtime::recorded_conversation_policy(fixture.log);
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        domain::ConversationAdmissionErrorCode::unsupported_version);
}

TEST_CASE("Policy projection refuses a reused control run") {
  Fixture fixture;
  fixture.start();
  fixture.policy();
  fixture.append(domain::RunCompleted{});
  fixture.start();
  fixture.policy(1, 2);
  fixture.append(domain::RunCompleted{});
  REQUIRE_FALSE(runtime::recorded_conversation_policy(fixture.log));
}

TEST_CASE("Policy recovery uses an exact historical snapshot") {
  Fixture fixture;
  fixture.start();
  fixture.policy();
  fixture.append(domain::RunCompleted{});
  fixture.append(domain::UnknownEvent{"session.conversation_policy_set"}, 9);
  REQUIRE_FALSE(runtime::recorded_conversation_policy(fixture.log));
  const auto original = runtime::recorded_conversation_policy(fixture.log, 3);
  REQUIRE(original);
  CHECK(original->policy.revision == 1);
  CHECK(
      runtime::recorded_conversation_policy(fixture.log, 0)->policy.revision ==
      0);
  CHECK_FALSE(runtime::recorded_conversation_policy(fixture.log, 2));
  CHECK_FALSE(runtime::recorded_conversation_policy(fixture.log, 5));
}

TEST_CASE("Conversation policy is implicit full until explicitly changed") {
  Fixture fixture;
  const auto initial = runtime::recorded_conversation_policy(fixture.log);
  REQUIRE(initial);
  CHECK(initial->policy.mode == domain::ConversationMode::full);
  CHECK(initial->policy.revision == 0);
  CHECK_FALSE(initial->event_id);
  fixture.start();
  fixture.policy();
  fixture.append(domain::RunCompleted{});
  const auto selected = runtime::recorded_conversation_policy(fixture.log);
  REQUIRE(selected);
  CHECK(selected->policy.mode == domain::ConversationMode::rolling);
  CHECK(selected->policy.revision == 1);
  CHECK(selected->event_id == id<domain::EventId>("event-2"));
  CHECK(selected->event_sequence == 2);
  CHECK(fixture.log.events().size() == 3);
}

TEST_CASE("Kernel policy rejection preserves history and dispatches nothing") {
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  REQUIRE(kernel.record_conversation_policy(change()));
  const auto original = kernel.event_log().events();
  auto next = change("next", 1);
  SECTION("stale decision") {
    next.expected_revision = 0;
  }
  SECTION("missing pin") {
    next.pinned_run_ids = {id<domain::RunId>("missing")};
  }
  SECTION("policy control cannot be pinned") {
    next.pinned_run_ids = {id<domain::RunId>("policy")};
  }
  SECTION("reused run") {
    next.run_id = id<domain::RunId>("policy");
  }
  SECTION("ordinary conversation attributes") {
    next.attributes.purpose = domain::RunPurpose::conversation;
  }
  REQUIRE_FALSE(kernel.record_conversation_policy(next));
  CHECK(kernel.event_log().events() == original);
  CHECK(backend.recorded_requests().empty());
  const auto policy = runtime::recorded_conversation_policy(kernel.event_log());
  REQUIRE(policy);
  CHECK(policy->policy.revision == 1);
}

TEST_CASE("Policy persistence failure cannot apply a partial decision") {
  testing::ScriptedBackend backend{{}};
  const auto session = id<domain::SessionId>("session");
  const auto timestamp = [] { return domain::EventTimestamp{}; };
  runtime::RunKernel example{session, backend, nullptr, timestamp};
  REQUIRE(example.record_conversation_policy(change()));
  const auto batch = example.event_log().events();
  REQUIRE(batch.size() == 3);
  testing::ScriptedSessionStore store{
      {{testing::CreateSessionCall{{session, domain::EventTimestamp{}}},
        testing::VoidSessionStoreResult{}},
       {testing::AppendEventsCall{session, batch},
        storage::SessionStoreError{storage::SessionStoreErrorCode::contention,
                                   "append refused", true}}}};
  auto kernel = runtime::RunKernel::open_durable(
      {session, runtime::DurableSessionMode::create, domain::EventTimestamp{}},
      store, backend, nullptr, timestamp);
  REQUIRE(kernel);
  REQUIRE_FALSE((*kernel)->record_conversation_policy(change()));
  CHECK((*kernel)->event_log().events().empty());
  CHECK(backend.recorded_requests().empty());
  CHECK(store.remaining_exchanges() == 0);
  CHECK(store.recorded_calls().size() == 2);
  const auto policy =
      runtime::recorded_conversation_policy((*kernel)->event_log());
  REQUIRE(policy);
  CHECK(policy->policy.revision == 0);
}
