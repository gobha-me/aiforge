#include "../131summarykernel/fixture.hpp"

namespace {
using namespace summary_kernel_test;
auto publication(std::string run = "publication")
    -> runtime::ConversationSummaryPublication {
  return {id<domain::RunId>(run), attributes(domain::RunPurpose::control),
          id<domain::ConversationSummaryId>("summary")};
}
auto version(const domain::ConversationSummaryCandidate& candidate)
    -> domain::ConversationSummaryVersion {
  REQUIRE(candidate.candidate_digest);
  return {candidate.summary_id, candidate.revision,
          *candidate.candidate_digest};
}
auto publish(Fixture& fixture) -> domain::ConversationSummaryCandidate {
  auto candidate = fixture.kernel->publish_conversation_summary(publication());
  REQUIRE(candidate);
  return std::move(*candidate);
}
} // namespace

TEST_CASE("publication cannot generate missing output or use noncontrol "
          "mutation runs",
          "[summarypublication][failure]") {
  Fixture fixture;
  const auto source = fixture.kernel->event_log().events();
  CHECK_FALSE(fixture.kernel->publish_conversation_summary(publication()));
  CHECK(fixture.kernel->event_log().events() == source);
  CHECK(fixture.backend.requests().empty());
  fixture.complete();
  auto invalid = publication();
  invalid.attributes.purpose = domain::RunPurpose::conversation;
  const auto completed = fixture.kernel->event_log().events();
  CHECK_FALSE(fixture.kernel->publish_conversation_summary(invalid));
  CHECK(fixture.kernel->event_log().events() == completed);
  CHECK(fixture.backend.requests().size() == 1);
}

TEST_CASE("durable summary output survives publication failure and reopens "
          "without regeneration",
          "[summarypublication][storage]") {
  Fixture fixture;
  fixture.complete();
  const auto before = fixture.kernel->event_log().events();
  fixture.store.fail_append = true;
  const auto failed =
      fixture.kernel->publish_conversation_summary(publication());
  REQUIRE_FALSE(failed);
  CHECK(fixture.kernel->event_log().events() == before);
  CHECK(fixture.store.history == before);
  CHECK(fixture.backend.requests().size() == 1);
  fixture.store.fail_append = false;
  fixture.reopen();
  CHECK(fixture.backend.requests().size() == 1);
  const auto candidate = publish(fixture);
  CHECK(candidate.revision == 1);
  CHECK(fixture.backend.requests().size() == 1);
  CHECK(fixture.kernel->event_log().events().size() == before.size() + 3);
}

TEST_CASE("summary publication is idempotent and seals actual created event "
          "provenance",
          "[summarypublication][provenance]") {
  Fixture fixture;
  const auto source = fixture.kernel->event_log().events();
  fixture.complete();
  const auto candidate = publish(fixture);
  const auto events = fixture.kernel->event_log().events();
  const auto created = std::ranges::find(
      events, candidate.created_event_id,
      [](const auto& event) { return event.metadata.event_id; });
  REQUIRE(created != events.end());
  CHECK(created->metadata.sequence == candidate.created_sequence);
  REQUIRE(std::holds_alternative<domain::ConversationSummaryCandidatePublished>(
      created->payload));
  CHECK(
      std::get<domain::ConversationSummaryCandidatePublished>(created->payload)
          .candidate == candidate);
  CHECK(candidate.output_sequence < candidate.created_sequence);
  auto state =
      runtime::recorded_conversation_summaries(fixture.kernel->event_log());
  REQUIRE(state);
  REQUIRE(state->intents.size() == 1);
  REQUIRE(domain::validate_conversation_summary_candidate(
      candidate, state->intents.front()));
  CHECK(state->active.empty());
  CHECK(state->policy_revision == 0);
  fixture.reopen();
  const auto repeated = fixture.kernel->publish_conversation_summary(
      publication("retry-publication"));
  REQUIRE(repeated);
  CHECK(*repeated == candidate);
  CHECK(fixture.kernel->event_log().events() == events);
  CHECK(fixture.backend.requests().size() == 1);
  const auto history =
      runtime::reconstruct_conversation_history({fixture.kernel->event_log()});
  REQUIRE(history);
  REQUIRE(history->size() == 1);
  CHECK(history->front().run_id == id<domain::RunId>("source-run"));
  CHECK(std::equal(source.begin(), source.end(), events.begin()));
}

