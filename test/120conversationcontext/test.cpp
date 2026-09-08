#include <aiforge/runtime/conversation_context.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <limits>
#include <stop_token>
#include <string>

namespace {
using namespace aiforge;
using Code = runtime::ConversationContextErrorCode;
template <class Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}
struct History {
  domain::SessionEventLog log{id<domain::SessionId>("session")};
  std::uint64_t policy_revision{};
  auto add(const std::string& run, domain::RunEventPayload payload) -> void {
    auto sequence = log.last_sequence() + 1;
    const auto schema =
        std::holds_alternative<domain::RunStarted>(payload) ? 3U : 1U;
    REQUIRE(log.append(
        {{id<domain::EventId>("event-" + std::to_string(sequence)),
          id<domain::RunId>(run),
          sequence,
          schema,
          domain::EventTimestamp{std::chrono::milliseconds{sequence}},
          {},
          {},
          {}},
         std::move(payload)}));
  }
  auto start(const std::string& run,
             domain::RunPurpose purpose = domain::RunPurpose::conversation)
      -> void {
    add(run, domain::RunStarted{id<domain::SurfaceId>("chat"),
                                id<domain::WorkspaceId>("chat"),
                                id<domain::PermissionProfileId>("observe"),
                                {},
                                {},
                                purpose});
  }
  auto user(const std::string& run, std::string text) -> void {
    add(run, domain::UserContentAdded{{id<domain::MessageId>(run + "-user"),
                                       domain::Role::user,
                                       {domain::TextBlock{std::move(text)}},
                                       {}}});
  }
  auto turn(const std::string& run, std::string text = "question") -> void {
    start(run);
    user(run, std::move(text));
    const auto inference = id<domain::InferenceId>(run + "-inference");
    const auto message = id<domain::MessageId>(run + "-assistant");
    add(run, domain::InferenceStarted{inference, id<domain::ModelId>("model")});
    add(run, domain::AssistantContentStarted{message, inference});
    add(run, domain::AssistantContentDeltaAdded{message, inference,
                                                domain::TextBlock{"answer"}});
    add(run, domain::AssistantContentFinished{message, inference});
    add(run, domain::InferenceFinished{inference, domain::FinishReason::stop});
    add(run, domain::RunCompleted{});
  }
  auto policy(domain::ConversationMode mode,
              std::vector<domain::RunId> pins = {}) -> void {
    auto run = "policy-" + std::to_string(policy_revision + 1);
    start(run, domain::RunPurpose::control);
    add(run,
        domain::ConversationPolicySet{
            policy_revision, {policy_revision + 1, mode, std::move(pins)}});
    ++policy_revision;
    add(run, domain::RunCompleted{});
  }
};
auto request(const domain::SessionEventLog& log)
    -> runtime::ConversationContextRequest {
  return {log, id<domain::ModelId>("model"), {4096, 100, 50}, 20, 1, {}, {}};
}
auto prepared(const domain::SessionEventLog& log)
    -> runtime::PreparedConversationContext {
  auto result = runtime::prepare_conversation_context(request(log));
  REQUIRE(result);
  return std::move(*result);
}
auto rewritten(const domain::SessionEventLog& source,
               const std::function<void(domain::RunEvent&)>& change)
    -> domain::SessionEventLog {
  domain::SessionEventLog result{source.session_id()};
  for (auto event : source.events()) {
    change(event);
    REQUIRE(result.append(std::move(event)));
  }
  return result;
}
} // namespace

TEST_CASE("conversation preparation rejects invalid order and bounded failures",
          "[conversation][context][failure]") {
  History history;
  history.turn("first");
  auto input = request(history.log);
  SECTION("zero order") {
    input.first_history_order = 0;
    auto result = runtime::prepare_conversation_context(input);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::invalid_order);
  }
  SECTION("order overflow") {
    input.first_history_order = std::numeric_limits<std::uint64_t>::max();
    auto result = runtime::prepare_conversation_context(input);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::invalid_order);
  }
  SECTION("bounded source events") {
    input.history_limits.maximum_events = 1;
    auto result = runtime::prepare_conversation_context(input);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::resource_exhausted);
  }
  SECTION("cancelled before work") {
    std::stop_source stop;
    stop.request_stop();
    auto result =
        runtime::prepare_conversation_context(input, stop.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::cancelled);
  }
  SECTION("mandatory capacity exhausted") {
    input.mandatory_input_tokens = input.capacity.context_window_tokens;
    auto result = runtime::prepare_conversation_context(input);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::selection_failed);
  }
}

TEST_CASE("conversation recovery rejects corrupted or substituted sources",
          "[conversation][context][failure]") {
  History history;
  history.turn("first");
  auto admission = prepared(history.log).admission;
  SECTION("broken seal") {
    ++admission.mandatory_input_tokens;
    auto result = runtime::recover_conversation_context(history.log, admission);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::invalid_admission);
  }
  SECTION("foreign session") {
    domain::SessionEventLog foreign{id<domain::SessionId>("other")};
    for (const auto& event : history.log.events())
      REQUIRE(foreign.append(event));
    auto result = runtime::recover_conversation_context(foreign, admission);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::invalid_admission);
  }
  SECTION("missing snapshot") {
    domain::SessionEventLog empty{history.log.session_id()};
    REQUIRE_FALSE(runtime::recover_conversation_context(empty, admission));
  }
  SECTION("changed user text with unchanged durable identity") {
    auto changed = rewritten(history.log, [](auto& event) {
      if (auto* user = std::get_if<domain::UserContentAdded>(&event.payload))
        user->message.content = {domain::TextBlock{"different"}};
    });
    auto result = runtime::recover_conversation_context(changed, admission);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::source_mismatch);
  }
  SECTION("resealed wrong token estimate") {
    ++admission.groups.front().entries.front().estimated_tokens;
    REQUIRE(domain::seal_conversation_admission(admission));
    auto result = runtime::recover_conversation_context(history.log, admission);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::source_mismatch);
  }
  SECTION("resealed partial group") {
    admission.groups.front().entries.pop_back();
    REQUIRE(domain::seal_conversation_admission(admission));
    auto result = runtime::recover_conversation_context(history.log, admission);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::source_mismatch);
  }
  SECTION("cancelled recovery") {
    std::stop_source stop;
    stop.request_stop();
    auto result = runtime::recover_conversation_context(history.log, admission,
                                                        {}, stop.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::cancelled);
  }
}

