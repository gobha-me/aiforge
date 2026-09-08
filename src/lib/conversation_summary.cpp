#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/domain/conversation_summary.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {
using Code = ConversationSummaryErrorCode;
using Status = ConversationSummaryStatus;
auto fail(Code code, const char* message)
    -> std::unexpected<ConversationSummaryError> {
  return std::unexpected(ConversationSummaryError{code, message});
}
template <typename Function> auto guarded(Function&& function) -> Status {
  try {
    return std::forward<Function>(function)();
  } catch (...) {
    return fail(Code::internal_failure,
                "conversation summary operation failed internally");
  }
}
auto digest_shape(const ContentDigest& value,
                  std::uint64_t maximum = summary_maximum_manifest_bytes)
    -> bool {
  return value.algorithm == "sha256" && value.value.size() == 64 &&
         value.byte_size > 0 && value.byte_size <= maximum &&
         std::ranges::all_of(value.value, [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'f') || (ch >= '0' && ch <= '9');
         });
}
auto add(std::uint64_t& value, std::uint64_t amount) -> bool {
  if (amount > std::numeric_limits<std::uint64_t>::max() - value) return false;
  value += amount;
  return true;
}
class Seal final {
 public:
  auto field(std::string_view value) -> void {
    if (!m_valid) return;
    const auto prefix = std::to_string(value.size()) + ':';
    if (prefix.size() > summary_maximum_manifest_bytes - m_bytes ||
        value.size() >
            summary_maximum_manifest_bytes - m_bytes - prefix.size()) {
      m_valid = false;
      return;
    }
    m_hash.update(std::as_bytes(std::span{prefix.data(), prefix.size()}));
    m_hash.update(std::as_bytes(std::span{value.data(), value.size()}));
    m_bytes += prefix.size() + value.size();
  }
  auto number(std::uint64_t value) -> void { field(std::to_string(value)); }
  auto digest(const ContentDigest& value) -> void {
    field(value.algorithm);
    field(value.value);
    number(value.byte_size);
  }
  auto optional_text(const std::optional<std::string>& value) -> void {
    number(static_cast<std::uint64_t>(value.has_value()));
    if (value) field(*value);
  }
  auto finish() -> std::expected<ContentDigest, ConversationSummaryError> {
    if (!m_valid)
      return fail(Code::resource_exhausted,
                  "conversation summary manifest exceeds its bound");
    return ContentDigest{"sha256", m_hash.finish(), m_bytes};
  }