TEST_CASE("summary edits reject stale versions invalid text and preserve "
          "previous candidate on append failure",
          "[summarypublication][edit][failure]") {
  Fixture fixture;
  fixture.complete();
  const auto original = publish(fixture);
  const auto before = fixture.kernel->event_log().events();
  runtime::ConversationSummaryEdit edit{
      id<domain::RunId>("edit"), attributes(domain::RunPurpose::control),
      fixture.kernel->event_log().last_sequence(), version(original),
      "Reviewed facts."};
  auto invalid = edit;
  --invalid.expected_sequence;
  CHECK_FALSE(fixture.kernel->edit_conversation_summary(invalid));
  invalid = edit;
  ++invalid.previous.revision;
  CHECK_FALSE(fixture.kernel->edit_conversation_summary(invalid));
  invalid = edit;
  invalid.previous.candidate_digest.value[0] = 'x';
  CHECK_FALSE(fixture.kernel->edit_conversation_summary(invalid));
  invalid = edit;
  invalid.text.clear();
  CHECK_FALSE(fixture.kernel->edit_conversation_summary(invalid));
  invalid.text = std::string(domain::summary_maximum_text_bytes + 1, 'x');
  CHECK_FALSE(fixture.kernel->edit_conversation_summary(invalid));
  CHECK(fixture.kernel->event_log().events() == before);
  fixture.store.fail_append = true;
  CHECK_FALSE(fixture.kernel->edit_conversation_summary(edit));
  CHECK(fixture.kernel->event_log().events() == before);
  fixture.store.fail_append = false;
  fixture.reopen();
  const auto state =
      runtime::recorded_conversation_summaries(fixture.kernel->event_log());
  REQUIRE(state);
  REQUIRE(state->candidates.size() == 1);
  CHECK(state->candidates.front() == original);
  CHECK(fixture.backend.requests().size() == 1);
}

TEST_CASE("reviewed summary edits retain model output provenance and link the "
          "exact immutable parent",
          "[summarypublication][edit]") {
  Fixture fixture;
  fixture.complete();
  const auto original = publish(fixture);
  runtime::ConversationSummaryEdit edit{
      id<domain::RunId>("edit"), attributes(domain::RunPurpose::control),
      fixture.kernel->event_log().last_sequence(), version(original),
      "Reviewed facts, with uncertainty preserved."};
  const auto edited = fixture.kernel->edit_conversation_summary(edit);
  REQUIRE(edited);
  CHECK(edited->revision == original.revision + 1);
  CHECK(edited->author == domain::ConversationSummaryAuthor::user_edit);
  CHECK(edited->edited_from == edit.previous);
  CHECK(edited->output_event_id == original.output_event_id);
  CHECK(edited->output_sequence == original.output_sequence);
  CHECK(edited->source_digest == original.source_digest);
  CHECK(edited->intent_digest == original.intent_digest);
  CHECK(edited->created_sequence > original.created_sequence);
  CHECK(edited->text == edit.text);
  const auto state =
      runtime::recorded_conversation_summaries(fixture.kernel->event_log());
  REQUIRE(state);
  REQUIRE(state->intents.size() == 1);
  REQUIRE(state->candidates.size() == 2);
  CHECK(state->candidates.front() == original);
  REQUIRE(domain::validate_conversation_summary_candidate(
      *edited, state->intents.front(), &original));
  CHECK(state->active.empty());
  CHECK(state->policy_revision == 0);
  edit.run_id = id<domain::RunId>("stale-parent");
  edit.expected_sequence = fixture.kernel->event_log().last_sequence();
  CHECK_FALSE(fixture.kernel->edit_conversation_summary(edit));
  CHECK(fixture.backend.requests().size() == 1);
}

TEST_CASE("output exceeding the requested byte bound cannot become a summary "
          "candidate",
          "[summarypublication][bounds]") {
  Fixture fixture;
  auto start = fixture.request();
  auto spec = *start.summary_intent;
  spec.maximum_output_bytes = 8;
  spec.intent_digest.reset();
  spec.estimated_input_tokens = 0;
  const auto prepared = runtime::prepare_conversation_summary_generation(
      fixture.kernel->event_log(), spec);
  REQUIRE(prepared);
  start.summary_intent = prepared->intent;
  start.user_message = prepared->user_message;
  start.request.context = prepared->context;
  REQUIRE(fixture.kernel->start(start));
  fixture.drain();
  const auto before = fixture.kernel->event_log().events();
  CHECK_FALSE(fixture.kernel->publish_conversation_summary(publication()));
  CHECK(fixture.kernel->event_log().events() == before);
  CHECK(fixture.backend.requests().size() == 1);
}
