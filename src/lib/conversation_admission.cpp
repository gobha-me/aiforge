#include <aiforge/domain/conversation_admission.hpp>

#include <aiforge/detail/sha256.hpp>
#include <algorithm>
#include <concepts>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace aiforge::domain {
namespace {
using Code = ConversationAdmissionErrorCode;
using Result = std::expected<void, ConversationAdmissionError>;

auto failure(Code code, std::string message)
    -> std::unexpected<ConversationAdmissionError> {
  return std::unexpected(ConversationAdmissionError{code, std::move(message)});
}

auto valid_mode(ConversationMode mode) -> bool {
  return mode == ConversationMode::full || mode == ConversationMode::rolling;
}

auto valid_digest(const ContentDigest& digest, std::uint64_t maximum) -> bool {
  return digest.algorithm == "sha256" && digest.value.size() == 64 &&
         digest.byte_size > 0 && digest.byte_size <= maximum &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f');
         });
}

auto add(std::uint64_t& total, std::uint64_t value) -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

class Seal final {
 public:
  explicit Seal(std::size_t maximum) : m_maximum{maximum} {}
  auto field(std::string_view value) -> void {
    if (!m_valid) return;
    const auto prefix = std::to_string(value.size()) + ':';
    if (prefix.size() > m_maximum - m_bytes ||
        value.size() > m_maximum - m_bytes - prefix.size()) {
      m_valid = false;
      return;
    }
    m_hash.update(std::as_bytes(std::span{prefix.data(), prefix.size()}));
    m_hash.update(std::as_bytes(std::span{value.data(), value.size()}));
    m_bytes += prefix.size() + value.size();
  }
  auto number(std::uint64_t value) -> void { field(std::to_string(value)); }
  auto optional_text(const std::optional<std::string>& value) -> void {
    number(value.has_value());
    if (value) field(*value);
  }
  auto digest(const ContentDigest& value) -> void {
    field(value.algorithm);
    field(value.value);
    number(value.byte_size);
  }
  [[nodiscard]] auto finish()
      -> std::expected<ContentDigest, ConversationAdmissionError> {
    if (!m_valid)
      return failure(Code::resource_exhausted,
                     "conversation encoding exceeds its bound");
    return ContentDigest{"sha256", m_hash.finish(), m_bytes};
  }

 private:
  detail::Sha256 m_hash;
  std::size_t m_maximum;
  std::size_t m_bytes{};
  bool m_valid{true};
};

auto valid_optional_text(const std::optional<std::string>& value) -> bool {
  return !value || (!value->empty() &&
                    value->size() <= conversation_maximum_provenance_bytes);
}

auto header_shape(const ConversationAdmission& value) -> Result {
  if (value.version != 1 || value.estimator_version != 1)
    return failure(Code::unsupported_version,
                   "unsupported conversation admission version");
  if (!valid_mode(value.mode) ||
      value.policy_event_id.has_value() != (value.policy_revision != 0) ||
      (!value.policy_event_id && value.mode != ConversationMode::full))
    return failure(Code::invalid_policy,
                   "conversation admission policy is inconsistent");
  if (value.groups.size() > conversation_maximum_groups)
    return failure(Code::resource_exhausted,
                   "too many admitted conversation groups");
  if (value.omitted_groups_digest.has_value() !=
          (value.omitted_group_count != 0) ||
      (value.mode == ConversationMode::full &&
       value.omitted_group_count != 0) ||
      value.omitted_group_count > value.source_snapshot_sequence)
    return failure(Code::invalid_admission,
                   "conversation omission summary is inconsistent");
  if (value.omitted_groups_digest &&
      !valid_digest(*value.omitted_groups_digest,
                    std::numeric_limits<std::uint64_t>::max()))
    return failure(Code::invalid_digest,
                   "invalid conversation omission digest");
  return {};
}

struct Sources {
  std::set<RunId> runs;
  std::set<EventId> events;
  std::set<std::uint64_t> sequences;
  std::set<ContextEntryId> entries;
  std::set<MessageId> messages;
  std::set<ContextSourceId> source_ids;
  std::uint64_t previous_group{};
  std::uint64_t previous_order{};
  std::uint64_t tokens{};
  std::size_t count{};
  std::size_t pins{};

