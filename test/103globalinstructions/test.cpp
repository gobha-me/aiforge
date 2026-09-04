#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/user_global_instruction.hpp>
#include <aiforge/instructions/editor.hpp>
#include <aiforge/runtime/user_global_instructions.hpp>
#include <aiforge/testing/scripted_user_global_instruction_source.hpp>

namespace {

using namespace aiforge;

template <typename Id> auto make_id(const std::string& value) -> Id {
  return Id::from(value).value();
}

auto document(std::string text = "Prefer concise answers.")
    -> domain::UserGlobalInstructionDocument {
  detail::Sha256 digest;
  digest.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {{make_id<domain::ContextSourceId>(
               std::string{domain::user_global_instruction_source_identity}),
           std::string{domain::user_global_instruction_source_location},
           {"sha256", digest.finish(), text.size()}},
          std::move(text)};
}

} // namespace

TEST_CASE("user-global instruction values reject noncanonical provenance",
          "[global-instructions][domain][failure]") {
  const auto valid = document();
  REQUIRE(domain::validate_user_global_instruction_reference(valid.reference));
  REQUIRE(domain::validate_user_global_instruction_document(valid));

  auto wrong_identity = valid;
  wrong_identity.reference.source_id =
      make_id<domain::ContextSourceId>("user-selected-source");
  REQUIRE_FALSE(
      domain::validate_user_global_instruction_document(wrong_identity));

  auto wrong_location = valid;
  wrong_location.reference.source_location = "../global.md";
  REQUIRE_FALSE(
      domain::validate_user_global_instruction_document(wrong_location));

  auto wrong_algorithm = valid;
  wrong_algorithm.reference.content_digest.algorithm = "sha512";
  REQUIRE_FALSE(
      domain::validate_user_global_instruction_document(wrong_algorithm));

  auto uppercase_digest = valid;
  uppercase_digest.reference.content_digest.value = std::string(64, 'A');
  REQUIRE_FALSE(
      domain::validate_user_global_instruction_document(uppercase_digest));

  auto empty_digest = valid;
  empty_digest.reference.content_digest.byte_size = 0;
  REQUIRE_FALSE(
      domain::validate_user_global_instruction_document(empty_digest));

  auto mismatched_size = valid;
  ++mismatched_size.reference.content_digest.byte_size;
  REQUIRE_FALSE(
      domain::validate_user_global_instruction_document(mismatched_size));

  auto mismatched_digest = valid;
  mismatched_digest.reference.content_digest.value = std::string(64, 'a');
  REQUIRE_FALSE(
      domain::validate_user_global_instruction_document(mismatched_digest));

  for (const auto& malformed :
       {std::string{}, std::string{"bad\0text", 8}, std::string{"\xc3", 1},
        std::string{"\xc2\x85", 2}, std::string{"\xe2\x80\xae", 3}}) {
    auto unsafe = document(malformed);
    REQUIRE_FALSE(domain::validate_user_global_instruction_document(unsafe));
  }
}

TEST_CASE("user-global writes fail before mutation on malformed requests",
          "[global-instructions][editor][failure]") {
  instructions::UserGlobalInstructionWrite request{std::nullopt, "abc", {3}};
  auto prepared = instructions::prepare_user_global_instruction_write(request);
  REQUIRE(prepared);
  REQUIRE(prepared->reference.content_digest.byte_size == 3);
  REQUIRE(prepared->reference.content_digest.value ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  request.text = "abcd";
  auto rejected = instructions::prepare_user_global_instruction_write(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(
      rejected.error().code ==
      instructions::UserGlobalInstructionEditorErrorCode::resource_exhausted);
  REQUIRE_FALSE(rejected.error().may_have_applied);

  for (const auto& malformed :
       {std::string{}, std::string{"bad\0text", 8}, std::string{"\xc3", 1},
        std::string{"\xc2\x85", 2}, std::string{"\xe2\x80\xae", 3}}) {
    request = {std::nullopt, malformed, {1024}};
    rejected = instructions::prepare_user_global_instruction_write(request);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code ==
            instructions::UserGlobalInstructionEditorErrorCode::malformed_text);
    REQUIRE_FALSE(rejected.error().may_have_applied);
  }

  request = {std::nullopt, "valid", {0}};
  rejected = instructions::prepare_user_global_instruction_write(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          instructions::UserGlobalInstructionEditorErrorCode::invalid_request);

  request.limits.maximum_file_bytes =
      instructions::UserGlobalInstructionLimits{}.maximum_file_bytes + 1;
  rejected = instructions::prepare_user_global_instruction_write(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          instructions::UserGlobalInstructionEditorErrorCode::invalid_request);

  request = {document().reference, "valid", {1024}};
  request.expected->source_location = "elsewhere.md";
  rejected = instructions::prepare_user_global_instruction_write(request);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          instructions::UserGlobalInstructionEditorErrorCode::invalid_request);
}

