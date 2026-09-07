#include <aiforge/domain/repository_context.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <set>
#include <span>
#include <string_view>

namespace aiforge::domain {
namespace {
using Result = std::expected<void, RepositoryContextError>;
auto failure(std::string message) -> std::unexpected<RepositoryContextError> {
  return std::unexpected(RepositoryContextError{
      RepositoryContextErrorCode::invalid_admission, std::move(message)});
}
auto opaque(const std::string_view value) -> bool {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](const unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == ':' || c == '.' || c == '_' ||
                  c == '-';
         });
}
auto path(const std::string_view value, const bool empty) -> bool {
  if (value.empty()) return empty;
  if (value.size() > repository_context_maximum_path_bytes ||
      !detail::is_safe_utf8_text(value) || value.front() == '/' ||
      value.back() == '/' ||
      value.find_first_of("\\:\r\n\t") != std::string_view::npos)
    return false;
  std::size_t start{};
  while (start < value.size()) {
    const auto end = value.find('/', start);
    const auto part =
        value.substr(start, end == std::string_view::npos ? end : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
  return false;
}
auto digest(const ContentDigest& value) -> bool {
  const auto length = value.algorithm == "git-sha1" ? 40U : 64U;
  return (value.algorithm == "sha256" || value.algorithm == "git-sha1" ||
          value.algorithm == "git-sha256") &&
         value.value.size() == length &&
         std::ranges::all_of(value.value, [](const unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
auto source(const RepositorySourceIdentity& value, const ContentDigest& text,
            const RepositorySnapshotIdentity& snapshot,
            const std::uint64_t maximum) -> bool {
  return value.snapshot == snapshot && !value.range &&
         path(value.relative_path, false) && digest(value.content_digest) &&
         digest(text) && text.algorithm == "sha256" &&
         value.content_digest.byte_size == text.byte_size &&
         text.byte_size <= maximum;
}
auto applies(const std::string_view subtree, const std::string_view target)
    -> bool {
  return subtree.empty() || target == subtree ||
         (target.starts_with(subtree) && target.size() > subtree.size() &&
          target[subtree.size()] == '/');
}
auto instruction_shape(const RepositoryContextInstruction& ref,
                       const RepositoryContextAdmission& admission) -> bool {
  const auto expected_path = ref.applicable_subtree.empty()
                                 ? "AGENTS.md"
                                 : ref.applicable_subtree + "/AGENTS.md";
  const auto specificity =
      ref.applicable_subtree.empty()
          ? 0U
          : static_cast<unsigned>(
                std::ranges::count(ref.applicable_subtree, '/') + 1);
  return source(ref.source, ref.text_digest, admission.source_snapshot,
                repository_context_maximum_instruction_bytes) &&
         path(ref.applicable_subtree, true) &&
         applies(ref.applicable_subtree, admission.target_subtree) &&
         ref.source.relative_path == expected_path &&
         ref.specificity == specificity && ref.order != 0 &&
         ref.estimated_tokens == ref.text_digest.byte_size &&
         ref.estimated_tokens != 0 &&
         detail::is_safe_utf8_text(ref.instruction_id.value());
}
auto evidence_shape(const RepositoryContextEvidence& ref,
                    const RepositoryContextAdmission& admission) -> bool {
  const bool decision =
      ref.decision == RepositoryContextDecision::admitted ||
      ref.decision == RepositoryContextDecision::omitted_budget ||
      ref.decision == RepositoryContextDecision::omitted_class_budget;
  return source(ref.source, ref.text_digest, admission.source_snapshot,
                repository_context_maximum_evidence_bytes) &&
         decision && ref.order != 0 &&
         ref.estimated_tokens ==
             std::max(std::uint64_t{1}, ref.text_digest.byte_size) &&
         detail::is_safe_utf8_text(ref.evidence_id.value()) &&
         ref.entry_id.value().starts_with("repository-context-entry-") &&
         ref.message_id.value().starts_with("repository-context-message-") &&
         ref.source_id.value().starts_with("repository-context-source-");
}
auto shape(const RepositoryContextAdmission& admission) -> Result {
  if (admission.version != 1 || !opaque(admission.root_binding) ||
      !path(admission.target_subtree, true) ||
      admission.selection_revision == 0 ||
      !digest(admission.source_snapshot.fingerprint) ||
      !detail::is_safe_utf8_text(
          admission.source_snapshot.repository_id.value()) ||
      admission.instructions.size() > repository_context_maximum_instructions ||
      admission.evidence.size() > repository_context_maximum_evidence ||
      admission.capacity.context_window_tokens == 0 ||
      admission.capacity.reserved_output_tokens >=
          admission.capacity.context_window_tokens ||
      admission.capacity.reserved_input_tokens >=
          admission.capacity.context_window_tokens -
              admission.capacity.reserved_output_tokens)
    return failure("repository context admission shape is invalid");
  std::set<std::string> identities;
  std::set<std::string> paths;
  std::uint64_t bytes{};
  std::uint64_t order{};
  std::optional<std::uint32_t> specificity;
  for (const auto& ref : admission.instructions) {
    if (!instruction_shape(ref, admission) || ref.order <= order ||
        (specificity && ref.specificity <= *specificity) ||
        !identities.insert(std::string{ref.instruction_id.value()}).second ||
        !paths.insert(ref.source.relative_path).second ||
        ref.text_digest.byte_size >
            repository_context_maximum_instruction_total_bytes - bytes)
      return failure("repository instruction references are invalid");
    bytes += ref.text_digest.byte_size;
    order = ref.order;
    specificity = ref.specificity;
  }
  paths.clear();
  bytes = 0;
  order = 0;
  std::set<EvidenceId> evidence_ids;
  std::set<MessageId> messages;
  std::set<ContextSourceId> sources;
  for (const auto& ref : admission.evidence) {
    if (!evidence_shape(ref, admission) || ref.order <= order ||
        !identities.insert(std::string{ref.entry_id.value()}).second ||
        !evidence_ids.insert(ref.evidence_id).second ||
        !messages.insert(ref.message_id).second ||
        !sources.insert(ref.source_id).second ||
        !paths.insert(ref.source.relative_path).second ||
        ref.text_digest.byte_size >
            repository_context_maximum_evidence_total_bytes - bytes)
      return failure("repository evidence references are invalid");
    order = ref.order;
    bytes += ref.text_digest.byte_size;
  }
  return {};
}
class Seal final {
 public:
  auto field(const std::string_view value) -> void {
    const auto prefix = std::to_string(value.size()) + ":";
    m_hash.update(std::as_bytes(std::span{prefix.data(), prefix.size()}));
    m_hash.update(std::as_bytes(std::span{value.data(), value.size()}));
    m_bytes += prefix.size() + value.size();
  }
  auto number(const std::uint64_t value) -> void {
    field(std::to_string(value));
  }
  auto content_digest(const ContentDigest& value) -> void {
    field(value.algorithm);
    field(value.value);
    number(value.byte_size);
  }
  auto snapshot(const RepositorySnapshotIdentity& value) -> void {
    field(value.repository_id.value());
    content_digest(value.fingerprint);
  }
  auto source_ref(const RepositorySourceIdentity& value) -> void {
    snapshot(value.snapshot);
    field(value.relative_path);
    content_digest(value.content_digest);
  }
  auto finish() -> ContentDigest {
    return {"sha256", m_hash.finish(), m_bytes};
  }

 private:
  detail::Sha256 m_hash;
  std::uint64_t m_bytes{};
};
auto seal(const RepositoryContextAdmission& value) -> ContentDigest {
  Seal hash;
  hash.field("aiforge.repository-context.v1");
  hash.number(value.version);
  hash.field(value.root_binding);
  hash.snapshot(value.source_snapshot);
  hash.field(value.target_subtree);
  hash.number(value.selection_revision);
  hash.number(value.capacity.context_window_tokens);
  hash.number(value.capacity.reserved_output_tokens);
  hash.number(value.capacity.reserved_input_tokens);
  hash.number(value.instructions.size());
  for (const auto& ref : value.instructions) {
    hash.field(ref.instruction_id.value());
    hash.source_ref(ref.source);
    hash.field(ref.applicable_subtree);
    hash.number(ref.specificity);
    hash.number(ref.order);
    hash.number(ref.estimated_tokens);
    hash.content_digest(ref.text_digest);
  }
  hash.number(value.evidence.size());
  for (const auto& ref : value.evidence) {
    hash.field(ref.evidence_id.value());
    hash.field(ref.entry_id.value());
    hash.field(ref.message_id.value());
    hash.field(ref.source_id.value());
    hash.source_ref(ref.source);
    hash.number(ref.order);
    hash.number(ref.estimated_tokens);
    hash.number(static_cast<std::uint64_t>(ref.decision));
    hash.content_digest(ref.text_digest);
  }
  return hash.finish();
}
auto text_matches(const Message& message, const Role role,
                  const ContentDigest& expected) -> bool {
  if (message.role != role || message.invocation_id ||
      !message.tool_calls.empty() || message.content.size() != 1)
    return false;
  const auto* text = std::get_if<TextBlock>(&message.content.front());
  if (text == nullptr || text->text.size() != expected.byte_size ||
      !detail::is_safe_utf8_text(text->text))
    return false;
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text->text.data(), text->text.size()}));
  return hash.finish() == expected.value;
}
auto provenance_matches(const ContextProvenance& provenance,
                        const RepositorySourceIdentity& source) -> bool {
  return provenance.source_location == source.relative_path &&
         provenance.digest == source.content_digest.algorithm + ":" +
                                  source.content_digest.value;
}
auto project_entry(const ContextEntry& entry) -> bool {
  return entry.instruction_layer == InstructionLayer::project ||
         entry.entry_id.value().starts_with("project:") ||
         entry.message.message_id.value().starts_with("project:") ||
         entry.provenance.source_id.value().starts_with("project:");
}
auto repository_entry(const ContextEntry& entry) -> bool {
  return entry.entry_id.value().starts_with("repository-context-") ||
         entry.message.message_id.value().starts_with("repository-context-") ||
         entry.provenance.source_id.value().starts_with("repository-context-");
}
auto matches_instruction(const ContextEntry& entry,
                         const RepositoryContextInstruction& ref) -> bool {
  return entry.kind == ContextEntryKind::instruction &&
         entry.instruction_layer == InstructionLayer::project &&
         entry.entry_id.value() == ref.instruction_id.value() &&
         entry.message.message_id.value() == ref.instruction_id.value() &&
         entry.provenance.source_id.value() == ref.instruction_id.value() &&
         entry.specificity == ref.specificity && entry.order == ref.order &&
         entry.estimated_tokens == ref.estimated_tokens &&
         provenance_matches(entry.provenance, ref.source) &&
         text_matches(entry.message, Role::system, ref.text_digest);
}
auto matches_evidence(const ContextEntry& entry,
                      const RepositoryContextEvidence& ref) -> bool {
  return entry.kind == ContextEntryKind::evidence && !entry.instruction_layer &&
         entry.specificity == 0 && entry.entry_id == ref.entry_id &&
         entry.message.message_id == ref.message_id &&
         entry.provenance.source_id == ref.source_id &&
         entry.order == ref.order &&
         entry.estimated_tokens == ref.estimated_tokens &&
         provenance_matches(entry.provenance, ref.source) &&
         text_matches(entry.message, Role::evidence, ref.text_digest);
}
auto next_admitted(const std::vector<RepositoryContextEvidence>& evidence,
                   std::size_t index) -> std::size_t {
  while (index < evidence.size() &&
         evidence[index].decision != RepositoryContextDecision::admitted)
    ++index;
  return index;
}
} // namespace

auto seal_repository_context_admission(RepositoryContextAdmission& admission)
    -> Result {
  try {
    if (auto valid = shape(admission); !valid) return valid;
    admission.admission_digest = seal(admission);
    return {};
  } catch (...) {
    return failure("repository context could not be sealed");
  }
}
auto validate_repository_context_admission(
    const RepositoryContextAdmission& admission) -> Result {
  try {
    if (auto valid = shape(admission); !valid) return valid;
    if (!admission.admission_digest ||
        *admission.admission_digest != seal(admission))
      return failure("repository context admission integrity check failed");
    return {};
  } catch (...) {
    return failure("repository context integrity could not be checked");
  }
}
auto repository_context_admission_matches_context(
    const RepositoryContextAdmission& admission,
    const ConstructedContext& context) -> bool {
  try {
    if (!validate_repository_context_admission(admission) ||
        admission.capacity != context.capacity)
      return false;
    std::size_t instruction{};
    std::size_t evidence{};
    for (const auto& entry : context.entries) {
      if (project_entry(entry)) {
        if (instruction >= admission.instructions.size() ||
            !matches_instruction(entry, admission.instructions[instruction]))
          return false;
        ++instruction;
      } else if (repository_entry(entry)) {
        evidence = next_admitted(admission.evidence, evidence);
        if (evidence >= admission.evidence.size() ||
            !matches_evidence(entry, admission.evidence[evidence]))
          return false;
        ++evidence;
      }
    }
    evidence = next_admitted(admission.evidence, evidence);
    return instruction == admission.instructions.size() &&
           evidence == admission.evidence.size();
  } catch (...) {
    return false;
  }
}
auto repository_context_admission_matches_context(
    const std::optional<RepositoryContextAdmission>& admission,
    const ConstructedContext& context) -> bool {
  if (admission)
    return repository_context_admission_matches_context(*admission, context);
  return std::ranges::none_of(context.entries, [](const auto& entry) {
    return project_entry(entry) || repository_entry(entry);
  });
}
auto repository_context_admission_successor(
    const RepositoryContextAdmission& previous,
    const RepositoryContextAdmission& next) -> bool {
  try {
    if (!validate_repository_context_admission(previous) ||
        !validate_repository_context_admission(next) ||
        previous.source_snapshot.repository_id !=
            next.source_snapshot.repository_id)
      return false;
    auto normalized = next;
    normalized.source_snapshot = previous.source_snapshot;
    for (auto& ref : normalized.instructions)
      ref.source.snapshot = previous.source_snapshot;
    for (auto& ref : normalized.evidence)
      ref.source.snapshot = previous.source_snapshot;
    normalized.admission_digest = previous.admission_digest;
    return normalized == previous;
  } catch (...) {
    return false;
  }
}
} // namespace aiforge::domain