  auto entry(const ConversationAdmittedEntry& value, std::uint64_t snapshot)
      -> Result {
    if (value.event_sequence < previous_group ||
        value.event_sequence > snapshot || value.order <= previous_order ||
        value.estimated_tokens == 0 ||
        (value.kind != ContextContentKind::conversation &&
         value.kind != ContextContentKind::tool_result))
      return failure(Code::invalid_admission,
                     "conversation entry order or classification is invalid");
    if (!valid_optional_text(value.provenance.source_location) ||
        !valid_optional_text(value.provenance.digest))
      return failure(Code::invalid_admission,
                     "conversation provenance is invalid or oversized");
    if (!valid_digest(value.message_digest, conversation_maximum_message_bytes))
      return failure(Code::invalid_digest,
                     "conversation message digest is invalid");
    if (!events.insert(value.completed_event_id).second ||
        !sequences.insert(value.event_sequence).second ||
        !entries.insert(value.entry_id).second ||
        !messages.insert(value.message_id).second ||
        !source_ids.insert(value.provenance.source_id).second)
      return failure(Code::invalid_admission,
                     "conversation source identities are duplicated");
    if (!add(tokens, value.estimated_tokens))
      return failure(Code::token_overflow, "conversation estimates overflow");
    previous_order = value.order;
    return {};
  }

  auto group(const ConversationAdmittedGroup& value,
             const ConversationAdmission& admission) -> Result {
    if (value.entries.empty() || !runs.insert(value.run_id).second ||
        value.entries.front().kind != ContextContentKind::conversation ||
        value.entries.front().event_sequence <= previous_group ||
        (value.pinned && !admission.policy_event_id))
      return failure(Code::invalid_admission, "conversation group is invalid");
    if (value.entries.size() > conversation_maximum_entries - count)
      return failure(Code::resource_exhausted,
                     "too many conversation source entries");
    count += value.entries.size();
    if (value.pinned && ++pins > conversation_maximum_pins)
      return failure(Code::resource_exhausted, "too many conversation pins");
    previous_group = value.entries.front().event_sequence;
    for (const auto& item : value.entries) {
      auto valid = entry(item, admission.source_snapshot_sequence);
      if (!valid) return valid;
    }
    return {};
  }
};

auto shape(const ConversationAdmission& value) -> Result {
  auto valid = header_shape(value);
  if (!valid) return valid;
  Sources sources;
  for (const auto& group : value.groups) {
    valid = sources.group(group, value);
    if (!valid) return valid;
  }
  auto total = value.mandatory_input_tokens;
  if (!add(total, sources.tokens) ||
      !add(total, value.capacity.reserved_input_tokens) ||
      !add(total, value.capacity.reserved_output_tokens))
    return failure(Code::token_overflow,
                   "conversation capacity accounting overflows");
  if (value.capacity.context_window_tokens == 0 ||
      total > value.capacity.context_window_tokens)
    return failure(Code::capacity_exceeded,
                   "admitted conversation exceeds context capacity");
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

auto encoded_admission(const ConversationAdmission& value)
    -> std::expected<ContentDigest, ConversationAdmissionError> {
  Seal seal{conversation_maximum_manifest_bytes};
  seal.field("aiforge.conversation-admission.v1");
  seal.number(value.version);
  seal.number(value.estimator_version);
  seal.field(value.session_id.value());
  seal.field(value.model_id.value());
  seal.number(value.source_snapshot_sequence);
  seal.number(value.policy_event_id.has_value());
  if (value.policy_event_id) seal.field(value.policy_event_id->value());
  seal.number(value.policy_revision);
  seal.number(static_cast<std::uint64_t>(value.mode));
  seal.number(value.capacity.context_window_tokens);
  seal.number(value.capacity.reserved_output_tokens);
  seal.number(value.capacity.reserved_input_tokens);
  seal.number(value.mandatory_input_tokens);
  seal.number(value.groups.size());
  for (const auto& group : value.groups) {
    seal.field(group.run_id.value());
    seal.number(group.pinned);
    seal.number(group.entries.size());
    for (const auto& entry : group.entries)
      encode_entry(seal, entry);
  }
  seal.number(value.omitted_group_count);
  seal.number(value.omitted_groups_digest.has_value());
  if (value.omitted_groups_digest) seal.digest(*value.omitted_groups_digest);
  return seal.finish();
}

auto encode_block(Seal& seal, const ContentBlock& block) -> bool {
  return std::visit(
      [&seal](const auto& value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, TextBlock>) {
          seal.field("text");
          seal.field(value.text);
        } else if constexpr (std::same_as<T, StructuredDataBlock>) {
          seal.field("structured");
          seal.field(value.media_type);
          seal.field(value.data);
        } else if constexpr (std::same_as<T, CitationBlock>) {
          seal.field("citation");
          seal.field(value.uri);
          seal.optional_text(value.title);
        } else {
          return false;
        }
        return true;
      },
      block);
}

