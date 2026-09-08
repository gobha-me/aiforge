#include "fixture.hpp"
#include <limits>
#include <stop_token>

namespace {
using namespace summary_context_test;
using Code = runtime::ConversationSummaryContextErrorCode;
} // namespace

TEST_CASE("summary context rejects invalid order limits cancellation and "
          "incomplete admissions",
          "[summarycontext][failure]") {
  Fixture f;
  f.source("old");
  const auto summary = f.summary("summary", {id<domain::RunId>("old")});
  f.activate(summary);
  CHECK_FALSE(runtime::prepare_conversation_summary_context(f.log, 0));
  for (unsigned bound = 0; bound < 4; ++bound) {
    auto limits = runtime::ConversationHistoryLimits{};
    if (bound == 0) limits.maximum_content_bytes = 1;
    if (bound == 1) limits.maximum_content_items = 0;
    if (bound == 2) limits.maximum_source_references = 0;
    if (bound == 3) limits.maximum_events = 1;
    CHECK_FALSE(
        runtime::prepare_conversation_summary_context(f.log, 1, limits));
  }
  CHECK_FALSE(runtime::recover_conversation_summary_context(
      f.log, {}, f.log.last_sequence()));
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = runtime::prepare_conversation_summary_context(
      f.log, 1, {}, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == Code::cancelled);
}

TEST_CASE(
    "summary context rejects changed evidence and saved activation metadata",
    "[summarycontext][failure]") {
  Fixture f;
  f.source("old");
  f.activate(f.summary("summary", {id<domain::RunId>("old")}));
  const auto prepared = runtime::prepare_conversation_summary_context(f.log, 9);
  REQUIRE(prepared);
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    auto saved = prepared->summaries;
    if (mutation == 0) saved.front().estimated_tokens++;
    if (mutation == 1) saved.front().message_digest.value[0] = 'x';
    if (mutation == 2) saved.front().activation_sequence++;
    if (mutation == 3) saved.front().candidate.revision++;
    if (mutation == 4) saved.front().source_anchor_sequence++;
    if (mutation == 5) saved.front().provenance.source_location = "foreign";
    CHECK_FALSE(runtime::recover_conversation_summary_context(
        f.log, saved, f.log.last_sequence()));
  }
  const auto restored = runtime::recover_conversation_summary_context(
      f.log, prepared->summaries, f.log.last_sequence());
  REQUIRE(restored);
  CHECK(*restored == *prepared);
}

TEST_CASE("disable and reactivation cannot revive an unresolved old summary "
          "admission",
          "[summarycontext][recovery]") {
  Fixture f;
  f.source("old");
  const auto summary = f.summary("summary", {id<domain::RunId>("old")});
  const auto first = f.activate(summary);
  const auto prepared = runtime::prepare_conversation_summary_context(f.log);
  REQUIRE(prepared);
  const auto snapshot = f.log.last_sequence();
  f.disable(first);
  auto restored = runtime::recover_conversation_summary_context(
      f.log, prepared->summaries, snapshot);
  REQUIRE_FALSE(restored);
  CHECK(restored.error().code == Code::unavailable_activation);
  const auto second = f.activate(summary);
  CHECK(second.candidate == first.candidate);
  CHECK(second.activation_event_id != first.activation_event_id);
  CHECK_FALSE(runtime::recover_conversation_summary_context(
      f.log, prepared->summaries, snapshot));
  const auto pinned = runtime::recover_conversation_summary_context(
      f.log, prepared->summaries, snapshot, {}, {}, false);
  REQUIRE(pinned);
  CHECK(*pinned == *prepared);
  auto forged = prepared->summaries;
  forged.front().message_digest.value[0] = 'x';
  CHECK_FALSE(runtime::recover_conversation_summary_context(
      f.log, forged, snapshot, {}, {}, false));
}

