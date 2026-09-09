#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/domain/local_context.hpp>
#include <algorithm>
#include <set>
#include <span>
#include <string_view>

namespace aiforge::domain {
namespace {
using Code = LocalContextErrorCode;
using Status = std::expected<void, LocalContextError>;
auto failure(Code code, std::string message) -> Status {
  return std::unexpected(LocalContextError{code, std::move(message)});
}
auto reserved_id(std::string_view value, std::string_view prefix) -> bool {
  if (!value.starts_with(prefix)) return false;
  value.remove_prefix(prefix.size());
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= 'a' && character <= 'f') ||
                  (character >= '0' && character <= '9');
         });
}
auto valid_reference(const LocalContextEvidence& ref) -> bool {
  return reserved_id(ref.evidence_id.value(), "local-evidence-") &&
         reserved_id(ref.entry_id.value(), "local-context-entry-") &&
         reserved_id(ref.message_id.value(), "local-context-message-") &&
         reserved_id(ref.source_id.value(), "local-context-source-") &&
         ref.estimated_tokens == ref.source.content_digest.byte_size &&
         ref.estimated_tokens > 0 &&
         (ref.decision == LocalContextDecision::admitted ||
          ref.decision == LocalContextDecision::omitted_budget ||
          ref.decision == LocalContextDecision::omitted_class_budget);
}
auto shape(const LocalContextAdmission& value) -> Status {
  const LocalSourceLimits maximum;
  if (value.version != 1 || value.selection_revision == 0 ||
      !detail::is_safe_utf8_text(value.session_id.value()) ||
      value.capacity.context_window_tokens == 0 ||
      value.capacity.reserved_output_tokens >=
          value.capacity.context_window_tokens ||
      value.capacity.reserved_input_tokens >=
          value.capacity.context_window_tokens -
              value.capacity.reserved_output_tokens ||
      value.evidence.size() > maximum.maximum_selected_files)
    return failure(Code::invalid_admission, "local admission shape is invalid");
  std::set<std::string_view> ids;
  std::vector<LocalSourceIdentity> sources;
  std::uint64_t order{};
  for (const auto& ref : value.evidence) {
    if (!validate_local_source_identity(ref.source) || !valid_reference(ref) ||
        ref.order <= order || !ids.insert(ref.evidence_id.value()).second ||
        !ids.insert(ref.entry_id.value()).second ||
        !ids.insert(ref.message_id.value()).second ||
        !ids.insert(ref.source_id.value()).second)
      return failure(Code::invalid_admission,
                     "local admission references are invalid");
    order = ref.order;
    sources.push_back(ref.source);
  }
  if (!validate_local_source_selection(sources))
    return failure(Code::invalid_admission,
                   "local admission sources exceed bounds or repeat");
  return {};
}
class Seal final {
 public:
  auto field(std::string_view value) -> void {
    const auto count = std::to_string(value.size());
    append(count);
    append(":");
    append(value);
  }
  auto number(std::uint64_t value) -> void { field(std::to_string(value)); }
  [[nodiscard]] auto finish()
      -> std::expected<ContentDigest, LocalContextError> {
    if (!m_valid)
      return std::unexpected(LocalContextError{
          Code::resource_exhausted, "local admission metadata exceeds bound"});
    return ContentDigest{"sha256", m_hash.finish(), m_bytes};
  }

