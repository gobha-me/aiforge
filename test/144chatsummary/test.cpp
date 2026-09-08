#include "fixture.hpp"

namespace {
using namespace chat_summary_test;
}
TEST_CASE("Chat summary generation rejects invalid explicit source requests "
          "before effects",
          "[chatsummary][failure]") {
  Fixture f;
  auto request = f.request();
  SECTION("stale snapshot") {
    --request.expected_sequence;
  }
  SECTION("missing source") {
    request.covered_run_ids = {id<domain::RunId>("missing")};
  }
  SECTION("duplicate source") {
    request.covered_run_ids.push_back(request.covered_run_ids.front());
  }
  SECTION("empty selection") {
    request.covered_run_ids.clear();
  }
  SECTION("zero output bound") {
    request.maximum_output_bytes = 0;
  }
  SECTION("excessive output bound") {
    request.maximum_output_bytes = domain::summary_maximum_text_bytes + 1;
  }
  SECTION("cancelled") {
    f.stop.request_stop();
  }
  const auto before = f.chat->event_log().events();
  CHECK_FALSE(f.chat->generate_conversation_summary(request));
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Chat summary generation rejects insufficient capacity and failed "
          "persistence",
          "[chatsummary][failure]") {
  Fixture f;
  SECTION("capacity") {
    f.models.window = 300;
    REQUIRE(f.chat->select_model(id<domain::ModelId>("tiny")));
  }
  SECTION("append") {
    f.store.fail_append = true;
  }
  const auto before = f.chat->event_log().events();
  const auto result = f.chat->generate_conversation_summary(f.request());
  REQUIRE_FALSE(result);
  CHECK_FALSE(result.error().effect_may_have_applied);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Chat summary generation is explicit idle-only and sends one "
          "canonical tool-free request",
          "[chatsummary]") {
  Fixture f;
  auto generated = f.chat->generate_conversation_summary(f.request());
  REQUIRE(generated);
  CHECK_FALSE(f.chat->generate_conversation_summary(f.request()));
  f.drain();
  const auto requests = f.backend.requests();
  REQUIRE(requests.size() == 1);
  CHECK(f.backend.intent_was_durable());
  CHECK(requests.front().tools.empty());
  CHECK_FALSE(requests.front().options.temperature);
  CHECK_FALSE(requests.front().options.seed);
  CHECK(requests.front().options.extensions.empty());
  CHECK(requests.front().options.max_output_tokens == 256);
  CHECK(requests.front().assistant_continuation_state.empty());
  CHECK(
      f.chat->submitted_prompts() ==
      std::vector<std::string>{"Preserve the open task and its constraints."});
  const auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  CHECK(catalog->snapshot.candidates.empty());
  CHECK(catalog->snapshot.active.empty());
  REQUIRE(catalog->unpublished.size() == 1);
  CHECK(catalog->unpublished.front().intent.summary_id ==
        generated->summary_id);
  f.reopen();
  const auto recovered = f.chat->summary_catalog();
  REQUIRE(recovered);
  CHECK(recovered->unpublished == catalog->unpublished);
  CHECK(f.backend.requests().size() == 1);
  CHECK(f.chat->submitted_prompts().size() == 1);
}
TEST_CASE("Chat completed summary publication recovers after append failure "
          "without regeneration",
          "[chatsummary][recovery]") {
  Fixture f;
  const auto generated = f.generate();
  f.store.fail_append = true;
  const auto before = f.chat->event_log().events();
  CHECK_FALSE(f.chat->publish_conversation_summary(generated.summary_id));
  CHECK(f.chat->event_log().events() == before);
  f.store.fail_append = false;
  f.reopen();
  const auto published =
      f.chat->publish_conversation_summary(generated.summary_id);
  REQUIRE(published);
  const auto sequence = f.chat->event_log().last_sequence();
  const auto repeated =
      f.chat->publish_conversation_summary(generated.summary_id);
  REQUIRE(repeated);
  CHECK(*repeated == *published);
  CHECK(f.chat->event_log().last_sequence() == sequence);
  CHECK(f.backend.requests().size() == 1);
  const auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  CHECK(catalog->unpublished.empty());
  CHECK(catalog->snapshot.active.empty());
}
TEST_CASE("Chat summary edits require exact parent and retain original output "
          "provenance",
          "[chatsummary][edit]") {
  Fixture f;
  const auto original = f.candidate();
  const auto sequence = f.chat->event_log().last_sequence();
  CHECK_FALSE(f.chat->edit_conversation_summary(sequence - 1, version(original),
                                                "edited"));
  const auto edited = f.chat->edit_conversation_summary(
      sequence, version(original), "Reviewed unfinished work.");
  REQUIRE(edited);
  CHECK(edited->author == domain::ConversationSummaryAuthor::user_edit);
  CHECK(edited->edited_from == version(original));
  CHECK(edited->output_event_id == original.output_event_id);
  CHECK(edited->source_digest == original.source_digest);
  CHECK_FALSE(f.chat->edit_conversation_summary(
      f.chat->event_log().last_sequence(), version(original), "stale"));
  CHECK(f.backend.requests().size() == 1);
}

TEST_CASE(
    "Unpublishable completed summary cannot hide existing reviewed catalog",
    "[chatsummary][length]") {
  Fixture f;
  const auto original = f.candidate();
  const auto policy = f.chat->conversation_policy();
  REQUIRE(policy);
  REQUIRE(f.chat->set_conversation_policy(
      policy->policy.revision, domain::ConversationMode::rolling, {}));
  const auto preview =
      f.chat->preview_conversation_summary(version(original), {}, "next draft");
  REQUIRE(preview);
  const auto activation =
      f.chat->apply_conversation_summary(*preview, "next draft");
  REQUIRE(activation);
  f.backend.reason = domain::FinishReason::length;
  const auto incomplete = f.generate();
  const auto before = f.chat->event_log().events();
  const auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  REQUIRE(catalog->snapshot.active.size() == 1);
  CHECK(catalog->snapshot.active.front() == *activation);
  REQUIRE(catalog->unpublishable.size() == 1);
  CHECK(catalog->unpublishable.front().summary_id == incomplete.summary_id);
  CHECK_FALSE(catalog->unpublishable.front().message.empty());
  CHECK(catalog->unpublished.empty());
  CHECK_FALSE(f.chat->publish_conversation_summary(incomplete.summary_id));
  CHECK(f.chat->event_log().events() == before);
  f.reopen();
  const auto reopened = f.chat->summary_catalog();
  REQUIRE(reopened);
  CHECK(reopened->snapshot.active == catalog->snapshot.active);
  CHECK(reopened->unpublishable.size() == 1);
  CHECK(f.backend.requests().size() == 2);
}

TEST_CASE("Chat summary generation honors current session spending boundary",
          "[chatsummary][spend]") {
  Fixture f;
  f.ceiling = domain::SessionSpendCeiling::from("0.01").value();
  f.reopen();
  f.generate();
  const auto before = f.chat->event_log().events();
  const auto second = f.chat->generate_conversation_summary(f.request());
  REQUIRE_FALSE(second);
  CHECK(second.error().code ==
        surfaces::ChatSessionErrorCode::spend_accounting_unavailable);
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
}