auto valid_message_shape(const Message& value) -> bool {
  if (value.content.empty() && value.tool_calls.empty()) return false;
  switch (value.role) {
    case Role::user:
    case Role::evidence:
      return !value.invocation_id && value.tool_calls.empty();
    case Role::assistant: return !value.invocation_id;
    case Role::tool:
      return value.invocation_id.has_value() && value.tool_calls.empty();
    default: return false;
  }
}

auto valid_call(const ToolCall& value) -> bool {
  return !value.tool_name.empty() && value.tool_name.size() <= 128 &&
         std::ranges::none_of(value.tool_name,
                              [](const unsigned char ch) {
                                return ch < 0x20U || ch == 0x7fU;
                              }) &&
         value.arguments.media_type == "application/json" &&
         !value.arguments.data.empty();
}

auto message_digest(const Message& value, std::uint32_t version)
    -> std::expected<ContentDigest, ConversationAdmissionError> {
  if (version != 1)
    return failure(Code::unsupported_version,
                   "unsupported conversation message encoding");
  if (value.content.size() > conversation_maximum_message_items ||
      value.tool_calls.size() >
          conversation_maximum_message_items - value.content.size())
    return failure(Code::resource_exhausted,
                   "conversation message has too many items");
  if (!valid_message_shape(value))
    return failure(Code::invalid_message,
                   "invalid normalized conversation message");
  Seal seal{conversation_maximum_message_bytes};
  seal.field("aiforge.conversation-message.v1");
  seal.number(version);
  seal.field(value.message_id.value());
  seal.number(static_cast<std::uint64_t>(value.role));
  seal.number(value.invocation_id.has_value());
  if (value.invocation_id) seal.field(value.invocation_id->value());
  seal.number(value.content.size());
  for (const auto& block : value.content)
    if (!encode_block(seal, block))
      return failure(Code::invalid_message,
                     "conversation content is unresolved or unsupported");
  std::set<InvocationId> invocations;
  seal.number(value.tool_calls.size());
  for (const auto& call : value.tool_calls) {
    if (!valid_call(call) || !invocations.insert(call.invocation_id).second)
      return failure(Code::invalid_message,
                     "conversation tool call is invalid or duplicated");
    seal.field(call.invocation_id.value());
    seal.field(call.tool_name);
    seal.field(call.arguments.media_type);
    seal.field(call.arguments.data);
  }
  return seal.finish();
}
} // namespace

auto validate_conversation_policy(const ConversationPolicy& policy) -> Result {
  try {
    if (!valid_mode(policy.mode) || policy.revision == 0)
      return failure(Code::invalid_policy,
                     "conversation policy mode or revision is invalid");
    if (policy.pinned_run_ids.size() > conversation_maximum_pins)
      return failure(Code::resource_exhausted,
                     "conversation policy has too many pins");
    std::set<RunId> pins;
    for (const auto& pin : policy.pinned_run_ids)
      if (!pins.insert(pin).second)
        return failure(Code::invalid_policy,
                       "conversation policy contains duplicate pins");
    return {};
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation policy validation failed");
  }
}

auto seal_conversation_admission(ConversationAdmission& admission) -> Result {
  try {
    auto valid = shape(admission);
    if (!valid) return valid;
    auto digest = encoded_admission(admission);
    if (!digest) return std::unexpected(digest.error());
    admission.admission_digest = std::move(*digest);
    return {};
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation admission sealing failed");
  }
}

auto validate_conversation_admission(const ConversationAdmission& admission)
    -> Result {
  try {
    auto valid = shape(admission);
    if (!valid) return valid;
    if (!admission.admission_digest ||
        !valid_digest(*admission.admission_digest,
                      conversation_maximum_manifest_bytes))
      return failure(Code::invalid_digest,
                     "conversation admission seal is missing or invalid");
    auto digest = encoded_admission(admission);
    if (!digest) return std::unexpected(digest.error());
    if (*digest != *admission.admission_digest)
      return failure(Code::invalid_digest,
                     "conversation admission integrity check failed");
    return {};
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation admission validation failed");
  }
}

auto validate_conversation_admission(const ConversationAdmission& admission,
                                     const SessionId& expected_session)
    -> Result {
  try {
    if (admission.session_id != expected_session)
      return failure(Code::foreign_scope,
                     "conversation admission belongs to another session");
    return validate_conversation_admission(admission);
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation admission scope validation failed");
  }
}

auto normalized_conversation_message_digest(const Message& message,
                                            std::uint32_t version)
    -> std::expected<ContentDigest, ConversationAdmissionError> {
  try {
    return message_digest(message, version);
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation message digest failed");
  }
}

} // namespace aiforge::domain