TEST_CASE("active summary ordering follows source chronology rather than "
          "activation order",
          "[summarycontext][ordering]") {
  Fixture f;
  f.source("old");
  f.source("new");
  const auto old = f.summary("z-old", {id<domain::RunId>("old")});
  const auto newer = f.summary("a-new", {id<domain::RunId>("new")});
  f.activate(newer);
  f.activate(old);
  const auto prepared = runtime::prepare_conversation_summary_context(f.log, 7);
  REQUIRE(prepared);
  REQUIRE(prepared->summaries.size() == 2);
  CHECK(prepared->summaries[0].candidate.summary_id == old.intent.summary_id);
  CHECK(prepared->summaries[1].candidate.summary_id == newer.intent.summary_id);
  CHECK(prepared->content[0].order == 7);
  CHECK(prepared->content[1].order == 8);
  CHECK(prepared->content[0].kind == domain::ContextContentKind::evidence);
  CHECK(prepared->content[0].message.role == domain::Role::evidence);
  CHECK(prepared->content[0].message.tool_calls.empty());
  CHECK_FALSE(runtime::prepare_conversation_summary_context(
      f.log, std::numeric_limits<std::uint64_t>::max()));
}

TEST_CASE("rolling summaries are mandatory while full mode keeps original "
          "groups dormant",
          "[summarycontext][selection]") {
  Fixture f;
  f.source("old", std::string(3000, 'x'));
  f.activate(f.summary("summary", {id<domain::RunId>("old")}));
  f.source("recent");
  auto full = runtime::prepare_conversation_context(f.request());
  REQUIRE(full);
  CHECK(full->admission.version == 2);
  CHECK(full->admission.summaries.empty());
  CHECK(full->summary_content.empty());
  CHECK(full->admission.groups.size() == 2);
  f.policy(domain::ConversationMode::rolling);
  const auto rolling = runtime::prepare_conversation_context(f.request(5));
  REQUIRE(rolling);
  REQUIRE(rolling->admission.summaries.size() == 1);
  REQUIRE(rolling->admission.groups.size() == 1);
  CHECK(rolling->admission.groups.front().run_id ==
        id<domain::RunId>("recent"));
  CHECK(rolling->admission.summaries.front().order == 5);
  CHECK(rolling->admission.groups.front().entries.front().order == 6);
  CHECK(rolling->admission.mandatory_input_tokens ==
        10 + rolling->admission.summaries.front().estimated_tokens);
  CHECK(rolling->admission.omitted_group_count == 1);
  REQUIRE(rolling->selection.decisions.size() == 2);
  CHECK(rolling->selection.decisions[0].decision ==
        runtime::ConversationSelectionDecision::omitted_summary);
  CHECK(rolling->selection.omitted_history_tokens >= 3000);
  REQUIRE(runtime::recover_conversation_context(f.log, rolling->admission));
  auto forged = rolling->admission;
  forged.groups = full->admission.groups;
  std::uint64_t forged_order = 6;
  for (auto& group : forged.groups)
    for (auto& entry : group.entries)
      entry.order = forged_order++;
  forged.omitted_group_count = 0;
  forged.omitted_groups_digest = full->admission.omitted_groups_digest;
  REQUIRE(domain::seal_conversation_admission(forged));
  CHECK_FALSE(runtime::recover_conversation_context(f.log, forged));
  auto missing_summary = rolling->admission;
  missing_summary.summaries.clear();
  REQUIRE(domain::seal_conversation_admission(missing_summary));
  CHECK_FALSE(runtime::recover_conversation_context(f.log, missing_summary));

  auto tiny = f.request();
  tiny.capacity.context_window_tokens =
      10 + 16 + 7 + rolling->admission.summaries.front().estimated_tokens - 1;
  CHECK_FALSE(runtime::prepare_conversation_context(tiny));
  f.policy(domain::ConversationMode::rolling, {id<domain::RunId>("old")});
  const auto pinned = runtime::prepare_conversation_context(f.request());
  REQUIRE(pinned);
  REQUIRE(pinned->admission.groups.size() == 2);
  CHECK(pinned->admission.groups.front().pinned);
  CHECK(pinned->admission.omitted_group_count == 0);
}
