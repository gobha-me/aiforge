#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <variant>

#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/persona.hpp>
#include <aiforge/persona/editor.hpp>
#include <aiforge/runtime/persona.hpp>
#include <aiforge/testing/scripted_persona_editor.hpp>
#include <aiforge/testing/scripted_persona_source.hpp>

namespace {

using namespace aiforge;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
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

auto persona_create(std::string text = "Review carefully.")
    -> persona::PersonaCreate {
  return {{"Reviewer", persona::PersonaFileKind::markdown, std::move(text)},
          {}};
}

} // namespace

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
  const auto instruction = runtime::persona_instruction_input(document, 17, 4);
  REQUIRE(instruction);
  REQUIRE(instruction->layer == domain::InstructionLayer::persona);
  REQUIRE(instruction->operation == domain::InstructionOperation::add);
  REQUIRE(instruction->message);
  REQUIRE(instruction->message->role == domain::Role::system);
  REQUIRE(
      std::get<domain::TextBlock>(instruction->message->content.front()).text ==
      document.text);
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
  REQUIRE(log.append(
      event(1, domain::PersonaSelectionRecorded{selection(document)})));
  REQUIRE(log.append(event(2, domain::PersonaSelectionRecorded{
                                  {domain::PersonaSelectionAction::disabled,
                                   domain::PersonaSelectionSource::interactive,
                                   std::nullopt, document.reference}})));

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
  REQUIRE(exhausted.error().code ==
          persona::PersonaErrorCode::internal_failure);

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped = source.list({}, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code == persona::PersonaErrorCode::cancelled);
}

TEST_CASE("persona writes prepare exact portable identities and receipts",
          "[persona][editor]") {
  const auto create = persona_create("abc");
  const auto prepared = persona::prepare_persona_create(create);
  REQUIRE(prepared);
  REQUIRE(prepared->reference.persona_id ==
          make_id<domain::PersonaId>("persona:reviewer"));
  REQUIRE(prepared->reference.name == "Reviewer");
  REQUIRE(prepared->reference.source_location == "personas/Reviewer.md");
  REQUIRE(
      prepared->reference.content_digest ==
      domain::ContentDigest{
          "sha256",
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          3});

  const persona::PersonaWriteReceipt created{std::nullopt, prepared->reference};
  REQUIRE(persona::validate_persona_write_receipt(create, created));

  const persona::PersonaReplace replace{prepared->reference, "updated", {}};
  const auto replacement = persona::prepare_persona_replace(replace);
  REQUIRE(replacement);
  REQUIRE(replacement->reference.persona_id == prepared->reference.persona_id);
  REQUIRE(replacement->reference.name == prepared->reference.name);
  REQUIRE(replacement->reference.source_location ==
          prepared->reference.source_location);
  REQUIRE(replacement->reference.content_digest.byte_size == 7);
  REQUIRE(replacement->reference.content_digest !=
          prepared->reference.content_digest);
  REQUIRE(persona::validate_persona_write_receipt(
      replace, {prepared->reference, replacement->reference}));

  auto text_create = create;
  text_create.draft.file_kind = persona::PersonaFileKind::text;
  const auto text = persona::prepare_persona_create(text_create);
  REQUIRE(text);
  REQUIRE(text->reference.source_location == "personas/Reviewer.txt");
}

TEST_CASE("persona write preparation rejects malformed input before mutation",
          "[persona][editor][failure]") {
  auto request = persona_create();

  request.draft.name = "../Reviewer";
  auto rejected = persona::prepare_persona_create(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          persona::PersonaEditorErrorCode::invalid_name);
  REQUIRE_FALSE(rejected.error().may_have_applied);

  request = persona_create();
  request.draft.file_kind = static_cast<persona::PersonaFileKind>(99);
  rejected = persona::prepare_persona_create(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          persona::PersonaEditorErrorCode::invalid_file_kind);

  for (const auto& malformed :
       {std::string{}, std::string{"bad\0text", 8}, std::string{"\xc3", 1},
        std::string{"\xc2\x85", 2}, std::string{"\xe2\x80\xae", 3}}) {
    request = persona_create(malformed);
    rejected = persona::prepare_persona_create(request);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code ==
            persona::PersonaEditorErrorCode::malformed_text);
  }

  request = persona_create("abc");
  request.limits.maximum_file_bytes = 2;
  rejected = persona::prepare_persona_create(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          persona::PersonaEditorErrorCode::resource_exhausted);

  request = persona_create();
  request.limits.maximum_name_bytes = 0;
  rejected = persona::prepare_persona_create(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          persona::PersonaEditorErrorCode::invalid_request);

  const auto valid = persona::prepare_persona_create(persona_create());
  REQUIRE(valid);
  auto malformed_reference = valid->reference;
  malformed_reference.persona_id =
      make_id<domain::PersonaId>("persona:someone-else");
  const auto replacement = persona::prepare_persona_replace(
      {std::move(malformed_reference), "replacement", {}});
  REQUIRE_FALSE(replacement);
  REQUIRE(replacement.error().code ==
          persona::PersonaEditorErrorCode::invalid_request);
  REQUIRE_FALSE(replacement.error().may_have_applied);
}

