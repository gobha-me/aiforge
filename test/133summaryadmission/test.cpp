#include <aiforge/domain/conversation_admission.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>

namespace {
using namespace aiforge::domain;
using Code = ConversationAdmissionErrorCode;
template <typename T> auto id(const std::string& text) -> T {
  return T::from(text).value();
}
auto message() -> Message {
  return {
      id<MessageId>("message"), Role::user, {TextBlock{"story"}}, std::nullopt};
}
auto admission() -> ConversationAdmission {
  const auto text = message();
  const auto digest = normalized_conversation_message_digest(text).value();
  ConversationAdmission value{.session_id = id<SessionId>("session"),
                              .model_id = id<ModelId>("model"),
                              .policy_event_id = {},
                              .capacity = {},
                              .groups = {},
                              .omitted_groups_digest = {},
                              .admission_digest = {}};
  value.source_snapshot_sequence = 10;
  value.capacity = {100, 10, 5};
  value.mandatory_input_tokens = 15;
  value.groups = {
      {id<RunId>("run"),
       {{id<EventId>("event"),
         2,
         id<ContextEntryId>("entry"),
         text.message_id,
         {id<ContextSourceId>("source"), "session:source", "source-digest"},
         1,
         20,
         ContextContentKind::conversation,
         digest}},
       false}};
  return value;
}
auto summary() -> ConversationAdmittedSummary {
  Message text{id<MessageId>("summary-message"),
               Role::evidence,
               {TextBlock{"Derived facts"}},
               {}};
  return {{id<ConversationSummaryId>("summary"),
           1,
           {"sha256", std::string(64, 'a'), 200}},
          id<EventId>("activation"),
          9,
          1,
          id<ContextEntryId>("summary-entry"),
          text.message_id,
          {id<ContextSourceId>("summary-source"), "derived summary",
           "candidate-digest"},
          2,
          10,
          normalized_conversation_message_digest(text).value()};
}
auto with_summary() -> ConversationAdmission {
  auto value = admission();
  value.version = 2;
  value.mode = ConversationMode::rolling;
  value.policy_revision = 1;
  value.policy_event_id = id<EventId>("policy");
  value.summaries.push_back(summary());
  value.groups[0].entries[0].order = 3;
  return value;
}
} // namespace

TEST_CASE(
    "v2 summary admission rejects invalid scope order and mandatory accounting",
    "[summaryadmission][failure]") {
  auto value = with_summary();
  SECTION("legacy v1 cannot carry summaries") {
    value.version = 1;
  }
  SECTION("unknown schema") {
    value.version = 99;
  }
  SECTION("full mode keeps summaries dormant") {
    value.mode = ConversationMode::full;
  }
  SECTION("zero revision") {
    value.summaries[0].candidate.revision = 0;
  }
  SECTION("malformed candidate digest") {
    value.summaries[0].candidate.candidate_digest.algorithm = "md5";
  }
  SECTION("zero activation sequence") {
    value.summaries[0].activation_sequence = 0;
  }
  SECTION("future activation sequence") {
    value.summaries[0].activation_sequence = 11;
  }
  SECTION("zero anchor") {
    value.summaries[0].source_anchor_sequence = 0;
  }
  SECTION("anchor after activation") {
    value.summaries[0].source_anchor_sequence = 9;
  }
  SECTION("zero evidence order") {
    value.summaries[0].order = 0;
  }
  SECTION("history precedes summary") {
    value.groups[0].entries[0].order = 1;
  }
  SECTION("zero evidence estimate") {
    value.summaries[0].estimated_tokens = 0;
  }
  SECTION("summary not included in mandatory estimate") {
    value.mandatory_input_tokens = 9;
  }
  SECTION("empty provenance") {
    value.summaries[0].provenance.source_location = "";
  }
  SECTION("oversized provenance") {
    value.summaries[0].provenance.digest =
        std::string(conversation_maximum_provenance_bytes + 1, 'x');
  }
  SECTION("malformed message digest") {
    value.summaries[0].message_digest.value = "wrong";
  }
  SECTION("activation collides with history event") {
    value.summaries[0].activation_event_id =
        value.groups[0].entries[0].completed_event_id;
  }
  SECTION("activation sequence collides with history") {
    value.summaries[0].activation_sequence = 2;
  }
  SECTION("entry collides with history") {
    value.summaries[0].entry_id = value.groups[0].entries[0].entry_id;
  }
  SECTION("message collides with history") {
    value.summaries[0].message_id = value.groups[0].entries[0].message_id;
  }
  SECTION("provenance collides with history") {
    value.summaries[0].provenance.source_id =
        value.groups[0].entries[0].provenance.source_id;
  }
  SECTION("oversized summary array") {
    value.summaries.resize(summary_maximum_active + 1, value.summaries[0]);
  }
  REQUIRE_FALSE(seal_conversation_admission(value));
}

