#include "../135summarycontext/fixture.hpp"
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/testing/scripted_session_store.hpp>

namespace {
using namespace summary_context_test;
}

TEST_CASE("mandatory summaries reject before any scoped memory selection",
          "[summarysession][failure]") {
  Fixture f;
  f.source("old");
  f.activate(f.summary("summary", {id<domain::RunId>("old")}));
  f.policy(domain::ConversationMode::rolling);
  auto input = mandatory();
  input.capacity.context_window_tokens = 50;
  testing::ScriptedSessionStore store{{}};
  runtime::MemoryController memory{store, [] { return 1; },
                                   [] { return domain::EventTimestamp{}; }};
  const auto result = runtime::prepare_session_context(
      {f.log, id<domain::ModelId>("model"), input, &memory, {}, {}, {}});
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::SessionContextErrorCode::conversation_failed);
  CHECK(store.recorded_calls().empty());
}

TEST_CASE(
    "summary reservation precedes scoped memory budget and recent history",
    "[summarysession][budget]") {
  Fixture f;
  f.source("old", std::string(3000, 'o'));
  f.activate(f.summary("summary", {id<domain::RunId>("old")}));
  f.source("recent", std::string(1000, 'r'));
  f.policy(domain::ConversationMode::rolling);
  const auto summaries = runtime::prepare_conversation_summary_context(f.log);
  REQUIRE(summaries);
  auto input = mandatory();
  input.capacity.context_window_tokens =
      16 + 7 + 20 + summaries->estimated_tokens + 100;
  const auto result = runtime::prepare_session_context(
      {f.log, id<domain::ModelId>("model"), input, nullptr, {}, {}, {}});
  REQUIRE(result);
  CHECK(result->memory_selection.available_tokens == 100);
  CHECK(result->conversation_admission.mandatory_input_tokens ==
        20 + summaries->estimated_tokens);
  CHECK(result->conversation_admission.groups.empty());
  REQUIRE(result->conversation_admission.summaries.size() == 1);
  REQUIRE(result->input.content.size() == 2);
  CHECK(result->input.content[0].kind == domain::ContextContentKind::evidence);
  CHECK(result->input.content[0].order == 1);
  CHECK(result->input.content[1].message.message_id ==
        id<domain::MessageId>("current-user"));
  CHECK(result->input.content[1].order == 2);
  CHECK(result->conversation_admission.omitted_group_count == 2);
  const auto built = runtime::ContextBuilder{}.build(result->input);
  REQUIRE(built);
  CHECK(built->estimated_input_tokens == 7 + 20 + summaries->estimated_tokens);
  CHECK(input.content.size() == 1);
}

TEST_CASE("covered pins reserve original evidence in addition to their summary",
          "[summarysession][pins]") {
  Fixture f;
  f.source("old", std::string(1000, 'o'));
  f.activate(f.summary("summary", {id<domain::RunId>("old")}));
  f.policy(domain::ConversationMode::rolling, {id<domain::RunId>("old")});
  const auto summary = runtime::prepare_conversation_summary_context(f.log);
  const auto history = runtime::reconstruct_conversation_history({f.log});
  REQUIRE(summary);
  REQUIRE(history);
  const auto pin_tokens =
      history->front().entries.front().content.estimated_tokens;
  auto input = mandatory();
  input.capacity.context_window_tokens =
      16 + 7 + 20 + summary->estimated_tokens + pin_tokens + 100;
  const auto result = runtime::prepare_session_context(
      {f.log, id<domain::ModelId>("model"), input, nullptr, {}, {}, {}});
  REQUIRE(result);
  CHECK(result->memory_selection.available_tokens == 100);
  REQUIRE(result->conversation_admission.groups.size() == 1);
  CHECK(result->conversation_admission.groups.front().pinned);
  CHECK(result->conversation_admission.omitted_group_count == 0);
  REQUIRE(result->input.content.size() == 3);
  CHECK(result->input.content[0].kind == domain::ContextContentKind::evidence);
  CHECK(result->input.content[1].message.role == domain::Role::user);
  CHECK(result->input.content[2].message.message_id ==
        id<domain::MessageId>("current-user"));
}

TEST_CASE("full policy leaves active summaries dormant without disabling a "
          "rolling admission",
          "[summarysession][full]") {
  Fixture f;
  f.source("old");
  f.activate(f.summary("summary", {id<domain::RunId>("old")}));
  f.policy(domain::ConversationMode::rolling);
  auto input = mandatory();
  const auto rolling = runtime::prepare_session_context(
      {f.log, id<domain::ModelId>("model"), input, nullptr, {}, {}, {}});
  REQUIRE(rolling);
  f.policy(domain::ConversationMode::full);
  const auto full = runtime::prepare_session_context(
      {f.log, id<domain::ModelId>("model"), input, nullptr, {}, {}, {}});
  REQUIRE(full);
  CHECK(full->conversation_admission.version == 2);
  CHECK(full->conversation_admission.summaries.empty());
  REQUIRE(full->conversation_admission.groups.size() == 1);
  CHECK(full->conversation_admission.groups.front().run_id ==
        id<domain::RunId>("old"));
  CHECK(full->conversation_admission.mandatory_input_tokens == 20);
  CHECK(runtime::recover_conversation_summary_context(
      f.log, rolling->conversation_admission.summaries,
      rolling->conversation_admission.source_snapshot_sequence));
  CHECK(runtime::recover_conversation_context(f.log,
                                              rolling->conversation_admission));
}