TEST_CASE("persona receipt validation marks uncertain postconditions",
          "[persona][editor][failure]") {
  const auto create = persona_create();
  const auto prepared = persona::prepare_persona_create(create);
  REQUIRE(prepared);

  auto wrong = prepared->reference;
  wrong.content_digest.value = std::string(64, 'f');
  const auto invalid_create =
      persona::validate_persona_write_receipt(create, {std::nullopt, wrong});
  REQUIRE_FALSE(invalid_create);
  REQUIRE(invalid_create.error().code ==
          persona::PersonaEditorErrorCode::internal_failure);
  REQUIRE(invalid_create.error().may_have_applied);

  const persona::PersonaReplace replace{prepared->reference, "replacement", {}};
  const auto replacement = persona::prepare_persona_replace(replace);
  REQUIRE(replacement);
  const auto invalid_replace = persona::validate_persona_write_receipt(
      replace, {std::nullopt, replacement->reference});
  REQUIRE_FALSE(invalid_replace);
  REQUIRE(invalid_replace.error().may_have_applied);
}

TEST_CASE("scripted persona editor is exact cancellable and observable",
          "[persona][editor][fake][failure]") {
  const auto create = persona_create("original");
  const auto original = persona::prepare_persona_create(create);
  REQUIRE(original);
  const persona::PersonaWriteReceipt created{std::nullopt, original->reference};
  const persona::PersonaReplace replace{original->reference, "replacement", {}};
  const auto replacement = persona::prepare_persona_replace(replace);
  REQUIRE(replacement);
  const persona::PersonaWriteReceipt replaced{original->reference,
                                              replacement->reference};

  testing::ScriptedPersonaEditor editor{
      {{create, testing::PersonaWriteOutcome{created}}},
      {{replace, testing::PersonaWriteOutcome{replaced}}}};
  REQUIRE(editor.create(create) == created);
  REQUIRE(editor.replace(replace) == replaced);
  REQUIRE(editor.recorded_creates() ==
          std::vector<persona::PersonaCreate>{create});
  REQUIRE(editor.recorded_replaces() ==
          std::vector<persona::PersonaReplace>{replace});
  REQUIRE(editor.remaining_creates() == 0);
  REQUIRE(editor.remaining_replaces() == 0);

  const auto exhausted = editor.create(create);
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          persona::PersonaEditorErrorCode::internal_failure);

  std::stop_source cancellation;
  cancellation.request_stop();
  const auto cancelled = editor.replace(replace, cancellation.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code == persona::PersonaEditorErrorCode::cancelled);
  REQUIRE(editor.recorded_replaces().size() == 1);

  testing::ScriptedPersonaEditor invalid;
  auto malformed = create;
  malformed.draft.text.clear();
  const auto preflight = invalid.create(std::move(malformed));
  REQUIRE_FALSE(preflight);
  REQUIRE(preflight.error().code ==
          persona::PersonaEditorErrorCode::malformed_text);
  REQUIRE(invalid.recorded_creates().empty());

  const persona::PersonaEditorError concurrent{
      persona::PersonaEditorErrorCode::concurrent_change,
      "persona changed concurrently", original->reference, true, false};
  testing::ScriptedPersonaEditor failing{
      {{create, testing::PersonaWriteOutcome{concurrent}}}, {}};
  const auto failed = failing.create(create);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error() == concurrent);
}