 private:
  detail::Sha256 m_hash;
  std::size_t m_bytes{};
  bool m_valid{true};
};
auto check_seal(
    const std::optional<ContentDigest>& actual,
    const std::expected<ContentDigest, ConversationSummaryError>& expected)
    -> Status {
  if (!expected) return std::unexpected(expected.error());
  if (!actual || !digest_shape(*actual) || *actual != *expected)
    return fail(Code::invalid_digest,
                "conversation summary integrity check failed");
  return {};
}
auto assign_seal(std::optional<ContentDigest>& target,
                 std::expected<ContentDigest, ConversationSummaryError> value)
    -> Status {
  if (!value) return std::unexpected(value.error());
  target = std::move(*value);
  return {};
}
auto valid_provenance(const ContextProvenance& value) -> bool {
  const auto valid = [](const std::optional<std::string>& text) {
    return !text || (!text->empty() &&
                     text->size() <= conversation_maximum_provenance_bytes);
  };
  return valid(value.source_location) && valid(value.digest);
}
struct SourceState {
  std::set<RunId> runs;
  std::set<EventId> events;
  std::set<std::uint64_t> sequences;
  std::set<ContextEntryId> entries;
  std::set<MessageId> messages;
  std::set<ContextSourceId> provenance;
  std::uint64_t last_group{};
  std::uint64_t last_order{};
  std::uint64_t source_bytes{};
  std::uint64_t tokens{};
  std::size_t count{};
  auto entry(const ConversationAdmittedEntry& value,
             const ConversationSummarySourceGroup& group) -> Status {
    if (value.event_sequence < last_group ||
        value.event_sequence >= group.terminal_sequence ||
        value.order <= last_order || value.estimated_tokens == 0 ||
        (value.kind != ContextContentKind::conversation &&
         value.kind != ContextContentKind::tool_result) ||
        !valid_provenance(value.provenance))
      return fail(Code::invalid_source, "summary source entry is invalid");
    if (!digest_shape(value.message_digest, summary_maximum_source_bytes))
      return fail(Code::invalid_digest,
                  "summary source message digest is invalid");
    if (value.message_digest.byte_size >
        summary_maximum_source_bytes - source_bytes)
      return fail(Code::resource_exhausted,
                  "summary source content exceeds its bound");
    source_bytes += value.message_digest.byte_size;
    if (!events.insert(value.completed_event_id).second ||
        !sequences.insert(value.event_sequence).second ||
        !entries.insert(value.entry_id).second ||
        !messages.insert(value.message_id).second ||
        !provenance.insert(value.provenance.source_id).second)
      return fail(Code::invalid_source,
                  "summary source identities are duplicated");
    if (!add(tokens, value.estimated_tokens))
      return fail(Code::token_overflow, "summary source estimates overflow");
    last_order = value.order;
    return {};
  }
  auto group(const ConversationSummarySourceGroup& value,
             std::uint64_t snapshot) -> Status {
    if (value.entries.empty() || !runs.insert(value.run_id).second ||
        value.entries.front().kind != ContextContentKind::conversation ||
        value.entries.front().event_sequence <= last_group ||
        value.terminal_sequence <= value.entries.front().event_sequence ||
        value.terminal_sequence > snapshot ||
        !events.insert(value.terminal_event_id).second ||
        !sequences.insert(value.terminal_sequence).second)
      return fail(Code::invalid_source, "summary source run is invalid");
    if (value.entries.size() > summary_maximum_entries - count)
      return fail(Code::resource_exhausted, "too many summary source entries");
    count += value.entries.size();
    last_group = value.entries.front().event_sequence;
    for (const auto& item : value.entries) {
      auto valid = entry(item, value);
      if (!valid) return valid;
    }
    return {};
  }
};
auto source_shape(const ConversationSummarySources& value) -> Status {
  if (value.version != 1 || value.estimator_version != 1)
    return fail(Code::unsupported_version,
                "unsupported summary source version");
  if (value.groups.empty() || value.snapshot_sequence == 0)
    return fail(Code::invalid_source, "summary source coverage is empty");
  if (value.groups.size() > summary_maximum_groups)
    return fail(Code::resource_exhausted, "too many summary source runs");
  SourceState state;
  for (const auto& group : value.groups) {
    auto valid = state.group(group, value.snapshot_sequence);
    if (!valid) return valid;
  }
  return {};
}
auto encode_entry(Seal& seal, const ConversationAdmittedEntry& value) -> void {
  seal.field(value.completed_event_id.value());
  seal.number(value.event_sequence);
  seal.field(value.entry_id.value());
  seal.field(value.message_id.value());
  seal.field(value.provenance.source_id.value());
  seal.optional_text(value.provenance.source_location);
  seal.optional_text(value.provenance.digest);
  seal.number(value.order);
  seal.number(value.estimated_tokens);
  seal.number(static_cast<std::uint64_t>(value.kind));
  seal.digest(value.message_digest);
}
auto encode_sources(const ConversationSummarySources& value)
    -> std::expected<ContentDigest, ConversationSummaryError> {
  Seal seal;
  seal.field("aiforge.summary-sources.v1");
  seal.number(value.version);
  seal.number(value.estimator_version);
  seal.field(value.session_id.value());
  seal.number(value.snapshot_sequence);
  seal.number(value.groups.size());
  for (const auto& group : value.groups) {
    seal.field(group.run_id.value());
    seal.field(group.terminal_event_id.value());
    seal.number(group.terminal_sequence);
    seal.number(group.entries.size());
    for (const auto& entry : group.entries)
      encode_entry(seal, entry);
  }
  return seal.finish();
}
auto valid_runtime_version(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return ch >= 0x20U && ch <= 0x7eU;
         });
}
auto source_tokens(const ConversationSummaryIntent& value)
    -> std::expected<std::uint64_t, ConversationSummaryError> {
  std::uint64_t source_tokens{};
  for (const auto& group : value.sources.groups) {
    if (group.run_id == value.producing_run_id)
      return fail(Code::invalid_intent, "summary cannot be its own source");
    for (const auto& entry : group.entries) {
      if (entry.message_id == value.output_message_id)
        return fail(Code::invalid_intent,
                    "summary output reuses a source identity");
      if (!add(source_tokens, entry.estimated_tokens))
        return fail(Code::token_overflow, "summary source estimates overflow");
    }
  }
  return source_tokens;
}
auto intent_shape(const ConversationSummaryIntent& value) -> Status {
  if (value.version != 1 || value.format_version != 1)
    return fail(Code::unsupported_version,
                "unsupported summary intent version");
  auto valid = validate_conversation_summary_sources(value.sources);
  if (!valid) return valid;
  if (!valid_runtime_version(value.runtime_version) ||
      value.maximum_output_bytes == 0 ||
      value.maximum_output_bytes > summary_maximum_text_bytes ||
      value.capacity.reserved_output_tokens == 0)
    return fail(Code::invalid_intent,
                "summary intent bounds or producer are invalid");
  const auto tokens = source_tokens(value);
  if (!tokens) return std::unexpected(tokens.error());
  if (value.estimated_input_tokens < *tokens)
    return fail(Code::invalid_intent, "summary intent omits source input cost");
  auto total = value.estimated_input_tokens;
  if (!add(total, value.capacity.reserved_input_tokens) ||
      !add(total, value.capacity.reserved_output_tokens))
    return fail(Code::token_overflow, "summary intent capacity overflows");
  if (value.capacity.context_window_tokens == 0 ||
      total > value.capacity.context_window_tokens)
    return fail(Code::capacity_exceeded, "summary source range does not fit");
  return {};
}
auto encode_intent(const ConversationSummaryIntent& value)
    -> std::expected<ContentDigest, ConversationSummaryError> {
  if (!value.sources.source_digest)
    return fail(Code::invalid_digest, "summary intent source seal is missing");
  Seal seal;
  seal.field("aiforge.summary-intent.v1");
  seal.number(value.version);
  seal.number(value.format_version);
  seal.field(value.summary_id.value());
  seal.digest(*value.sources.source_digest);
  seal.field(value.producing_run_id.value());
  seal.field(value.producing_inference_id.value());
  seal.field(value.model_id.value());
  seal.field(value.output_message_id.value());
  seal.field(value.runtime_version);
  seal.number(value.capacity.context_window_tokens);
  seal.number(value.capacity.reserved_input_tokens);
  seal.number(value.capacity.reserved_output_tokens);
  seal.number(value.estimated_input_tokens);
  seal.number(value.maximum_output_bytes);
  return seal.finish();
}
auto version_shape(const ConversationSummaryVersion& value) -> bool {
  return value.revision > 0 && digest_shape(value.candidate_digest);
}
auto encode_version(Seal& seal, const ConversationSummaryVersion& value)
    -> void {
  seal.field(value.summary_id.value());
  seal.number(value.revision);
  seal.digest(value.candidate_digest);
}
auto source_event(const ConversationSummarySources& sources,
                  const EventId& event) -> bool {
  return std::ranges::any_of(sources.groups, [&](const auto& group) {
    return group.terminal_event_id == event ||
           std::ranges::any_of(group.entries, [&](const auto& entry) {
             return entry.completed_event_id == event;
           });
  });
}
auto candidate_shape(const ConversationSummaryCandidate& value,
                     const ConversationSummaryIntent& intent) -> Status {
  if (!intent.sources.source_digest || !intent.intent_digest)
    return fail(Code::invalid_digest,
                "summary candidate intent seals are missing");
  if (value.version != 1)
    return fail(Code::unsupported_version,
                "unsupported summary candidate version");
  if (value.session_id != intent.sources.session_id)
    return fail(Code::foreign_scope,
                "summary candidate belongs to another session");
  if (value.summary_id != intent.summary_id ||
      value.source_digest != *intent.sources.source_digest ||
      value.intent_digest != *intent.intent_digest || value.revision == 0 ||
      value.output_sequence <= intent.sources.snapshot_sequence ||
      value.created_sequence <= value.output_sequence ||
      value.output_event_id == value.created_event_id)
    return fail(Code::invalid_candidate,
                "summary candidate source or producer is inconsistent");
  if (source_event(intent.sources, value.output_event_id) ||
      source_event(intent.sources, value.created_event_id))
    return fail(Code::invalid_candidate,
                "summary candidate reuses a source event identity");
  if (value.text.size() > intent.maximum_output_bytes)
    return fail(Code::resource_exhausted,
                "summary candidate exceeds its output bound");
  if (!detail::is_safe_utf8_text(value.text))
    return fail(Code::invalid_candidate,
                "summary candidate text is empty or unsafe");
  if (value.author == ConversationSummaryAuthor::model) {
    if (value.revision != 1 || value.edited_from)
      return fail(Code::invalid_candidate,
                  "model summary must be the initial candidate");
  } else if (value.author == ConversationSummaryAuthor::user_edit) {
    if (!value.edited_from || !version_shape(*value.edited_from) ||
        value.edited_from->summary_id != value.summary_id ||
        value.edited_from->revision ==
            std::numeric_limits<std::uint64_t>::max() ||
        value.revision != value.edited_from->revision + 1)
      return fail(Code::stale_revision,
                  "summary edit does not identify its previous version");
  } else
    return fail(Code::invalid_candidate,
                "summary candidate authorship is unsupported");
  return {};
}
auto encode_candidate(const ConversationSummaryCandidate& value)
    -> std::expected<ContentDigest, ConversationSummaryError> {
  Seal seal;
  seal.field("aiforge.summary-candidate.v1");
  seal.number(value.version);
  seal.field(value.session_id.value());
  seal.field(value.summary_id.value());
  seal.number(value.revision);
  seal.digest(value.source_digest);
  seal.digest(value.intent_digest);
  seal.field(value.output_event_id.value());
  seal.number(value.output_sequence);
  seal.field(value.created_event_id.value());
  seal.number(value.created_sequence);
  seal.number(static_cast<std::uint64_t>(value.author));
  seal.number(static_cast<std::uint64_t>(value.edited_from.has_value()));
  if (value.edited_from) encode_version(seal, *value.edited_from);
  seal.field(value.text);
  return seal.finish();
}
auto candidate_link(const ConversationSummaryCandidate& value,
                    const ConversationSummaryIntent& intent,
                    const ConversationSummaryCandidate* previous) -> Status {
  auto valid = validate_conversation_summary_intent(intent);
  if (!valid) return valid;
  valid = candidate_shape(value, intent);
  if (!valid) return valid;
  if (value.author == ConversationSummaryAuthor::model) {
    if (previous != nullptr)
      return fail(Code::invalid_candidate,
                  "generated summary cannot have an edit parent");
    return {};
  }
  if (previous == nullptr)
    return fail(Code::stale_revision, "summary edit parent is unavailable");
  // Resolve one exact immutable parent, not an unbounded recursive edit chain.
  // Its original generation intent and source coverage remain unchanged.
  valid = candidate_shape(*previous, intent);
  if (!valid) return valid;
  valid = check_seal(previous->candidate_digest, encode_candidate(*previous));
  if (!valid) return valid;
  const ConversationSummaryVersion expected{
      previous->summary_id, previous->revision, *previous->candidate_digest};
  if (*value.edited_from != expected ||
      value.created_sequence <= previous->created_sequence ||
      value.created_event_id == previous->created_event_id ||
      value.output_event_id != previous->output_event_id ||
      value.output_sequence != previous->output_sequence)
    return fail(Code::stale_revision,
                "summary edit parent or original provenance changed");
  return {};
}
auto activation_shape(const ConversationSummaryActivation& value) -> Status {
  if (value.version != 1)
    return fail(Code::unsupported_version,
                "unsupported summary activation version");
  if (!version_shape(value.candidate) || !digest_shape(value.source_digest) ||
      value.covered_run_ids.empty() || value.source_anchor_sequence == 0 ||
      value.activation_sequence <= value.source_anchor_sequence)
    return fail(Code::invalid_activation,
                "summary activation reference is invalid");
  if (value.covered_run_ids.size() > summary_maximum_groups)
    return fail(Code::resource_exhausted,
                "summary activation coverage exceeds its bound");
  std::set<RunId> runs;
  for (const auto& run : value.covered_run_ids)
    if (!runs.insert(run).second)
      return fail(Code::invalid_activation,
                  "summary activation duplicates source coverage");
  return {};
}
auto encode_activation(const ConversationSummaryActivation& value)
    -> std::expected<ContentDigest, ConversationSummaryError> {
  Seal seal;
  seal.field("aiforge.summary-activation.v1");
  seal.number(value.version);
  seal.field(value.session_id.value());
  encode_version(seal, value.candidate);
  seal.digest(value.source_digest);
  seal.number(value.covered_run_ids.size());
  for (const auto& run : value.covered_run_ids)
    seal.field(run.value());
  seal.number(value.source_anchor_sequence);
  seal.field(value.activation_event_id.value());
  seal.number(value.activation_sequence);
  return seal.finish();
}
auto activation_link(const ConversationSummaryActivation& value,
                     const ConversationSummaryCandidate& candidate,
                     const ConversationSummaryIntent& intent,
                     const ConversationSummaryCandidate* previous) -> Status {
  auto valid =
      validate_conversation_summary_candidate(candidate, intent, previous);
  if (!valid) return valid;
  valid = activation_shape(value);
  if (!valid) return valid;
  if (value.session_id != candidate.session_id)
    return fail(Code::foreign_scope,
                "summary activation belongs to another session");
  if (!candidate.candidate_digest)
    return fail(Code::invalid_digest,
                "summary activation candidate seal is missing");
  const ConversationSummaryVersion expected{
      candidate.summary_id, candidate.revision, *candidate.candidate_digest};
  if (value.candidate != expected ||
      value.source_digest != candidate.source_digest ||
      value.activation_sequence <= candidate.created_sequence ||
      value.activation_event_id == candidate.created_event_id ||
      value.activation_event_id == candidate.output_event_id ||
      value.source_anchor_sequence !=
          intent.sources.groups.front().entries.front().event_sequence ||
      value.covered_run_ids.size() != intent.sources.groups.size())
    return fail(Code::invalid_activation,
                "summary activation differs from its candidate coverage");
  if (source_event(intent.sources, value.activation_event_id))
    return fail(Code::invalid_activation,
                "summary activation reuses a source event identity");
  for (std::size_t index = 0; index < value.covered_run_ids.size(); ++index)
    if (value.covered_run_ids[index] != intent.sources.groups[index].run_id)
      return fail(Code::invalid_activation,
                  "summary activation substitutes source coverage");
  return {};
}

