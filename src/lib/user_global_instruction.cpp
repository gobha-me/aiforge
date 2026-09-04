#include <aiforge/domain/user_global_instruction.hpp>

#include <algorithm>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/instructions/editor.hpp>

namespace aiforge::domain {
namespace {

constexpr std::size_t maximum_document_bytes{std::size_t{1024} * 1024U};

[[nodiscard]] auto failure(const UserGlobalInstructionValidationErrorCode code,
                           std::string message)
    -> std::unexpected<UserGlobalInstructionValidationError> {
  return std::unexpected(
      UserGlobalInstructionValidationError{code, std::move(message)});
}

} // namespace

auto validate_user_global_instruction_reference(
    const UserGlobalInstructionReference& reference)
    -> std::expected<void, UserGlobalInstructionValidationError> {
  if (reference.source_id.value() != user_global_instruction_source_identity) {
    return failure(UserGlobalInstructionValidationErrorCode::invalid_identity,
                   "user-global instruction source identity is invalid");
  }
  if (reference.source_location != user_global_instruction_source_location) {
    return failure(UserGlobalInstructionValidationErrorCode::invalid_location,
                   "user-global instruction source location is invalid");
  }
  if (reference.content_digest.algorithm != "sha256" ||
      reference.content_digest.value.size() != 64 ||
      reference.content_digest.byte_size == 0 ||
      reference.content_digest.byte_size > maximum_document_bytes ||
      !std::ranges::all_of(reference.content_digest.value,
                           [](const unsigned char character) {
                             return (character >= '0' && character <= '9') ||
                                    (character >= 'a' && character <= 'f');
                           })) {
    return failure(UserGlobalInstructionValidationErrorCode::invalid_digest,
                   "user-global instruction content digest is invalid");
  }
  return {};
}

auto validate_user_global_instruction_document(
    const UserGlobalInstructionDocument& document)
    -> std::expected<void, UserGlobalInstructionValidationError> {
  if (auto valid =
          validate_user_global_instruction_reference(document.reference);
      !valid) {
    return valid;
  }
  if (document.reference.content_digest.byte_size != document.text.size() ||
      document.text.size() > maximum_document_bytes ||
      !detail::is_safe_utf8_text(document.text)) {
    return failure(UserGlobalInstructionValidationErrorCode::invalid_document,
                   "user-global instruction document text is invalid");
  }
  detail::Sha256 digest;
  digest.update(
      std::as_bytes(std::span{document.text.data(), document.text.size()}));
  if (digest.finish() != document.reference.content_digest.value) {
    return failure(UserGlobalInstructionValidationErrorCode::invalid_digest,
                   "user-global instruction content digest does not match "
                   "the document");
  }
  return {};
}

} // namespace aiforge::domain

namespace aiforge::instructions {
namespace {

[[nodiscard]] auto editor_failure(
    const UserGlobalInstructionEditorErrorCode code, std::string message,
    std::optional<domain::UserGlobalInstructionReference> observed =
        std::nullopt,
    const bool retryable = false, const bool may_have_applied = false)
    -> std::unexpected<UserGlobalInstructionEditorError> {
  return std::unexpected(UserGlobalInstructionEditorError{
      code, std::move(message), std::move(observed), retryable,
      may_have_applied});
}

[[nodiscard]] auto digest(const std::string_view text)
    -> domain::ContentDigest {
  detail::Sha256 value;
  value.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", value.finish(), text.size()};
}

[[nodiscard]] auto valid_limits(const UserGlobalInstructionLimits& limits)
    -> bool {
  constexpr UserGlobalInstructionLimits maximums;
  return limits.maximum_file_bytes != 0 &&
         limits.maximum_file_bytes <= maximums.maximum_file_bytes;
}

[[nodiscard]] auto resulting_document(const std::string& text)
    -> std::expected<domain::UserGlobalInstructionDocument,
                     UserGlobalInstructionEditorError> {
  auto source_id = domain::ContextSourceId::from(
      std::string{domain::user_global_instruction_source_identity});
  if (!source_id) {
    return editor_failure(
        UserGlobalInstructionEditorErrorCode::internal_failure,
        "user-global instruction identity cannot be represented");
  }
  domain::UserGlobalInstructionDocument document{
      {std::move(*source_id),
       std::string{domain::user_global_instruction_source_location},
       digest(text)},
      text};
  if (!domain::validate_user_global_instruction_document(document)) {
    return editor_failure(
        UserGlobalInstructionEditorErrorCode::internal_failure,
        "prepared user-global instruction document is invalid");
  }
  return document;
}

} // namespace

auto prepare_user_global_instruction_write(
    const UserGlobalInstructionWrite& request)
    -> std::expected<domain::UserGlobalInstructionDocument,
                     UserGlobalInstructionEditorError> {
  try {
    if (!valid_limits(request.limits) ||
        (request.expected &&
         !domain::validate_user_global_instruction_reference(
             *request.expected))) {
      return editor_failure(
          UserGlobalInstructionEditorErrorCode::invalid_request,
          "user-global instruction write request is invalid");
    }
    if (request.text.size() > request.limits.maximum_file_bytes) {
      return editor_failure(
          UserGlobalInstructionEditorErrorCode::resource_exhausted,
          "user-global instruction text exceeds its byte limit");
    }
    if (!detail::is_safe_utf8_text(request.text)) {
      return editor_failure(
          UserGlobalInstructionEditorErrorCode::malformed_text,
          "user-global instruction text must be nonempty UTF-8 without "
          "unsafe controls");
    }
    return resulting_document(request.text);
  } catch (...) {
    return editor_failure(
        UserGlobalInstructionEditorErrorCode::internal_failure,
        "user-global instruction write preparation failed internally");
  }
}

auto validate_user_global_instruction_write_receipt(
    const UserGlobalInstructionWrite& request,
    const UserGlobalInstructionWriteReceipt& receipt)
    -> std::expected<void, UserGlobalInstructionEditorError> {
  try {
    auto prepared = prepare_user_global_instruction_write(request);
    if (!prepared || receipt.previous != request.expected ||
        receipt.resulting != prepared->reference) {
      return editor_failure(
          UserGlobalInstructionEditorErrorCode::internal_failure,
          "user-global instruction write receipt is inconsistent",
          receipt.resulting, false, true);
    }
    return {};
  } catch (...) {
    return editor_failure(
        UserGlobalInstructionEditorErrorCode::internal_failure,
        "user-global instruction write receipt validation failed internally",
        receipt.resulting, false, true);
  }
}

} // namespace aiforge::instructions
