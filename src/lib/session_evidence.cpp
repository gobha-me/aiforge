#include <aiforge/runtime/session_evidence.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <utility>

namespace aiforge::runtime {
namespace {
using namespace domain;
using Code = SessionEvidenceErrorCode;
using Status = std::expected<void, SessionEvidenceError>;
constexpr auto maximum_entries = conversation_maximum_entries;

auto failure(Code code) -> std::unexpected<SessionEvidenceError> {
  std::string message;
  switch (code) {
    case Code::invalid_base:
      message = "prepared session context does not match its saved admission";
      break;
    case Code::invalid_evidence:
      message = "selected evidence is invalid or does not match its exact "
                "preparation";
      break;
    case Code::resource_exhausted:
      message = "session evidence exceeds the bounded input limits";
      break;
    case Code::token_overflow:
      message = "session evidence order or token accounting overflowed";
      break;
    case Code::selection_failed:
      message = "required session context could not fit the final selection";
      break;
    case Code::admission_failed:
      message = "final evidence context does not match its exact admission";
      break;
    case Code::cancelled:
      message = "session evidence selection cancelled";
      break;
    case Code::internal_failure:
      message = "session evidence selection failed internally";
      break;
  }
  return std::unexpected(SessionEvidenceError{code, std::move(message)});
}
auto add(std::uint64_t& total, std::uint64_t amount) -> Status {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total)
    return failure(Code::token_overflow);
  total += amount;
  return {};
}

// Bounds consumed messages and metadata before any content-bearing copy.
struct Bounds {
  std::stop_token stop;
  std::uint64_t bytes{};
  std::size_t items{};
  auto charge(std::uint64_t amount) -> bool {
    if (stop.stop_requested() ||
        amount > conversation_maximum_message_bytes - bytes)
      return false;
    bytes += amount;
    return true;
  }
  auto provenance(const ContextProvenance& value) -> bool {
    return charge(value.source_id.value().size()) &&
           (!value.source_location || charge(value.source_location->size())) &&
           (!value.digest || charge(value.digest->size()));
  }
  auto message(const Message& value) -> bool {
    if (value.content.size() > conversation_maximum_message_items - items)
      return false;
    items += value.content.size();
    if (value.tool_calls.size() > conversation_maximum_message_items - items)
      return false;
    items += value.tool_calls.size();
    const auto estimated = estimate_conversation_message(value, 1, {}, stop);
    return estimated && charge(*estimated) &&
           charge(value.message_id.value().size());
  }
  auto content(const ContextContentInput& value) -> bool {
    return charge(value.entry_id.value().size()) &&
           provenance(value.provenance) && message(value.message);
  }
  auto instruction(const InstructionInput& value) -> bool {
    return charge(value.entry_id.value().size()) &&
           provenance(value.provenance) &&
           (!value.target_entry_id ||
            charge(value.target_entry_id->value().size())) &&
           (!value.message || message(*value.message));
  }
};

auto bounded_base(const PreparedSessionContext& base, Bounds& bounds)
    -> Status {
  if (base.input.instructions.size() > maximum_entries ||
      base.input.content.size() >
          maximum_entries - base.input.instructions.size())
    return failure(Code::resource_exhausted);
  for (const auto& instruction : base.input.instructions)
    if (!bounds.instruction(instruction))
      return failure(Code::resource_exhausted);
  std::set<std::uint64_t> orders;
  for (const auto& content : base.input.content) {
    if (!bounds.content(content)) return failure(Code::resource_exhausted);
    if (content.order == 0 || !orders.insert(content.order).second)
      return failure(Code::invalid_base);
  }
  if (!validate_conversation_admission(base.conversation_admission) ||
      !validate_memory_selection(base.memory_selection) ||
      base.input.capacity != base.conversation_admission.capacity)
    return failure(Code::invalid_base);
  return {};
}

template <typename Ref>
auto matches(const Ref& ref, const ContextEntry& entry, ContextEntryKind kind)
    -> bool {
  if (entry.kind != kind || entry.instruction_layer || entry.specificity != 0 ||
      entry.entry_id != ref.entry_id ||
      entry.message.message_id != ref.message_id ||
      entry.provenance != ref.provenance || entry.order != ref.order ||
      entry.estimated_tokens != ref.estimated_tokens)
    return false;
  const auto digest = normalized_conversation_message_digest(entry.message);
  return digest && *digest == ref.message_digest;
}

struct BaseProof {
  std::set<ContextEntryId> history;
  std::set<ContextEntryId> summaries;
};
auto validate_mandatory(const ConversationAdmission& admission,
                        const ConstructedContext& context,
                        const BaseProof& proof) -> Status {
  std::uint64_t mandatory{};
  std::size_t users{};
  for (const auto& entry : context.entries) {
    if (proof.history.contains(entry.entry_id)) continue;
    if (entry.kind == ContextEntryKind::conversation) {
      if (entry.message.role != Role::user || entry.message.invocation_id ||
          !entry.message.tool_calls.empty())
        return failure(Code::invalid_base);
      ++users;
    }
    if (entry.kind == ContextEntryKind::tool_result)
      return failure(Code::invalid_base);
    if (auto valid = add(mandatory, entry.estimated_tokens); !valid)
      return std::unexpected(valid.error());
  }
  if (users != 1 || mandatory != admission.mandatory_input_tokens)
    return failure(Code::invalid_base);
  return {};
}
auto validate_base(const PreparedSessionContext& base,
                   const ConstructedContext& context)
    -> std::expected<BaseProof, SessionEvidenceError> {
  if (!memory_selection_matches_context(base.memory_selection, context))
    return failure(Code::invalid_base);
  std::map<ContextEntryId, const ContextEntry*> entries;
  for (const auto& entry : context.entries)
    entries.emplace(entry.entry_id, &entry);
  BaseProof proof;
  for (const auto& group : base.conversation_admission.groups)
    for (const auto& ref : group.entries) {
      const auto found = entries.find(ref.entry_id);
      const auto kind = ref.kind == ContextContentKind::tool_result
                            ? ContextEntryKind::tool_result
                            : ContextEntryKind::conversation;
      if (found == entries.end() || !matches(ref, *found->second, kind))
        return failure(Code::invalid_base);
      proof.history.insert(ref.entry_id);
    }
  for (const auto& ref : base.conversation_admission.summaries) {
    const auto found = entries.find(ref.entry_id);
    if (found == entries.end() ||
        !matches(ref, *found->second, ContextEntryKind::evidence) ||
        found->second->message.role != Role::evidence ||
        found->second->message.invocation_id ||
        !found->second->message.tool_calls.empty())
      return failure(Code::invalid_base);
    proof.summaries.insert(ref.entry_id);
  }
  if (auto valid =
          validate_mandatory(base.conversation_admission, context, proof);
      !valid)
    return std::unexpected(valid.error());
  return proof;
}

auto bounded_digest(const ContentDigest& value) -> bool {
  return value.algorithm.size() <= 16 && value.value.size() <= 64;
}
auto bounded_source(const RepositorySourceIdentity& value) -> bool {
  return value.relative_path.size() <= repository_context_maximum_path_bytes &&
         bounded_digest(value.content_digest) &&
         bounded_digest(value.snapshot.fingerprint);
}
auto bounded_repository(const PreparedRepositoryContext& prepared,
                        const PreparedSessionContext& base, Bounds& bounds)
    -> Status {
  const auto& refs = prepared.admission;
  if (refs.admission_digest ||
      refs.instructions.size() > repository_context_maximum_instructions ||
      prepared.instructions.size() != refs.instructions.size() ||
      refs.evidence.size() > repository_context_maximum_evidence ||
      prepared.evidence.items.size() != refs.evidence.size() ||
      prepared.evidence.parcel.items.size() != refs.evidence.size() ||
      refs.root_binding.size() > 128 ||
      refs.target_subtree.size() > repository_context_maximum_path_bytes ||
      !bounded_digest(refs.source_snapshot.fingerprint))
    return failure(Code::invalid_evidence);
  for (const auto& ref : refs.instructions)
    if (!bounded_source(ref.source) || !bounded_digest(ref.text_digest) ||
        ref.applicable_subtree.size() > repository_context_maximum_path_bytes)
      return failure(Code::resource_exhausted);
  for (const auto& ref : refs.evidence)
    if (!bounded_source(ref.source) || !bounded_digest(ref.text_digest))
      return failure(Code::resource_exhausted);
  for (const auto& instruction : prepared.instructions)
    if (!bounds.instruction(instruction) ||
        std::ranges::find(base.input.instructions, instruction) ==
            base.input.instructions.end())
      return failure(Code::invalid_evidence);
  repository::ContextParcelLimits limits;
  limits.maximum_items = repository_context_maximum_evidence;
  limits.maximum_content_blocks_per_item = 1;
  limits.maximum_provenance_references = 64;
  limits.maximum_item_bytes = repository_context_maximum_evidence_bytes;
  limits.maximum_total_bytes = repository_context_maximum_evidence_total_bytes;
  limits.maximum_total_tokens = repository_context_maximum_evidence_total_bytes;
  if (!refs.evidence.empty() &&
      !repository::validate_context_parcel(prepared.evidence.parcel, limits))
    return failure(Code::invalid_evidence);
  auto shape = refs;
  shape.capacity = base.input.capacity;
  if (!seal_repository_context_admission(shape))
    return failure(Code::invalid_evidence);
  return {};
}

auto repository_rows(const PreparedRepositoryContext& prepared) -> Status {
  for (std::size_t index = 0; index < prepared.admission.evidence.size();
       ++index) {
    const auto& ref = prepared.admission.evidence[index];
    const auto& binding = prepared.evidence.items[index];
    const auto& item = prepared.evidence.parcel.items[index];
    const auto* exact = std::get_if<ExactSourceEvidence>(&item.reference);
    const auto* text = item.content.size() == 1
                           ? std::get_if<TextBlock>(&item.content.front())
                           : nullptr;
    if (binding.evidence_id != ref.evidence_id ||
        binding.entry_id != ref.entry_id ||
        binding.message_id != ref.message_id ||
        binding.source_id != ref.source_id || binding.order != ref.order ||
        binding.required || binding.alternative_group ||
        binding.budget_class != ContextBudgetClass::repository_evidence ||
        binding.representation != ContextRepresentation::exact ||
        item.evidence_id != ref.evidence_id ||
        item.freshness != EvidenceFreshness::current ||
        item.estimated_tokens != ref.estimated_tokens || exact == nullptr ||
        exact->source != ref.source || text == nullptr ||
        text->text.size() != ref.text_digest.byte_size)
      return failure(Code::invalid_evidence);
    detail::Sha256 hash;
    hash.update(std::as_bytes(std::span{text->text.data(), text->text.size()}));
    if (hash.finish() != ref.text_digest.value)
      return failure(Code::invalid_evidence);
  }
  return {};
}

auto bounded_local_content(const PreparedLocalContext& prepared, Bounds& bounds)
    -> Status {
  const LocalSourceLimits limits;
  std::uint64_t bytes{};
  for (std::size_t index = 0; index < prepared.candidates.size(); ++index) {
    const auto& ref = prepared.admission.evidence[index];
    const auto& value = prepared.candidates[index].content;
    if (!validate_local_source_identity(ref.source, limits))
      return failure(Code::invalid_evidence);
    const auto size = ref.source.content_digest.byte_size;
    if (size > limits.maximum_total_bytes - bytes)
      return failure(Code::resource_exhausted);
    bytes += size;
    const auto* text =
        value.message.content.size() == 1
            ? std::get_if<TextBlock>(&value.message.content.front())
            : nullptr;
    if (text == nullptr || text->text.size() != size ||
        value.message.invocation_id || !value.message.tool_calls.empty() ||
        (value.provenance.source_location &&
         value.provenance.source_location->size() >
             limits.maximum_path_bytes + 100) ||
        (value.provenance.digest && value.provenance.digest->size() > 80))
      return failure(Code::invalid_evidence);
    if (!bounds.content(value)) return failure(Code::resource_exhausted);
  }
  return {};
}

auto bounded_local(const PreparedLocalContext& prepared,
                   const PreparedSessionContext& base, Bounds& bounds)
    -> Status {
  const LocalSourceLimits limits;
  if (prepared.admission.admission_digest ||
      prepared.admission.session_id != base.conversation_admission.session_id ||
      prepared.admission.evidence.size() > limits.maximum_selected_files ||
      prepared.candidates.size() != prepared.admission.evidence.size())
    return failure(Code::invalid_evidence);
  if (auto valid = bounded_local_content(prepared, bounds); !valid)
    return valid;
  // Validate even omitted candidates before rebasing, using the same exact
  // finalization contract. No selector runs and no source is read here.
  ContextSelectionResult proof;
  proof.context.capacity = {std::numeric_limits<std::uint64_t>::max(), 0, 0};
  for (const auto& candidate : prepared.candidates) {
    const auto& value = candidate.content;
    proof.context.entries.push_back({value.entry_id,
                                     ContextEntryKind::evidence,
                                     {},
                                     value.message,
                                     value.provenance,
                                     0,
                                     value.order,
                                     value.estimated_tokens});
    proof.decisions.push_back({value.entry_id,
                               {},
                               ContextSelectionDecision::admitted,
                               value.estimated_tokens});
  }
  if (!finalize_local_context_admission(prepared, proof))
    return failure(Code::invalid_evidence);
  return {};
}

auto budget_class(const ContextContentInput& content, const BaseProof& proof)
    -> ContextBudgetClass {
  if (proof.summaries.contains(content.entry_id))
    return ContextBudgetClass::summary;
  if (content.entry_id.value().starts_with("memory-entry-"))
    return ContextBudgetClass::memory;
  if (content.kind == ContextContentKind::tool_result)
    return ContextBudgetClass::tool_result;
  if (content.kind == ContextContentKind::evidence)
    return ContextBudgetClass::attachment;
  return ContextBudgetClass::conversation;
}

auto request_for(const PreparedSessionContext& base, const BaseProof& proof)
    -> ContextSelectionRequest {
  ContextSelectionRequest request;
  request.capacity = base.input.capacity;
  request.instructions = base.input.instructions;
  request.selection_limits.maximum_candidates = maximum_entries;
  request.parcel_limits.maximum_item_bytes =
      repository_context_maximum_evidence_bytes;
  request.parcel_limits.maximum_total_bytes =
      repository_context_maximum_evidence_total_bytes;
  request.parcel_limits.maximum_total_tokens =
      repository_context_maximum_evidence_total_bytes;
  for (const auto& content : base.input.content)
    request.candidates.push_back({content,
                                  budget_class(content, proof),
                                  ContextRepresentation::direct,
                                  EvidenceFreshness::current,
                                  true,
                                  0,
                                  {}});
  return request;
}

auto append_repository(ContextSelectionRequest& request,
                       RepositoryContextAdmission& refs,
                       const ContextParcelSelection& evidence,
                       std::uint64_t& order) -> Status {
  if (refs.evidence.empty()) return {};
  auto parcel = evidence;
  for (std::size_t index = 0; index < refs.evidence.size(); ++index) {
    if (auto valid = add(order, 1); !valid) return valid;
    refs.evidence[index].order = order;
    parcel.items[index].order = order;
  }
  if (!parcel.items.empty()) request.parcels.push_back(std::move(parcel));
  return {};
}
auto append_local(ContextSelectionRequest& request, PreparedLocalContext& files,
                  std::uint64_t& order) -> Status {
  for (std::size_t index = 0; index < files.candidates.size(); ++index) {
    if (auto valid = add(order, 1); !valid) return valid;
    files.admission.evidence[index].order = order;
    files.candidates[index].content.order = order;
    files.candidates[index].relevance_rank = order;
    request.candidates.push_back(files.candidates[index]);
  }
  return {};
}

auto finish_session(PreparedSessionContext& session,
                    const ConstructedContext& context, const BaseProof& proof)
    -> Status {
  std::set<ContextEntryId> base_ids;
  for (const auto& entry : session.input.content)
    base_ids.insert(entry.entry_id);
  std::uint64_t mandatory{};
  for (const auto& entry : context.entries) {
    if (!proof.history.contains(entry.entry_id))
      if (auto valid = add(mandatory, entry.estimated_tokens); !valid)
        return valid;
    if (entry.kind != ContextEntryKind::instruction &&
        !base_ids.contains(entry.entry_id))
      session.input.content.push_back(
          {entry.entry_id, ContextContentKind::evidence, entry.message,
           entry.provenance, entry.order, entry.estimated_tokens});
  }
  session.conversation_admission.mandatory_input_tokens = mandatory;
  if (!seal_conversation_admission(session.conversation_admission) ||
      !memory_selection_matches_context(session.memory_selection, context))
    return failure(Code::admission_failed);
  return {};
}

auto preflight(const PreparedSessionContext& base,
               const std::optional<PreparedRepositoryContext>& repository,
               const std::optional<PreparedLocalContext>& local,
               std::stop_token stop) -> Status {
  if (stop.stop_requested()) return failure(Code::cancelled);
  Bounds bounds{stop};
  if (auto valid = bounded_base(base, bounds); !valid)
    return std::unexpected(valid.error());
  if (repository) {
    if (auto valid = bounded_repository(*repository, base, bounds); !valid)
      return std::unexpected(valid.error());
    if (auto valid = repository_rows(*repository); !valid)
      return std::unexpected(valid.error());
  }
  if (local)
    if (auto valid = bounded_local(*local, base, bounds); !valid)
      return std::unexpected(valid.error());
  const auto optional_count =
      (repository ? repository->admission.evidence.size() : 0) +
      (local ? local->candidates.size() : 0);
  if (optional_count > maximum_entries - base.input.content.size() -
                           base.input.instructions.size())
    return failure(Code::resource_exhausted);
  return {};
}

auto finalize(const PreparedSessionContext& base, const BaseProof& proof,
              std::optional<RepositoryContextAdmission> repo,
              const std::optional<PreparedLocalContext>& files,
              ContextSelectionResult selection, std::stop_token stop)
    -> std::expected<SelectedSessionEvidence, SessionEvidenceError> {
  if (repo) {
    auto finalized = finalize_repository_context_admission(*repo, selection);
    if (!finalized) return failure(Code::admission_failed);
    repo = std::move(*finalized);
  }
  std::optional<LocalContextAdmission> admitted_local;
  if (files) {
    auto finalized = finalize_local_context_admission(*files, selection);
    if (!finalized) return failure(Code::admission_failed);
    admitted_local = std::move(*finalized);
  }
  if (!repository_context_admission_matches_context(repo, selection.context) ||
      !local_context_admission_matches_context(admitted_local,
                                               selection.context))
    return failure(Code::admission_failed);
  auto session = base;
  if (auto valid = finish_session(session, selection.context, proof); !valid)
    return std::unexpected(valid.error());
  if (stop.stop_requested()) return failure(Code::cancelled);
  return SelectedSessionEvidence{std::move(session),
                                 std::move(selection.context),
                                 std::move(repo),
                                 std::move(admitted_local),
                                 std::move(selection.decisions),
                                 selection.usage};
}
auto select(const PreparedSessionContext& base,
            const std::optional<PreparedRepositoryContext>& repository,
            const std::optional<PreparedLocalContext>& local,
            std::stop_token stop)
    -> std::expected<SelectedSessionEvidence, SessionEvidenceError> {
  if (auto valid = preflight(base, repository, local, stop); !valid)
    return std::unexpected(valid.error());
  auto built = ContextBuilder{}.build(base.input);
  if (!built) return failure(Code::invalid_base);
  auto proof = validate_base(base, *built);
  if (!proof) return std::unexpected(proof.error());
  auto request = request_for(base, *proof);
  std::uint64_t order{};
  for (const auto& entry : base.input.content)
    order = std::max(order, entry.order);
  std::optional<RepositoryContextAdmission> repo;
  if (repository) {
    repo = repository->admission;
    if (auto valid =
            append_repository(request, *repo, repository->evidence, order);
        !valid)
      return std::unexpected(valid.error());
  }
  auto files = local;
  if (files)
    if (auto valid = append_local(request, *files, order); !valid)
      return std::unexpected(valid.error());
  if (stop.stop_requested()) return failure(Code::cancelled);
  auto selection = ContextBuilder{}.select_and_build(std::move(request));
  if (!selection) return failure(Code::selection_failed);
  return finalize(base, *proof, std::move(repo), files, std::move(*selection),
                  stop);
}

} // namespace

auto select_session_evidence(
    const PreparedSessionContext& base,
    const std::optional<PreparedRepositoryContext>& repository,
    const std::optional<PreparedLocalContext>& local, std::stop_token stop)
    -> std::expected<SelectedSessionEvidence, SessionEvidenceError> {
  try {
    auto result = select(base, repository, local, stop);
    if (stop.stop_requested()) return failure(Code::cancelled);
    return result;
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
} // namespace aiforge::runtime