 private:
  auto append(std::string_view value) -> void {
    if (!m_valid ||
        value.size() > local_context_maximum_admission_bytes - m_bytes) {
      m_valid = false;
      return;
    }
    m_bytes += value.size();
    m_hash.update(std::as_bytes(std::span{value.data(), value.size()}));
  }
  detail::Sha256 m_hash;
  std::uint64_t m_bytes{};
  bool m_valid{true};
};
auto seal(const LocalContextAdmission& value)
    -> std::expected<ContentDigest, LocalContextError> {
  Seal hash;
  hash.field("aiforge.local-context.v1");
  hash.number(value.version);
  hash.field(value.session_id.value());
  hash.number(value.selection_revision);
  hash.number(value.capacity.context_window_tokens);
  hash.number(value.capacity.reserved_output_tokens);
  hash.number(value.capacity.reserved_input_tokens);
  hash.number(value.evidence.size());
  for (const auto& ref : value.evidence) {
    hash.field(ref.evidence_id.value());
    hash.field(ref.entry_id.value());
    hash.field(ref.message_id.value());
    hash.field(ref.source_id.value());
    hash.number(ref.source.root.version);
    hash.field(ref.source.root.binding);
    hash.field(ref.source.relative_path);
    hash.field(ref.source.content_digest.algorithm);
    hash.field(ref.source.content_digest.value);
    hash.number(ref.source.content_digest.byte_size);
    hash.number(ref.order);
    hash.number(ref.estimated_tokens);
    hash.number(static_cast<std::uint64_t>(ref.decision));
  }
  return hash.finish();
}
auto local_entry(const ContextEntry& entry) -> bool {
  return entry.entry_id.value().starts_with("local-context-") ||
         entry.message.message_id.value().starts_with("local-context-") ||
         entry.provenance.source_id.value().starts_with("local-context-") ||
         (entry.provenance.source_location &&
          entry.provenance.source_location->starts_with("local-file:"));
}
auto text_matches(const Message& message, const ContentDigest& digest) -> bool {
  if (message.role != Role::evidence || message.invocation_id ||
      !message.tool_calls.empty() || message.content.size() != 1)
    return false;
  const auto* text = std::get_if<TextBlock>(&message.content.front());
  if (text == nullptr || text->text.size() != digest.byte_size ||
      !detail::is_safe_utf8_text(text->text))
    return false;
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text->text.data(), text->text.size()}));
  return hash.finish() == digest.value;
}
auto matches(const ContextEntry& entry, const LocalContextEvidence& ref)
    -> bool {
  const auto location = local_context_source_location(ref.source);
  return entry.kind == ContextEntryKind::evidence && !entry.instruction_layer &&
         entry.specificity == 0 && entry.entry_id == ref.entry_id &&
         entry.message.message_id == ref.message_id &&
         entry.provenance.source_id == ref.source_id && location &&
         entry.provenance.source_location == *location &&
         entry.provenance.digest ==
             "sha256:" + ref.source.content_digest.value &&
         entry.order == ref.order &&
         entry.estimated_tokens == ref.estimated_tokens &&
         text_matches(entry.message, ref.source.content_digest);
}
auto next_admitted(const std::vector<LocalContextEvidence>& refs,
                   std::size_t index) -> std::size_t {
  while (index < refs.size() &&
         refs[index].decision != LocalContextDecision::admitted)
    ++index;
  return index;
}
} // namespace
auto local_context_source_location(const LocalSourceIdentity& source)
    -> std::expected<std::string, LocalContextError> {
  try {
    if (!validate_local_source_identity(source))
      return std::unexpected(LocalContextError{
          Code::invalid_admission, "local source location is invalid"});
    return "local-file:v1:" + source.root.binding + "/" + source.relative_path;
  } catch (...) {
    return std::unexpected(LocalContextError{Code::internal_failure,
                                             "local source location failed"});
  }
}
auto seal_local_context_admission(LocalContextAdmission& value) -> Status {
  try {
    if (auto valid = shape(value); !valid) return valid;
    auto digest = seal(value);
    if (!digest) return std::unexpected(digest.error());
    value.admission_digest = std::move(*digest);
    return {};
  } catch (...) {
    return failure(Code::internal_failure, "local admission sealing failed");
  }
}
auto validate_local_context_admission(const LocalContextAdmission& value)
    -> Status {
  try {
    if (auto valid = shape(value); !valid) return valid;
    if (!value.admission_digest)
      return failure(Code::invalid_admission,
                     "local admission seal is missing");
    auto digest = seal(value);
    if (!digest) return std::unexpected(digest.error());
    if (*digest != *value.admission_digest)
      return failure(Code::invalid_admission,
                     "local admission seal does not match");
    return {};
  } catch (...) {
    return failure(Code::internal_failure, "local admission validation failed");
  }
}
auto local_context_admission_matches_context(
    const LocalContextAdmission& admission, const ConstructedContext& context)
    -> bool {
  try {
    if (!validate_local_context_admission(admission) ||
        admission.capacity != context.capacity)
      return false;
    std::size_t index{};
    for (const auto& entry : context.entries) {
      if (!local_entry(entry)) continue;
      index = next_admitted(admission.evidence, index);
      if (index >= admission.evidence.size() ||
          !matches(entry, admission.evidence[index]))
        return false;
      ++index;
    }
    return next_admitted(admission.evidence, index) ==
           admission.evidence.size();
  } catch (...) {
    return false;
  }
}
auto local_context_admission_matches_context(
    const std::optional<LocalContextAdmission>& admission,
    const ConstructedContext& context) -> bool {
  if (admission)
    return local_context_admission_matches_context(*admission, context);
  return std::ranges::none_of(context.entries, local_entry);
}
auto local_context_admission_successor(const LocalContextAdmission& previous,
                                       const LocalContextAdmission& next)
    -> bool {
  return validate_local_context_admission(previous).has_value() &&
         validate_local_context_admission(next).has_value() && previous == next;
}
} // namespace aiforge::domain