struct ActivationSet {
  const SessionId& session;
  std::set<ConversationSummaryId> summaries;
  std::set<RunId> runs;
  std::set<EventId> events;
  auto add(const ConversationSummaryActivation& value) -> Status {
    auto valid = validate_conversation_summary_activation(value);
    if (!valid) return valid;
    if (value.session_id != session)
      return fail(Code::foreign_scope,
                  "active summaries have different sessions");
    if (!summaries.insert(value.candidate.summary_id).second ||
        !events.insert(value.activation_event_id).second)
      return fail(Code::invalid_activation,
                  "active summary identity is duplicated");
    for (const auto& run : value.covered_run_ids)
      if (!runs.insert(run).second)
        return fail(Code::overlap, "active summary source coverage overlaps");
    return {};
  }
};
auto active_version(std::span<const ConversationSummaryActivation> active,
                    const ConversationSummaryVersion& version) -> bool {
  return std::ranges::any_of(
      active, [&](const auto& value) { return value.candidate == version; });
}
auto removed_versions(std::span<const ConversationSummaryActivation> active,
                      std::span<const ConversationSummaryVersion> replaced,
                      const ConversationSummaryActivation& replacement)
    -> std::expected<std::set<ConversationSummaryId>,
                     ConversationSummaryError> {
  std::set<ConversationSummaryId> removed;
  for (const auto& version : replaced) {
    if (!version_shape(version) || !removed.insert(version.summary_id).second ||
        !active_version(active, version))
      return fail(Code::stale_revision,
                  "summary replacement version is missing or stale");
    if (version.summary_id == replacement.candidate.summary_id &&
        replacement.candidate.revision <= version.revision)
      return fail(Code::stale_revision,
                  "summary replacement revision must advance");
  }
  return removed;
}
auto replacement_shape(std::span<const ConversationSummaryActivation> active,
                       std::span<const ConversationSummaryVersion> replaced,
                       const ConversationSummaryActivation& replacement)
    -> Status {
  if (active.size() > summary_maximum_active ||
      replaced.size() > summary_maximum_active)
    return fail(Code::resource_exhausted,
                "summary replacement set exceeds its bound");
  ActivationSet original{replacement.session_id, {}, {}, {}};
  for (const auto& value : active) {
    auto valid = original.add(value);
    if (!valid) return valid;
  }
  if (original.events.contains(replacement.activation_event_id) ||
      std::ranges::any_of(active, [&](const auto& value) {
        return value.activation_sequence >= replacement.activation_sequence;
      }))
    return fail(Code::invalid_activation,
                "summary replacement activation must be new");
  const auto removed = removed_versions(active, replaced, replacement);
  if (!removed) return std::unexpected(removed.error());
  if (active.size() - removed->size() >= summary_maximum_active)
    return fail(Code::resource_exhausted,
                "too many active summaries after replacement");
  ActivationSet final{replacement.session_id, {}, {}, {}};
  for (const auto& value : active)
    if (!removed->contains(value.candidate.summary_id)) {
      auto valid = final.add(value);
      if (!valid) return valid;
    }
  return final.add(replacement);
}
} // namespace

