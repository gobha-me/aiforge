#include <aiforge/persona/editor.hpp>

#include <algorithm>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::persona {
namespace {

[[nodiscard]] auto failure(
    const PersonaEditorErrorCode code, std::string message,
    std::optional<domain::PersonaReference> observed = std::nullopt,
    const bool retryable = false, const bool may_have_applied = false)
    -> std::unexpected<PersonaEditorError> {
  return std::unexpected(PersonaEditorError{code, std::move(message),
                                            std::move(observed), retryable,
                                            may_have_applied});
}

[[nodiscard]] auto valid_limits(const PersonaLimits& limits) -> bool {
  constexpr PersonaLimits maximums;
  return limits.maximum_personas != 0 && limits.maximum_name_bytes != 0 &&
         limits.maximum_file_bytes != 0 &&
         limits.maximum_description_bytes != 0 &&
         limits.maximum_personas <= maximums.maximum_personas &&
         limits.maximum_name_bytes <= maximums.maximum_name_bytes &&
         limits.maximum_file_bytes <= maximums.maximum_file_bytes &&
         limits.maximum_description_bytes <= maximums.maximum_description_bytes;
}

[[nodiscard]] auto valid_name(const std::string_view value,
                              const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  const auto is_ascii_alnum = [](const unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (!is_ascii_alnum(static_cast<unsigned char>(value.front()))) return false;
  return std::ranges::all_of(value.substr(1),
                             [](const unsigned char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'A' && character <= 'Z') ||
                                      (character >= 'a' && character <= 'z') ||
                                      character == '-' || character == '_';
                             });
}

[[nodiscard]] auto canonical_name(std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::transform(result, result.begin(), [](const unsigned char ch) {
    return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
  });
  return result;
}

[[nodiscard]] auto extension(const PersonaFileKind kind)
    -> std::expected<std::string_view, PersonaEditorError> {
  switch (kind) {
    case PersonaFileKind::markdown: return ".md";
    case PersonaFileKind::text: return ".txt";
  }
  return failure(PersonaEditorErrorCode::invalid_file_kind,
                 "persona file kind is invalid");
}

[[nodiscard]] auto digest(const std::string_view text)
    -> domain::ContentDigest {
  detail::Sha256 sha256;
  sha256.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", sha256.finish(), text.size()};
}

[[nodiscard]] auto validate_text(const std::string_view text,
                                 const PersonaLimits& limits)
    -> std::expected<void, PersonaEditorError> {
  if (text.size() > limits.maximum_file_bytes) {
    return failure(PersonaEditorErrorCode::resource_exhausted,
                   "persona text exceeds its byte limit");
  }
  if (!detail::is_safe_utf8_text(text)) {
    return failure(
        PersonaEditorErrorCode::malformed_text,
        "persona text must be nonempty UTF-8 without unsafe controls");
  }
  return {};
}

[[nodiscard]] auto prepare_draft(const PersonaDraft& draft,
                                 const PersonaLimits& limits)
    -> std::expected<domain::PersonaDocument, PersonaEditorError> {
  if (!valid_limits(limits)) {
    return failure(PersonaEditorErrorCode::invalid_request,
                   "persona write limits are invalid");
  }
  if (!valid_name(draft.name, limits.maximum_name_bytes)) {
    return failure(PersonaEditorErrorCode::invalid_name,
                   "persona name must be a bounded bare name");
  }
  auto suffix = extension(draft.file_kind);
  if (!suffix) return std::unexpected(std::move(suffix.error()));
  auto valid = validate_text(draft.text, limits);
  if (!valid) return std::unexpected(std::move(valid.error()));
  auto persona_id =
      domain::PersonaId::from("persona:" + canonical_name(draft.name));
  if (!persona_id) {
    return failure(PersonaEditorErrorCode::invalid_name,
                   "persona identity cannot be represented");
  }
  domain::PersonaDocument document{
      {std::move(*persona_id), draft.name,
       "personas/" + draft.name + std::string{*suffix}, digest(draft.text)},
      draft.text};
  if (!domain::validate_persona_document(document)) {
    return failure(PersonaEditorErrorCode::invalid_request,
                   "prepared persona document is invalid");
  }
  return document;
}

[[nodiscard]] auto receipt_failure() -> std::unexpected<PersonaEditorError> {
  return failure(PersonaEditorErrorCode::internal_failure,
                 "persona write receipt is inconsistent", std::nullopt, false,
                 true);
}

} // namespace

auto prepare_persona_create(const PersonaCreate& request)
    -> std::expected<domain::PersonaDocument, PersonaEditorError> {
  try {
    return prepare_draft(request.draft, request.limits);
  } catch (...) {
    return failure(PersonaEditorErrorCode::internal_failure,
                   "persona creation preparation failed internally");
  }
}

auto prepare_persona_replace(const PersonaReplace& request)
    -> std::expected<domain::PersonaDocument, PersonaEditorError> {
  try {
    if (!valid_limits(request.limits) ||
        !domain::validate_persona_reference(request.expected) ||
        request.expected.name.size() > request.limits.maximum_name_bytes ||
        request.expected.content_digest.byte_size >
            request.limits.maximum_file_bytes) {
      return failure(PersonaEditorErrorCode::invalid_request,
                     "persona replacement precondition is invalid");
    }
    auto valid = validate_text(request.text, request.limits);
    if (!valid) return std::unexpected(std::move(valid.error()));
    auto reference = request.expected;
    reference.content_digest = digest(request.text);
    domain::PersonaDocument document{std::move(reference), request.text};
    if (!domain::validate_persona_document(document)) {
      return failure(PersonaEditorErrorCode::invalid_request,
                     "prepared persona replacement is invalid");
    }
    return document;
  } catch (...) {
    return failure(PersonaEditorErrorCode::internal_failure,
                   "persona replacement preparation failed internally");
  }
}

auto validate_persona_write_receipt(const PersonaCreate& request,
                                    const PersonaWriteReceipt& receipt)
    -> std::expected<void, PersonaEditorError> {
  try {
    auto prepared = prepare_persona_create(request);
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    if (receipt.previous || receipt.resulting != prepared->reference) {
      return receipt_failure();
    }
    return {};
  } catch (...) {
    return receipt_failure();
  }
}

auto validate_persona_write_receipt(const PersonaReplace& request,
                                    const PersonaWriteReceipt& receipt)
    -> std::expected<void, PersonaEditorError> {
  try {
    auto prepared = prepare_persona_replace(request);
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    if (receipt.previous != request.expected ||
        receipt.resulting != prepared->reference) {
      return receipt_failure();
    }
    return {};
  } catch (...) {
    return receipt_failure();
  }
}

} // namespace aiforge::persona
