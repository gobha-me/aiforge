#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/memory.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <string_view>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto failure(
    const MemoryErrorCode code, std::string message,
    std::optional<MemoryProposalId> proposal_id = std::nullopt,
    std::optional<MemoryRecordId> record_id = std::nullopt)
    -> std::unexpected<MemoryError> {
  return std::unexpected(MemoryError{
      code, std::move(message), std::move(proposal_id), std::move(record_id)});
}

[[nodiscard]] auto valid_limits(const MemoryLimits& limits) -> bool {
  return limits.maximum_content_bytes != 0 &&
         limits.maximum_rationale_bytes != 0 &&
         limits.maximum_excerpt_bytes != 0 &&
         limits.maximum_source_events != 0 &&
         limits.maximum_relationships != 0 && limits.maximum_records != 0;
}

[[nodiscard]] auto lower_ascii(std::string value) -> std::string {
  std::ranges::transform(value, value.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

[[nodiscard]] auto shaped_like_secret(const std::string_view value) -> bool {
  const auto lower = lower_ascii(std::string{value});
  if (lower.contains("authorization:") || lower.contains("api_key=") ||
      lower.contains("api-key=") || lower.contains("access_token=") ||
      lower.contains("private key-----")) {
    return true;
  }
  const auto sk = lower.find("sk-");
  if (sk != std::string::npos && lower.size() - sk >= 12) return true;
  const auto bearer = lower.find("bearer ");
  return bearer != std::string::npos && lower.size() - bearer >= 16;
}

[[nodiscard]] auto valid_producer(const MemoryProducer& producer) -> bool {
  return !producer.runtime_name.empty() &&
         producer.runtime_name.size() <= 128 &&
         !producer.runtime_version.empty() &&
         producer.runtime_version.size() <= 128 &&
         memory_text_is_safe(producer.runtime_name) &&
         memory_text_is_safe(producer.runtime_version);
}

[[nodiscard]] auto valid_source(const MemorySource& source,
                                const MemoryLimits& limits) -> bool {
  if (source.event_ids.empty() ||
      source.event_ids.size() > limits.maximum_source_events) {
    return false;
  }
  return std::set<EventId>{source.event_ids.begin(), source.event_ids.end()}
             .size() == source.event_ids.size();
}

} // namespace

auto MemoryOwner::global() -> MemoryOwner {
  return {};
}

auto MemoryOwner::repository(RepositoryId repository_id) -> MemoryOwner {
  return {MemoryOwnerKind::repository, std::move(repository_id), std::nullopt};
}

auto MemoryOwner::persona(PersonaId persona_id) -> MemoryOwner {
  return {MemoryOwnerKind::persona, std::nullopt, std::move(persona_id)};
}

auto validate_memory_owner(const MemoryOwner& owner) noexcept -> bool {
  return (owner.kind == MemoryOwnerKind::global && !owner.repository_id &&
          !owner.persona_id) ||
         (owner.kind == MemoryOwnerKind::repository && owner.repository_id &&
          !owner.persona_id) ||
         (owner.kind == MemoryOwnerKind::persona && !owner.repository_id &&
          owner.persona_id);
}

auto memory_text_is_safe(const std::string_view value) -> bool {
  if (value.empty()) return false;
  return std::ranges::none_of(value, [](const unsigned char character) {
    return character == 0 || character == 0x1BU || character == 0x7FU ||
           (character < 0x20U && character != '\n' && character != '\t');
  });
}

auto memory_text_looks_secret(const std::string_view value) -> bool {
  return shaped_like_secret(value);
}

auto validate_memory_proposal(const MemoryProposal& proposal,
                              const MemoryLimits& limits)
    -> std::expected<void, MemoryError> {
  if (!valid_limits(limits)) {
    return failure(MemoryErrorCode::invalid_limits,
                   "memory limits must be positive");
  }
  if (!validate_memory_owner(proposal.owner)) {
    return failure(MemoryErrorCode::wrong_scope,
                   "memory proposal must have exactly one valid owner",
                   proposal.proposal_id, proposal.record_id);
  }
  if (proposal.content.size() > limits.maximum_content_bytes ||
      proposal.rationale.size() > limits.maximum_rationale_bytes ||
      proposal.evidence_excerpt.size() > limits.maximum_excerpt_bytes ||
      !memory_text_is_safe(proposal.content) ||
      !memory_text_is_safe(proposal.rationale) ||
      !memory_text_is_safe(proposal.evidence_excerpt) ||
      !valid_source(proposal.source, limits) ||
      !valid_producer(proposal.producer) ||
      proposal.overlap_record_ids.size() > limits.maximum_relationships) {
    return failure(MemoryErrorCode::invalid_record,
                   "memory proposal is malformed or exceeds its limits",
                   proposal.proposal_id, proposal.record_id);
  }
  if (shaped_like_secret(proposal.content) ||
      shaped_like_secret(proposal.rationale) ||
      shaped_like_secret(proposal.evidence_excerpt)) {
    return failure(MemoryErrorCode::secret_rejected,
                   "memory proposal contains credential-shaped content",
                   proposal.proposal_id, proposal.record_id);
  }
  std::set<MemoryRecordId> relationships{proposal.overlap_record_ids.begin(),
                                         proposal.overlap_record_ids.end()};
  if (relationships.size() != proposal.overlap_record_ids.size() ||
      relationships.contains(proposal.record_id) ||
      (proposal.replacement_record_id &&
       (*proposal.replacement_record_id == proposal.record_id ||
        !relationships.contains(*proposal.replacement_record_id)))) {
    return failure(MemoryErrorCode::invalid_record,
                   "memory proposal relationships are inconsistent",
                   proposal.proposal_id, proposal.record_id);
  }
  return {};
}

auto validate_memory_record(const MemoryRecord& record,
                            const MemoryLimits& limits)
    -> std::expected<void, MemoryError> {
  MemoryProposal proposal{record.proposal_id,
                          record.record_id,
                          record.owner,
                          record.kind,
                          record.content,
                          record.rationale,
                          "accepted-memory-source",
                          record.source,
                          record.producer,
                          std::nullopt,
                          {}};
  return validate_memory_proposal(proposal, limits);
}

auto memory_record_digest(const MemoryRecord& record)
    -> std::expected<ContentDigest, MemoryError> {
  try {
    if (auto valid = validate_memory_record(record); !valid)
      return std::unexpected(valid.error());
    detail::Sha256 hash;
    std::uint64_t size{};
    const auto add = [&](const std::string_view value) {
      const auto length = std::to_string(value.size()) + ":";
      hash.update(std::as_bytes(std::span{length.data(), length.size()}));
      hash.update(std::as_bytes(std::span{value.data(), value.size()}));
      size += length.size() + value.size();
    };
    add("aiforge.memory-record.v1");
    add(record.record_id.value());
    add(record.proposal_id.value());
    add(std::to_string(static_cast<int>(record.owner.kind)));
    add(record.owner.repository_id ? record.owner.repository_id->value() : "");
    add(record.owner.persona_id ? record.owner.persona_id->value() : "");
    add(std::to_string(static_cast<int>(record.kind)));
    add(record.content);
    add(record.rationale);
    add(record.source.session_id.value());
    add(record.source.run_id.value());
    add(record.source.invocation_id.value());
    add(std::to_string(record.source.event_ids.size()));
    for (const auto& id : record.source.event_ids)
      add(id.value());
    add(record.producer.model_id.value());
    add(record.producer.runtime_name);
    add(record.producer.runtime_version);
    return ContentDigest{"sha256", hash.finish(), size};
  } catch (...) {
    return failure(MemoryErrorCode::internal_failure,
                   "saved memory digest could not be constructed");
  }
}

auto memory_evidence_input(const MemoryRecord& record,
                           const std::uint64_t order)
    -> std::expected<ContextContentInput, MemoryError> {
  try {
    if (auto valid = validate_memory_record(record); !valid)
      return std::unexpected(valid.error());
    const auto owner = record.owner.kind == MemoryOwnerKind::persona ? "persona"
                       : record.owner.kind == MemoryOwnerKind::repository
                           ? "project"
                           : "global";
    std::string_view kind;
    switch (record.kind) {
      case MemoryKind::user_preference: kind = "user preference"; break;
      case MemoryKind::project_convention: kind = "project convention"; break;
      case MemoryKind::workflow: kind = "workflow"; break;
      case MemoryKind::reusable_fact: kind = "reusable fact"; break;
      case MemoryKind::unknown:
        return failure(MemoryErrorCode::invalid_record,
                       "unknown saved memory kind");
    }
    const auto id = std::string{record.record_id.value()};
    auto entry = ContextEntryId::from("memory-entry-" + id);
    auto message = MessageId::from("memory-message-" + id);
    auto source = ContextSourceId::from("memory-source-" + id);
    if (!entry || !message || !source || order == 0)
      return failure(MemoryErrorCode::invalid_record,
                     "saved memory context identity is invalid");
    auto text = "Saved " + std::string{owner} + " " + std::string{kind} + ": " +
                record.content;
    const auto tokens = text.size();
    return ContextContentInput{
        *entry,
        ContextContentKind::evidence,
        {*message, Role::evidence, {TextBlock{std::move(text)}}, std::nullopt},
        {*source,
         "memory:" + id +
             ";session:" + std::string{record.source.session_id.value()},
         std::nullopt},
        order,
        tokens};
  } catch (...) {
    return failure(MemoryErrorCode::internal_failure,
                   "saved memory context could not be constructed");
  }
}

auto validate_memory_selection(const MemorySelection& selection)
    -> std::expected<void, MemoryError> {
  try {
    // Bound the whole reference envelope as well as individual records.
    constexpr std::size_t maximum_entries = 4096;
    constexpr std::size_t maximum_source_references = 4096;
    if (selection.version != 1 || selection.entries.size() > maximum_entries)
      return failure(MemoryErrorCode::invalid_record,
                     "saved memory selection version or size is invalid");
    const auto valid_digest = [](const ContentDigest& value) {
      return value.algorithm == "sha256" && value.value.size() == 64 &&
             value.byte_size != 0 &&
             std::ranges::all_of(value.value, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             });
    };
    std::set<MemoryRecordId> records;
    std::uint64_t order{};
    std::uint64_t tokens{};
    std::size_t sources{};
    for (const auto& entry : selection.entries) {
      if (!validate_memory_owner(entry.owner) ||
          (entry.owner.kind == MemoryOwnerKind::repository &&
           entry.owner.repository_id != selection.repository_id) ||
          (entry.owner.kind == MemoryOwnerKind::persona &&
           entry.owner.persona_id != selection.persona_id) ||
          !records.insert(entry.record_id).second || entry.order <= order ||
          entry.estimated_tokens == 0 ||
          entry.estimated_tokens != entry.evidence_digest.byte_size ||
          !valid_digest(entry.record_digest) ||
          !valid_digest(entry.evidence_digest) ||
          entry.source.event_ids.empty() ||
          entry.source.event_ids.size() > 256 ||
          entry.source.event_ids.size() > maximum_source_references - sources ||
          entry.estimated_tokens >
              std::min(selection.maximum_tokens, selection.available_tokens) -
                  tokens)
        return failure(MemoryErrorCode::invalid_record,
                       "saved memory selection metadata is invalid");
      const std::set<EventId> source_ids{entry.source.event_ids.begin(),
                                         entry.source.event_ids.end()};
      if (source_ids.size() != entry.source.event_ids.size())
        return failure(
            MemoryErrorCode::invalid_record,
            "saved memory selection source identities are duplicated");
      order = entry.order;
      tokens += entry.estimated_tokens;
      sources += entry.source.event_ids.size();
    }
    return {};
  } catch (...) {
    return failure(MemoryErrorCode::internal_failure,
                   "saved memory selection validation failed");
  }
}