auto seal_conversation_summary_sources(ConversationSummarySources& value)
    -> Status {
  return guarded([&]() -> Status {
    auto valid = source_shape(value);
    if (!valid) return valid;
    return assign_seal(value.source_digest, encode_sources(value));
  });
}
auto validate_conversation_summary_sources(
    const ConversationSummarySources& value) -> Status {
  return guarded([&]() -> Status {
    auto valid = source_shape(value);
    if (!valid) return valid;
    return check_seal(value.source_digest, encode_sources(value));
  });
}
auto seal_conversation_summary_intent(ConversationSummaryIntent& value)
    -> Status {
  return guarded([&]() -> Status {
    auto valid = intent_shape(value);
    if (!valid) return valid;
    return assign_seal(value.intent_digest, encode_intent(value));
  });
}
auto validate_conversation_summary_intent(
    const ConversationSummaryIntent& value) -> Status {
  return guarded([&]() -> Status {
    auto valid = intent_shape(value);
    if (!valid) return valid;
    return check_seal(value.intent_digest, encode_intent(value));
  });
}
auto seal_conversation_summary_candidate(
    ConversationSummaryCandidate& value,
    const ConversationSummaryIntent& intent,
    const ConversationSummaryCandidate* previous) -> Status {
  return guarded([&]() -> Status {
    auto valid = candidate_link(value, intent, previous);
    if (!valid) return valid;
    return assign_seal(value.candidate_digest, encode_candidate(value));
  });
}
auto validate_conversation_summary_candidate(
    const ConversationSummaryCandidate& value,
    const ConversationSummaryIntent& intent,
    const ConversationSummaryCandidate* previous) -> Status {
  return guarded([&]() -> Status {
    auto valid = candidate_link(value, intent, previous);
    if (!valid) return valid;
    return check_seal(value.candidate_digest, encode_candidate(value));
  });
}
auto seal_conversation_summary_activation(
    ConversationSummaryActivation& value,
    const ConversationSummaryCandidate& candidate,
    const ConversationSummaryIntent& intent,
    const ConversationSummaryCandidate* previous) -> Status {
  return guarded([&]() -> Status {
    auto valid = activation_link(value, candidate, intent, previous);
    if (!valid) return valid;
    return assign_seal(value.activation_digest, encode_activation(value));
  });
}
auto validate_conversation_summary_activation(
    const ConversationSummaryActivation& value) -> Status {
  return guarded([&]() -> Status {
    auto valid = activation_shape(value);
    if (!valid) return valid;
    return check_seal(value.activation_digest, encode_activation(value));
  });
}
auto validate_conversation_summary_replacement(
    std::span<const ConversationSummaryActivation> active,
    std::span<const ConversationSummaryVersion> replaced,
    const ConversationSummaryActivation& replacement) -> Status {
  return guarded(
      [&] { return replacement_shape(active, replaced, replacement); });
}
} // namespace aiforge::domain