TEST_CASE("user-global write receipts bind exact create and replacement",
          "[global-instructions][editor][failure]") {
  const instructions::UserGlobalInstructionWrite create{
      std::nullopt, "abc", {1024}};
  const auto prepared =
      instructions::prepare_user_global_instruction_write(create);
  REQUIRE(prepared);
  const instructions::UserGlobalInstructionWriteReceipt created{
      std::nullopt, prepared->reference};
  REQUIRE(instructions::validate_user_global_instruction_write_receipt(
      create, created));

  instructions::UserGlobalInstructionWrite replace{
      prepared->reference, "updated", {1024}};
  const auto replacement =
      instructions::prepare_user_global_instruction_write(replace);
  REQUIRE(replacement);
  const instructions::UserGlobalInstructionWriteReceipt replaced{
      prepared->reference, replacement->reference};
  REQUIRE(instructions::validate_user_global_instruction_write_receipt(
      replace, replaced));

  auto stale = replaced;
  stale.previous.reset();
  const auto rejected =
      instructions::validate_user_global_instruction_write_receipt(replace,
                                                                   stale);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().may_have_applied);
}

TEST_CASE("user-global context input is fixed attributed system instruction",
          "[global-instructions][context][failure]") {
  const auto value = document();
  const auto input = runtime::user_global_instruction_input(value, 17, 4);
  REQUIRE(input);
  REQUIRE(input->entry_id ==
          make_id<domain::ContextEntryId>("user-global-instruction-entry"));
  REQUIRE(input->layer == domain::InstructionLayer::user_global);
  REQUIRE(input->operation == domain::InstructionOperation::add);
  REQUIRE_FALSE(input->target_entry_id);
  REQUIRE(input->message);
  REQUIRE(input->message->message_id ==
          make_id<domain::MessageId>("user-global-instruction-message"));
  REQUIRE(input->message->role == domain::Role::system);
  REQUIRE(std::get<domain::TextBlock>(input->message->content.front()).text ==
          value.text);
  REQUIRE(input->provenance.source_id == value.reference.source_id);
  REQUIRE(input->provenance.source_location == value.reference.source_location);
  REQUIRE(input->provenance.digest ==
          "sha256:" + value.reference.content_digest.value);
  REQUIRE(input->specificity == 0);
  REQUIRE(input->order == 4);
  REQUIRE(input->estimated_tokens == 17);

  REQUIRE_FALSE(runtime::user_global_instruction_input(value, 0, 1));
  REQUIRE_FALSE(runtime::user_global_instruction_input(value, 1, 0));

  auto malformed = value;
  malformed.reference.source_location = "other.md";
  const auto rejected = runtime::user_global_instruction_input(malformed, 1, 1);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::UserGlobalInstructionContextErrorCode::invalid_document);
}

TEST_CASE("scripted user-global source preserves optional missing and bounds",
          "[global-instructions][fake][failure]") {
  const auto value = document();
  testing::ScriptedUserGlobalInstructionSource source{{
      std::optional<domain::UserGlobalInstructionDocument>{},
      std::optional<domain::UserGlobalInstructionDocument>{value},
      instructions::UserGlobalInstructionError{
          instructions::UserGlobalInstructionErrorCode::io_failure,
          "scripted read failure", true},
  }};

  auto loaded = source.load({17});
  REQUIRE(loaded);
  REQUIRE_FALSE(*loaded);
  loaded = source.load({23});
  REQUIRE(loaded);
  REQUIRE(*loaded ==
          std::optional<domain::UserGlobalInstructionDocument>{value});
  REQUIRE(source.recorded_limits() ==
          std::vector<instructions::UserGlobalInstructionLimits>{{17}, {23}});

  const auto failed = source.load();
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code ==
          instructions::UserGlobalInstructionErrorCode::io_failure);
  REQUIRE(failed.error().retryable);
  REQUIRE(source.remaining_loads() == 0);

  const auto exhausted = source.load();
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          instructions::UserGlobalInstructionErrorCode::internal_failure);

  testing::ScriptedUserGlobalInstructionSource untouched{{
      std::optional<domain::UserGlobalInstructionDocument>{value},
  }};
  REQUIRE_FALSE(untouched.load({0}));
  REQUIRE(untouched.remaining_loads() == 1);

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped = untouched.load({}, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code ==
          instructions::UserGlobalInstructionErrorCode::cancelled);
  REQUIRE(untouched.remaining_loads() == 1);
}