TEST_CASE(
    "rolling selection records omissions and never substitutes later policy",
    "[conversation][context][recovery]") {
  History history;
  history.turn("old", std::string(400, 'x'));
  history.turn("recent");
  auto input = request(history.log);
  input.capacity.context_window_tokens = 400;
  REQUIRE_FALSE(runtime::prepare_conversation_context(input));
  history.policy(domain::ConversationMode::rolling);
  input.first_history_order = 17;
  auto result = runtime::prepare_conversation_context(input);
  REQUIRE(result);
  REQUIRE(result->selection.selected_groups.size() == 1);
  CHECK(result->selection.selected_groups.front().run_id ==
        id<domain::RunId>("recent"));
  CHECK(result->admission.omitted_group_count == 1);
  CHECK(result->admission.omitted_groups_digest.has_value());
  CHECK(result->admission.groups.front().entries.front().order == 17);
  CHECK(result->admission.groups.front().entries.back().order == 18);

  SECTION("later policy history and unsupported policy do not alter exact "
          "recovery") {
    history.policy(domain::ConversationMode::full, {id<domain::RunId>("old")});
    history.turn("new", std::string(800, 'y'));
    history.add("future",
                domain::UnknownEvent{"session.conversation_policy_set"});
    auto recovered =
        runtime::recover_conversation_context(history.log, result->admission);
    REQUIRE(recovered);
    CHECK(*recovered == result->selection.selected_groups);
  }
  SECTION("omitted source identity is sealed") {
    auto changed = rewritten(history.log, [](auto& event) {
      if (event.metadata.run_id == id<domain::RunId>("old") &&
          std::holds_alternative<domain::UserContentAdded>(event.payload))
        event.metadata.event_id = id<domain::EventId>("replaced-source");
    });
    auto recovered =
        runtime::recover_conversation_context(changed, result->admission);
    REQUIRE_FALSE(recovered);
    CHECK(recovered.error().code == Code::source_mismatch);
  }
  SECTION("original policy source must match") {
    auto changed = rewritten(history.log, [](auto& event) {
      if (auto* policy =
              std::get_if<domain::ConversationPolicySet>(&event.payload))
        policy->policy.mode = domain::ConversationMode::full;
    });
    auto recovered =
        runtime::recover_conversation_context(changed, result->admission);
    REQUIRE_FALSE(recovered);
    CHECK(recovered.error().code == Code::source_mismatch);
  }
}

TEST_CASE("conversation pins remain mandatory and source checked",
          "[conversation][context][failure]") {
  History history;
  history.turn("old", std::string(400, 'x'));
  history.turn("recent");
  history.policy(domain::ConversationMode::rolling, {id<domain::RunId>("old")});
  auto input = request(history.log);
  input.capacity.context_window_tokens = 400;
  REQUIRE_FALSE(runtime::prepare_conversation_context(input));
  input.capacity.context_window_tokens = 4096;
  auto result = runtime::prepare_conversation_context(input);
  REQUIRE(result);
  CHECK(result->admission.groups.front().pinned);
  result->admission.groups.front().pinned = false;
  REQUIRE(domain::seal_conversation_admission(result->admission));
  auto recovered =
      runtime::recover_conversation_context(history.log, result->admission);
  REQUIRE_FALSE(recovered);
  CHECK(recovered.error().code == Code::source_mismatch);
}

TEST_CASE("explicit empty snapshot stays empty after later history",
          "[conversation][context][recovery]") {
  History history;
  auto result = prepared(history.log);
  CHECK(result.admission.source_snapshot_sequence == 0);
  CHECK(result.admission.groups.empty());
  REQUIRE(result.admission.admission_digest);
  history.turn("later");
  history.policy(domain::ConversationMode::rolling);
  auto recovered =
      runtime::recover_conversation_context(history.log, result.admission);
  REQUIRE(recovered);
  CHECK(recovered->empty());
}

TEST_CASE("conversation admission preserves complete groups and content order",
          "[conversation][context][roundtrip]") {
  History history;
  history.turn("first");
  history.turn("second");
  auto input = request(history.log);
  input.first_history_order = 11;
  auto result = runtime::prepare_conversation_context(input);
  REQUIRE(result);
  REQUIRE(result->admission.groups.size() == 2);
  REQUIRE(result->admission.groups[0].entries.size() == 2);
  REQUIRE(result->admission.groups[1].entries.size() == 2);
  CHECK(result->admission.groups[0].entries[0].order == 11);
  CHECK(result->admission.groups[1].entries[1].order == 14);
  CHECK_FALSE(result->admission.omitted_groups_digest);
  auto recovered =
      runtime::recover_conversation_context(history.log, result->admission);
  REQUIRE(recovered);
  CHECK(*recovered == result->selection.selected_groups);
  CHECK(result->selection.estimated_input_tokens ==
        input.mandatory_input_tokens + input.capacity.reserved_input_tokens +
            result->selection.selected_history_tokens);
}
