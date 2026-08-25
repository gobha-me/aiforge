#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <variant>

#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/persona.hpp>
#include <aiforge/runtime/persona.hpp>
#include <aiforge/testing/scripted_persona_source.hpp>

namespace {

using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto persona_document(std::string text = "Review carefully.")
    -> domain::PersonaDocument {
  return {{make_id<domain::PersonaId>("persona:reviewer"),
           "Reviewer",
           "personas/Reviewer.md",
           {"sha256", std::string(64, 'a'), text.size()}},
          std::move(text)};
}

auto selection(const domain::PersonaDocument& document)
    -> domain::PersonaSelection {
  return {domain::PersonaSelectionAction::selected,
          domain::PersonaSelectionSource::command_line, document.reference,
          std::nullopt};
}

auto event(const std::uint64_t sequence, domain::RunEventPayload payload)
    -> domain::RunEvent {
  return {{make_id<domain::EventId>("event-" + std::to_string(sequence)),
           make_id<domain::RunId>("run"), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

}  // namespace

TEST_CASE("persona values reject ambiguous identity and malformed content",
          "[persona][domain][failure]") {
  const auto document = persona_document();
  REQUIRE(domain::validate_persona_document(document));
  REQUIRE(domain::validate_persona_selection(selection(document)));

  auto wrong_identity = document;
  wrong_identity.reference.persona_id =
      make_id<domain::PersonaId>("persona:someone-else");
  REQUIRE_FALSE(domain::validate_persona_document(wrong_identity));

  auto escaped = document;
  escaped.reference.source_location = "../Reviewer.md";
  REQUIRE_FALSE(domain::validate_persona_document(escaped));

  auto mismatched_size = document;
  ++mismatched_size.reference.content_digest.byte_size;
  REQUIRE_FALSE(domain::validate_persona_document(mismatched_size));

  auto unsafe = persona_document(std::string{"bad\0text", 8});
  REQUIRE_FALSE(domain::validate_persona_document(unsafe));

  auto inconsistent = selection(document);
  inconsistent.persona.reset();
  REQUIRE_FALSE(domain::validate_persona_selection(inconsistent));
}

TEST_CASE("persona context is a stable attributed system instruction",
          "[persona][context]") {
  const auto document = persona_document();
  const auto instruction =
      runtime::persona_instruction_input(document, 17, 4);
  REQUIRE(instruction);
  REQUIRE(instruction->layer == domain::InstructionLayer::persona);
  REQUIRE(instruction->operation == domain::InstructionOperation::add);
  REQUIRE(instruction->message);
  REQUIRE(instruction->message->role == domain::Role::system);
  REQUIRE(std::get<domain::TextBlock>(instruction->message->content.front())
              .text == document.text);
  REQUIRE(instruction->provenance.source_location == "personas/Reviewer.md");
  REQUIRE(instruction->provenance.digest ==
          "sha256:" + document.reference.content_digest.value);
  REQUIRE(instruction->order == 4);
  REQUIRE(instruction->estimated_tokens == 17);

  REQUIRE_FALSE(runtime::persona_instruction_input(document, 0));
}

TEST_CASE("latest persona selection is derived from append-only history",
          "[persona][events]") {
  const auto document = persona_document();
  domain::SessionEventLog log{make_id<domain::SessionId>("session")};
  REQUIRE(log.append(event(
      1, domain::PersonaSelectionRecorded{selection(document)})));
  REQUIRE(log.append(event(
      2, domain::PersonaSelectionRecorded{{
             domain::PersonaSelectionAction::disabled,
             domain::PersonaSelectionSource::interactive, std::nullopt,
             document.reference}})));

  const auto latest = runtime::latest_persona_selection(log);
  REQUIRE(latest);
  REQUIRE(*latest);
  REQUIRE((*latest)->action == domain::PersonaSelectionAction::disabled);
  REQUIRE((*latest)->previous_persona == document.reference);
}

TEST_CASE("scripted persona source is bounded deterministic and cancellable",
          "[persona][fake][failure]") {
  const auto document = persona_document();
  testing::ScriptedPersonaSource source{
      {std::vector<domain::PersonaSummary>{{document.reference, "Review"}}},
      {{"Reviewer", document}}};

  const auto listed = source.list();
  REQUIRE(listed);
  REQUIRE(listed->front().reference == document.reference);
  const auto loaded = source.load("Reviewer");
  REQUIRE(loaded == document);
  REQUIRE(source.recorded_loads() == std::vector<std::string>{"Reviewer"});

  const auto exhausted = source.load("Reviewer");
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code == persona::PersonaErrorCode::internal_failure);

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped = source.list({}, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code == persona::PersonaErrorCode::cancelled);
}