auto memory_selection_matches_context(const MemorySelection& selection,
                                      const ConstructedContext& context)
    -> bool {
  try {
    if (!validate_memory_selection(selection)) return false;
    std::size_t matched{};
    for (const auto& entry : context.entries) {
      const bool memory =
          entry.entry_id.value().starts_with("memory-entry-") ||
          (entry.provenance.source_location &&
           entry.provenance.source_location->starts_with("memory:"));
      if (!memory) continue;
      if (matched >= selection.entries.size()) return false;
      const auto& selected = selection.entries[matched++];
      const auto id = std::string{selected.record_id.value()};
      if (entry.kind != ContextEntryKind::evidence ||
          entry.message.role != Role::evidence || entry.message.invocation_id ||
          !entry.message.tool_calls.empty() ||
          entry.message.content.size() != 1 || entry.order != selected.order ||
          entry.estimated_tokens != selected.estimated_tokens ||
          entry.entry_id.value() != "memory-entry-" + id ||
          entry.message.message_id.value() != "memory-message-" + id ||
          entry.provenance.source_id.value() != "memory-source-" + id ||
          entry.provenance.source_location !=
              "memory:" + id + ";session:" +
                  std::string{selected.source.session_id.value()} ||
          entry.provenance.digest)
        return false;
      const auto* text = std::get_if<TextBlock>(&entry.message.content.front());
      if (text == nullptr ||
          text->text.size() != selected.evidence_digest.byte_size)
        return false;
      detail::Sha256 hash;
      hash.update(
          std::as_bytes(std::span{text->text.data(), text->text.size()}));
      if (hash.finish() != selected.evidence_digest.value) return false;
    }
    return matched == selection.entries.size();
  } catch (...) {
    return false;
  }
}

} // namespace aiforge::domain