TEST_CASE(
    "v2 summary admission seals every immutable reference and evidence field",
    "[summaryadmission][failure]") {
  auto value = with_summary();
  REQUIRE(seal_conversation_admission(value));
  SECTION("summary identity") {
    value.summaries[0].candidate.summary_id =
        id<ConversationSummaryId>("other");
  }
  SECTION("candidate revision") {
    ++value.summaries[0].candidate.revision;
  }
  SECTION("candidate digest") {
    value.summaries[0].candidate.candidate_digest.value.assign(64, 'b');
  }
  SECTION("activation identity") {
    value.summaries[0].activation_event_id = id<EventId>("reactivated");
  }
  SECTION("activation sequence") {
    value.summaries[0].activation_sequence = 10;
  }
  SECTION("source anchor") {
    value.summaries[0].source_anchor_sequence = 2;
  }
  SECTION("evidence identity") {
    value.summaries[0].entry_id = id<ContextEntryId>("other");
  }
  SECTION("message identity") {
    value.summaries[0].message_id = id<MessageId>("other");
  }
  SECTION("source identity") {
    value.summaries[0].provenance.source_id = id<ContextSourceId>("other");
  }
  SECTION("source location") {
    value.summaries[0].provenance.source_location = "other";
  }
  SECTION("source digest") {
    value.summaries[0].provenance.digest = "other";
  }
  SECTION("evidence order") {
    value.summaries[0].order = 1;
  }
  SECTION("evidence estimate") {
    ++value.summaries[0].estimated_tokens;
  }
  SECTION("message digest") {
    value.summaries[0].message_digest.value.assign(64, 'b');
  }
  SECTION("missing summaries") {
    value.summaries.clear();
  }
  REQUIRE_FALSE(validate_conversation_admission(value));
}

TEST_CASE("v2 summary admission is canonical bounded and counted once",
          "[summaryadmission]") {
  auto value = with_summary();
  value.capacity.context_window_tokens = 50;
  REQUIRE(seal_conversation_admission(value));
  REQUIRE(validate_conversation_admission(value));
  --value.capacity.context_window_tokens;
  REQUIRE_FALSE(seal_conversation_admission(value));
}

TEST_CASE("v2 empty summaries remain distinct from legacy v1",
          "[summaryadmission]") {
  auto legacy = admission();
  REQUIRE(seal_conversation_admission(legacy));
  auto current = legacy;
  current.version = 2;
  REQUIRE(seal_conversation_admission(current));
  CHECK(current.summaries.empty());
  CHECK(current.admission_digest != legacy.admission_digest);
  REQUIRE(validate_conversation_admission(legacy));
  REQUIRE(validate_conversation_admission(current));
}

TEST_CASE("v1 admission golden seals remain byte identical",
          "[summaryadmission][legacy]") {
  auto value = admission();
  std::string digest =
      "3dde7817c883a8c3ee4740ef7847c9450155456897b85dd5268ae33927a30d50";
  std::uint64_t bytes = 269;
  SECTION("populated full history") {
  }
  SECTION("empty full history") {
    value.groups.clear();
    digest = "ef9077cb748c6b107b7f276b8ae5fc3486be64b436e96685cd7429bd672f1a3e";
    bytes = 96;
  }
  SECTION("rolling pins and omissions") {
    value.mode = ConversationMode::rolling;
    value.policy_revision = 1;
    value.policy_event_id = id<EventId>("policy");
    value.groups[0].pinned = true;
    value.omitted_group_count = 1;
    value.omitted_groups_digest =
        ContentDigest{"sha256", std::string(64, 'b'), 17};
    digest = "53c92f1f1b2b6e65d26501c58d03a7f805c383ed9537f87dafa80c13ad187862";
    bytes = 356;
  }
  REQUIRE(seal_conversation_admission(value));
  REQUIRE(value.admission_digest);
  CHECK(value.admission_digest->value == digest);
  CHECK(value.admission_digest->byte_size == bytes);
  CHECK(validate_conversation_admission(value));
}

TEST_CASE("v2 summary sets reject duplicate references and noncanonical order",
          "[summaryadmission][failure]") {
  auto value = with_summary();
  auto second = summary();
  second.candidate.summary_id = id<ConversationSummaryId>("summary-z");
  second.activation_event_id = id<EventId>("activation-z");
  second.activation_sequence = 10;
  second.entry_id = id<ContextEntryId>("entry-z");
  second.message_id = id<MessageId>("message-z");
  second.provenance.source_id = id<ContextSourceId>("source-z");
  second.order = 3;
  value.summaries.push_back(second);
  value.groups[0].entries[0].order = 4;
  value.mandatory_input_tokens = 25;
  REQUIRE(seal_conversation_admission(value));
  SECTION("duplicate summary even at another revision") {
    value.summaries[1].candidate.summary_id =
        value.summaries[0].candidate.summary_id;
    ++value.summaries[1].candidate.revision;
  }
  SECTION("duplicate activation identity") {
    value.summaries[1].activation_event_id =
        value.summaries[0].activation_event_id;
  }
  SECTION("duplicate activation sequence") {
    value.summaries[1].activation_sequence =
        value.summaries[0].activation_sequence;
  }
  SECTION("duplicate entry identity") {
    value.summaries[1].entry_id = value.summaries[0].entry_id;
  }
  SECTION("duplicate message identity") {
    value.summaries[1].message_id = value.summaries[0].message_id;
  }
  SECTION("duplicate provenance identity") {
    value.summaries[1].provenance.source_id =
        value.summaries[0].provenance.source_id;
  }
  SECTION("duplicate content order") {
    value.summaries[1].order = value.summaries[0].order;
  }
  SECTION("unstable identity tie break") {
    value.summaries[1].candidate.summary_id =
        id<ConversationSummaryId>("a-summary");
  }
  SECTION("summary token sum overflow") {
    value.summaries[0].estimated_tokens =
        std::numeric_limits<std::uint64_t>::max();
    value.mandatory_input_tokens = std::numeric_limits<std::uint64_t>::max();
  }
  REQUIRE_FALSE(seal_conversation_admission(value));
}
