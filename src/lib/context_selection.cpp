#include <aiforge/runtime/context_builder.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aiforge::runtime {
namespace {

using namespace domain;

struct CandidateState {
  ContextContentInput content;
  ContextBudgetClass budget_class;
  ContextRepresentation representation;
  EvidenceFreshness freshness;
  bool required{};
  std::uint64_t relevance_rank{};
  std::optional<std::string> alternative_group;
  std::optional<EvidenceId> evidence_id;
  std::optional<RepositoryEvidenceReference> reference;
};

[[nodiscard]] auto failure(const ContextSelectionErrorCode code, std::string message,
                           std::optional<ContextEntryId> entry_id = std::nullopt,
                           std::optional<EvidenceId> evidence_id = std::nullopt)
    -> std::unexpected<ContextSelectionError> {
  return std::unexpected(
      ContextSelectionError{code, std::move(message), std::move(entry_id), std::move(evidence_id)});
}

[[nodiscard]] auto add_checked(std::uint64_t& total, const std::uint64_t value) noexcept -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

[[nodiscard]] auto valid_group(const std::optional<std::string>& group,
                               const std::size_t maximum_bytes) -> bool {
  if (!group) return true;
  if (group->empty() || group->size() > maximum_bytes) {
    return false;
  }
  return std::ranges::none_of(*group, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto valid_phase(const TaskPhase phase) noexcept -> bool {
  switch (phase) {
    case TaskPhase::orientation:
    case TaskPhase::diagnosis:
    case TaskPhase::editing:
    case TaskPhase::verification:
    case TaskPhase::review:
      return true;
    case TaskPhase::unknown:
      return false;
  }
  return false;
}

[[nodiscard]] auto known_budget_class(const ContextBudgetClass budget_class) noexcept -> bool {
  switch (budget_class) {
    case ContextBudgetClass::conversation:
    case ContextBudgetClass::summary:
    case ContextBudgetClass::tool_result:
    case ContextBudgetClass::repository_evidence:
    case ContextBudgetClass::attachment:
    case ContextBudgetClass::unknown:
      return true;
  }
  return false;
}

[[nodiscard]] auto known_representation(const ContextRepresentation representation) noexcept
    -> bool {
  switch (representation) {
    case ContextRepresentation::direct:
    case ContextRepresentation::exact:
    case ContextRepresentation::derived:
    case ContextRepresentation::excerpt:
    case ContextRepresentation::summary:
    case ContextRepresentation::artifact_reference:
    case ContextRepresentation::unknown:
      return true;
  }
  return false;
}

[[nodiscard]] auto known_freshness(const EvidenceFreshness freshness) noexcept -> bool {
  switch (freshness) {
    case EvidenceFreshness::current:
    case EvidenceFreshness::possibly_stale:
    case EvidenceFreshness::stale:
    case EvidenceFreshness::unavailable:
      return true;
  }
  return false;
}

[[nodiscard]] auto budget_limit(const ContextClassBudgets& budgets,
                                const ContextBudgetClass budget_class)
    -> std::optional<std::uint64_t> {
  switch (budget_class) {
    case ContextBudgetClass::conversation:
      return budgets.conversation_tokens;
    case ContextBudgetClass::summary:
      return budgets.summary_tokens;
    case ContextBudgetClass::tool_result:
      return budgets.tool_result_tokens;
    case ContextBudgetClass::repository_evidence:
      return budgets.repository_evidence_tokens;
    case ContextBudgetClass::attachment:
      return budgets.attachment_tokens;
    case ContextBudgetClass::unknown:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] auto usage_value(ContextClassUsage& usage, const ContextBudgetClass budget_class)
    -> std::uint64_t& {
  switch (budget_class) {
    case ContextBudgetClass::conversation:
      return usage.conversation_tokens;
    case ContextBudgetClass::summary:
      return usage.summary_tokens;
    case ContextBudgetClass::tool_result:
      return usage.tool_result_tokens;
    case ContextBudgetClass::repository_evidence:
      return usage.repository_evidence_tokens;
    case ContextBudgetClass::attachment:
      return usage.attachment_tokens;
    case ContextBudgetClass::unknown:
      break;
  }
  return usage.repository_evidence_tokens;
}

[[nodiscard]] auto contains_unknown(const std::vector<ContentBlock>& content) -> bool {
  return std::ranges::any_of(content, [](const ContentBlock& block) {
    return std::holds_alternative<UnknownContentBlock>(block);
  });
}

[[nodiscard]] auto contains_artifact(const std::vector<ContentBlock>& content) -> bool {
  return std::ranges::any_of(content, [](const ContentBlock& block) {
    return std::holds_alternative<ArtifactReferenceBlock>(block);
  });
}

[[nodiscard]] auto reference_location(const RepositoryEvidenceReference& reference) -> std::string {
  return std::visit(
      [](const auto& value) -> std::string {
        using Reference = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Reference, ExactSourceEvidence>) {
          return value.source.relative_path;
        } else if constexpr (std::same_as<Reference, DiagnosticEvidence>) {
          return value.source ? value.source->relative_path
                              : "artifact:" + std::string{value.artifact_id.value()};
        } else if constexpr (std::same_as<Reference, DiffEvidence>) {
          return "artifact:" + std::string{value.artifact_id.value()};
        } else if constexpr (std::same_as<Reference, ToolResultEvidence>) {
          return "invocation:" + std::string{value.invocation_id.value()};
        } else if constexpr (std::same_as<Reference, DerivedRecordEvidence>) {
          return value.record_type + ':' + value.record_id;
        } else {
          return "unknown:" + value.type_name;
        }
      },
      reference);
}

[[nodiscard]] auto reference_digest(const ContextParcelItem& item,
                                    const RepositorySnapshotIdentity& target_snapshot)
    -> std::string {
  if (const auto* exact = std::get_if<ExactSourceEvidence>(&item.reference)) {
    return exact->source.content_digest.algorithm + ':' + exact->source.content_digest.value;
  }
  const auto& snapshot = item.provenance.source_snapshot.value_or(target_snapshot);
  return snapshot.fingerprint.algorithm + ':' + snapshot.fingerprint.value;
}

[[nodiscard]] auto evidence_kind_rank(const std::optional<RepositoryEvidenceReference>& reference,
                                      const TaskPhase phase) -> std::uint32_t {
  if (!reference) return 3;
  const auto kind = reference->index();
  // Variant order: exact, diagnostic, diff, tool result, derived, unknown.
  switch (phase) {
    case TaskPhase::orientation: {
      constexpr std::uint32_t ranks[]{1, 4, 3, 4, 0, 5};
      return ranks[kind];
    }
    case TaskPhase::diagnosis: {
      constexpr std::uint32_t ranks[]{2, 0, 3, 1, 4, 5};
      return ranks[kind];
    }
    case TaskPhase::editing: {
      constexpr std::uint32_t ranks[]{0, 2, 1, 3, 4, 5};
      return ranks[kind];
    }
    case TaskPhase::verification: {
      constexpr std::uint32_t ranks[]{3, 1, 0, 2, 4, 5};
      return ranks[kind];
    }
    case TaskPhase::review: {
      constexpr std::uint32_t ranks[]{2, 1, 0, 3, 4, 5};
      return ranks[kind];
    }
    case TaskPhase::unknown:
      return 5;
  }
  return 5;
}

[[nodiscard]] auto class_rank(const ContextBudgetClass budget_class, const TaskPhase phase)
    -> std::uint32_t {
  const auto index = static_cast<std::size_t>(budget_class);
  switch (phase) {
    case TaskPhase::orientation: {
      constexpr std::uint32_t ranks[]{3, 0, 4, 1, 2, 5};
      return ranks[index];
    }
    case TaskPhase::diagnosis: {
      constexpr std::uint32_t ranks[]{2, 3, 0, 1, 4, 5};
      return ranks[index];
    }
    case TaskPhase::editing: {
      constexpr std::uint32_t ranks[]{1, 4, 2, 0, 3, 5};
      return ranks[index];
    }
    case TaskPhase::verification: {
      constexpr std::uint32_t ranks[]{3, 4, 0, 1, 2, 5};
      return ranks[index];
    }
    case TaskPhase::review: {
      constexpr std::uint32_t ranks[]{2, 3, 1, 0, 2, 5};
      return ranks[index];
    }
    case TaskPhase::unknown:
      return 5;
  }
  return 5;
}

[[nodiscard]] auto representation_rank(const ContextRepresentation representation,
                                       const TaskPhase phase) -> std::uint32_t {
  const auto index = static_cast<std::size_t>(representation);
  switch (phase) {
    case TaskPhase::orientation: {
      constexpr std::uint32_t ranks[]{2, 5, 1, 2, 0, 3, 6};
      return ranks[index];
    }
    case TaskPhase::diagnosis: {
      constexpr std::uint32_t ranks[]{2, 1, 3, 0, 4, 5, 6};
      return ranks[index];
    }
    case TaskPhase::editing: {
      constexpr std::uint32_t ranks[]{2, 0, 4, 1, 5, 6, 7};
      return ranks[index];
    }
    case TaskPhase::verification: {
      constexpr std::uint32_t ranks[]{2, 1, 4, 0, 3, 5, 6};
      return ranks[index];
    }
    case TaskPhase::review: {
      constexpr std::uint32_t ranks[]{2, 1, 4, 0, 3, 5, 6};
      return ranks[index];
    }
    case TaskPhase::unknown:
      return 7;
  }
  return 7;
}

[[nodiscard]] auto eligible_freshness(const EvidenceFreshness freshness, const TaskPhase phase)
    -> bool {
  if (freshness == EvidenceFreshness::current) return true;
  if (freshness != EvidenceFreshness::possibly_stale) return false;
  return phase == TaskPhase::orientation || phase == TaskPhase::diagnosis ||
         phase == TaskPhase::review;
}

[[nodiscard]] auto same_exact_location(const CandidateState& left, const CandidateState& right)
    -> bool {
  if (!left.reference || !right.reference) return false;
  const auto* left_source = std::get_if<ExactSourceEvidence>(&*left.reference);
  const auto* right_source = std::get_if<ExactSourceEvidence>(&*right.reference);
  return left_source && right_source &&
         left_source->source.snapshot == right_source->source.snapshot &&
         left_source->source.relative_path == right_source->source.relative_path &&
         left_source->source.range == right_source->source.range;
}

[[nodiscard]] auto valid_reference_representation(const CandidateState& candidate) -> bool {
  if (!candidate.reference) return true;
  return std::visit(
      [&](const auto& reference) {
        using Reference = std::remove_cvref_t<decltype(reference)>;
        if constexpr (std::same_as<Reference, ExactSourceEvidence>) {
          return candidate.representation == ContextRepresentation::exact ||
                 candidate.representation == ContextRepresentation::excerpt;
        } else if constexpr (std::same_as<Reference, DerivedRecordEvidence>) {
          return candidate.representation == ContextRepresentation::derived ||
                 candidate.representation == ContextRepresentation::summary ||
                 candidate.representation == ContextRepresentation::excerpt ||
                 candidate.representation == ContextRepresentation::artifact_reference;
        } else if constexpr (std::same_as<Reference, UnknownRepositoryEvidence>) {
          return true;
        } else {
          return candidate.representation == ContextRepresentation::direct ||
                 candidate.representation == ContextRepresentation::summary ||
                 candidate.representation == ContextRepresentation::excerpt ||
                 candidate.representation == ContextRepresentation::artifact_reference;
        }
      },
      *candidate.reference);
}

[[nodiscard]] auto unsupported_candidate(const CandidateState& candidate) -> bool {
  return candidate.budget_class == ContextBudgetClass::unknown ||
         candidate.representation == ContextRepresentation::unknown ||
         contains_unknown(candidate.content.message.content) ||
         (candidate.reference &&
          std::holds_alternative<UnknownRepositoryEvidence>(*candidate.reference)) ||
         (candidate.representation == ContextRepresentation::artifact_reference &&
          !contains_artifact(candidate.content.message.content));
}

[[nodiscard]] auto decision_for_freshness(const EvidenceFreshness freshness)
    -> ContextSelectionDecision {
  return freshness == EvidenceFreshness::unavailable ? ContextSelectionDecision::omitted_unavailable
                                                     : ContextSelectionDecision::omitted_stale;
}

[[nodiscard]] auto make_record(const CandidateState& candidate,
                               const ContextSelectionDecision decision)
    -> ContextSelectionDecisionRecord {
  return {candidate.content.entry_id, candidate.evidence_id, decision,
          candidate.content.estimated_tokens};
}

[[nodiscard]] auto context_error_code(const ContextBuildErrorCode code)
    -> ContextSelectionErrorCode {
  return code == ContextBuildErrorCode::capacity_exceeded ||
                 code == ContextBuildErrorCode::token_overflow
             ? ContextSelectionErrorCode::mandatory_capacity_exceeded
             : ContextSelectionErrorCode::context_build_failed;
}

}  // namespace

auto ContextBuilder::select_and_build(ContextSelectionRequest request) const
    -> std::expected<ContextSelectionResult, ContextSelectionError> {
  try {
    if (!valid_phase(request.phase)) {
      return failure(ContextSelectionErrorCode::invalid_phase,
                     "context selection requires a known task phase");
    }
    if (request.selection_limits.maximum_candidates == 0 ||
        request.selection_limits.maximum_parcels == 0 ||
        request.selection_limits.maximum_alternative_group_bytes == 0) {
      return failure(ContextSelectionErrorCode::invalid_candidate,
                     "context selection limits must be positive");
    }
    if (request.candidates.size() > request.selection_limits.maximum_candidates ||
        request.parcels.size() > request.selection_limits.maximum_parcels) {
      return failure(ContextSelectionErrorCode::resource_exhausted,
                     "context selection exceeds its candidate or parcel limit");
    }

    auto mandatory = build(ContextBuildInput{request.capacity, request.instructions, {}});
    if (!mandatory) {
      return failure(context_error_code(mandatory.error().code),
                     "mandatory context is invalid or exceeds capacity",
                     mandatory.error().entry_id);
    }

    std::vector<CandidateState> candidates;
    candidates.reserve(request.candidates.size());
    for (auto& candidate : request.candidates) {
      candidates.push_back(CandidateState{
          std::move(candidate.content), candidate.budget_class, candidate.representation,
          candidate.freshness, candidate.required, candidate.relevance_rank,
          std::move(candidate.alternative_group), std::nullopt, std::nullopt});
    }

    std::set<ContextParcelId> parcel_ids;
    std::optional<RepositorySnapshotIdentity> target_snapshot;
    for (auto& selection : request.parcels) {
      if (!parcel_ids.insert(selection.parcel.parcel_id).second) {
        return failure(ContextSelectionErrorCode::duplicate_candidate,
                       "context parcel IDs must be unique");
      }
      const auto validated =
          repository::validate_context_parcel(selection.parcel, request.parcel_limits);
      if (!validated) {
        return failure(ContextSelectionErrorCode::invalid_parcel,
                       "repository context parcel is invalid", std::nullopt,
                       validated.error().evidence_id);
      }
      if (selection.parcel.phase != request.phase) {
        return failure(ContextSelectionErrorCode::invalid_parcel,
                       "repository parcel phase does not match selection phase");
      }
      if (target_snapshot &&
          !same_source_state(*target_snapshot, selection.parcel.target_snapshot)) {
        return failure(ContextSelectionErrorCode::conflicting_candidate,
                       "repository parcels target different source states");
      }
      target_snapshot = selection.parcel.target_snapshot;

      std::map<EvidenceId, RepositoryEvidenceSelection*> bindings;
      for (auto& binding : selection.items) {
        if (!bindings.emplace(binding.evidence_id, &binding).second) {
          return failure(ContextSelectionErrorCode::duplicate_candidate,
                         "repository evidence bindings must be unique", binding.entry_id,
                         binding.evidence_id);
        }
      }
      if (bindings.size() != selection.parcel.items.size()) {
        return failure(ContextSelectionErrorCode::invalid_parcel,
                       "every parcel item requires exactly one selection binding");
      }

      for (auto& item : selection.parcel.items) {
        const auto found = bindings.find(item.evidence_id);
        if (found == bindings.end()) {
          return failure(ContextSelectionErrorCode::invalid_parcel,
                         "parcel item is missing its selection binding", std::nullopt,
                         item.evidence_id);
        }
        auto& binding = *found->second;
        if (candidates.size() == request.selection_limits.maximum_candidates) {
          return failure(ContextSelectionErrorCode::resource_exhausted,
                         "context selection contains too many candidates");
        }
        if (binding.order == 0) {
          return failure(ContextSelectionErrorCode::invalid_candidate,
                         "repository evidence order must be positive", binding.entry_id,
                         binding.evidence_id);
        }
        ContextProvenance provenance{binding.source_id, reference_location(item.reference),
                                     reference_digest(item, selection.parcel.target_snapshot)};
        candidates.push_back(CandidateState{
            {binding.entry_id,
             ContextContentKind::evidence,
             {binding.message_id, Role::evidence, std::move(item.content), std::nullopt},
             std::move(provenance),
             binding.order,
             item.estimated_tokens},
            binding.budget_class,
            binding.representation,
            item.freshness,
            binding.required,
            binding.relevance_rank,
            std::move(binding.alternative_group),
            item.evidence_id,
            item.reference});
      }
    }

    std::set<ContextEntryId> entry_ids;
    std::set<MessageId> message_ids;
    std::set<EvidenceId> evidence_ids;
    std::map<std::string, std::size_t> required_groups;
    bool has_required_current_exact{};
    for (std::size_t index{}; index < candidates.size(); ++index) {
      const auto& candidate = candidates[index];
      if (!known_budget_class(candidate.budget_class) ||
          !known_representation(candidate.representation) ||
          !known_freshness(candidate.freshness) || !valid_reference_representation(candidate)) {
        return failure(ContextSelectionErrorCode::invalid_candidate,
                       "candidate classification is invalid", candidate.content.entry_id,
                       candidate.evidence_id);
      }
      if (!entry_ids.insert(candidate.content.entry_id).second ||
          !message_ids.insert(candidate.content.message.message_id).second ||
          (candidate.evidence_id && !evidence_ids.insert(*candidate.evidence_id).second)) {
        return failure(ContextSelectionErrorCode::duplicate_candidate,
                       "context selection identities must be unique", candidate.content.entry_id,
                       candidate.evidence_id);
      }
      if (candidate.content.order == 0 ||
          (candidate.content.estimated_tokens == 0 &&
           candidate.freshness != EvidenceFreshness::unavailable) ||
          !valid_group(candidate.alternative_group,
                       request.selection_limits.maximum_alternative_group_bytes)) {
        return failure(ContextSelectionErrorCode::invalid_candidate,
                       "candidate order, estimate, or alternative group is invalid",
                       candidate.content.entry_id, candidate.evidence_id);
      }
      if (candidate.required && candidate.alternative_group &&
          ++required_groups[*candidate.alternative_group] > 1) {
        return failure(ContextSelectionErrorCode::conflicting_candidate,
                       "an alternative group cannot contain multiple required candidates",
                       candidate.content.entry_id, candidate.evidence_id);
      }
      if (candidate.required && candidate.freshness == EvidenceFreshness::current &&
          candidate.reference &&
          std::holds_alternative<ExactSourceEvidence>(*candidate.reference) &&
          candidate.representation == ContextRepresentation::exact) {
        has_required_current_exact = true;
      }
      for (std::size_t previous{}; previous < index; ++previous) {
        if (!same_exact_location(candidate, candidates[previous])) continue;
        const bool same =
            candidate.reference == candidates[previous].reference &&
            candidate.content.message.content == candidates[previous].content.message.content &&
            candidate.freshness == candidates[previous].freshness;
        return failure(same ? ContextSelectionErrorCode::duplicate_candidate
                            : ContextSelectionErrorCode::conflicting_candidate,
                       same ? "exact repository evidence is duplicated"
                            : "exact repository evidence conflicts",
                       candidate.content.entry_id, candidate.evidence_id);
      }
    }
    if (request.phase == TaskPhase::editing && !has_required_current_exact) {
      return failure(ContextSelectionErrorCode::required_candidate_unavailable,
                     "editing context requires required current exact source evidence");
    }

    std::vector<ContextSelectionDecisionRecord> decisions;
    decisions.reserve(candidates.size());
    std::vector<std::size_t> selectable;
    selectable.reserve(candidates.size());
    for (std::size_t index{}; index < candidates.size(); ++index) {
      const auto& candidate = candidates[index];
      std::optional<ContextSelectionDecision> omitted;
      if (unsupported_candidate(candidate)) {
        omitted = ContextSelectionDecision::omitted_unsupported;
      } else if (!eligible_freshness(candidate.freshness, request.phase)) {
        omitted = decision_for_freshness(candidate.freshness);
      }
      if (omitted) {
        if (candidate.required) {
          return failure(ContextSelectionErrorCode::required_candidate_unavailable,
                         "required context candidate is unavailable or unsupported",
                         candidate.content.entry_id, candidate.evidence_id);
        }
        decisions.push_back(make_record(candidate, *omitted));
      } else {
        selectable.push_back(index);
      }
    }

    std::ranges::sort(selectable, [&](const std::size_t left_index, const std::size_t right_index) {
      const auto& left = candidates[left_index];
      const auto& right = candidates[right_index];
      if (left.required != right.required) return left.required > right.required;
      const auto left_class = class_rank(left.budget_class, request.phase);
      const auto right_class = class_rank(right.budget_class, request.phase);
      if (left_class != right_class) return left_class < right_class;
      const auto left_kind = evidence_kind_rank(left.reference, request.phase);
      const auto right_kind = evidence_kind_rank(right.reference, request.phase);
      if (left_kind != right_kind) return left_kind < right_kind;
      if (left.relevance_rank != right.relevance_rank) {
        return left.relevance_rank < right.relevance_rank;
      }
      const auto left_representation = representation_rank(left.representation, request.phase);
      const auto right_representation = representation_rank(right.representation, request.phase);
      if (left_representation != right_representation) {
        return left_representation < right_representation;
      }
      if (left.content.estimated_tokens != right.content.estimated_tokens) {
        return left.content.estimated_tokens < right.content.estimated_tokens;
      }
      if (left.budget_class == ContextBudgetClass::conversation &&
          right.budget_class == ContextBudgetClass::conversation &&
          left.content.order != right.content.order) {
        return left.content.order > right.content.order;
      }
      if (left.content.order != right.content.order) {
        return left.content.order < right.content.order;
      }
      return left.content.entry_id < right.content.entry_id;
    });

    ContextClassUsage usage;
    std::uint64_t total = mandatory->estimated_input_tokens;
    const auto maximum_input =
        request.capacity.context_window_tokens - request.capacity.reserved_output_tokens;
    std::set<std::string> admitted_groups;
    std::vector<ContextContentInput> selected;
    for (const auto index : selectable) {
      auto& candidate = candidates[index];
      if (candidate.alternative_group && admitted_groups.contains(*candidate.alternative_group)) {
        decisions.push_back(make_record(candidate, ContextSelectionDecision::omitted_alternative));
        continue;
      }

      auto class_total = usage_value(usage, candidate.budget_class);
      std::uint64_t proposed_class = class_total;
      std::uint64_t proposed_total = total;
      const bool arithmetic_ok = add_checked(proposed_class, candidate.content.estimated_tokens) &&
                                 add_checked(proposed_total, candidate.content.estimated_tokens);
      if (!arithmetic_ok) {
        return failure(ContextSelectionErrorCode::token_overflow,
                       "context candidate token estimate overflowed", candidate.content.entry_id,
                       candidate.evidence_id);
      }

      const auto limit = budget_limit(request.budgets, candidate.budget_class);
      const bool class_fits = !limit || proposed_class <= *limit;
      const bool total_fits = proposed_total <= maximum_input;
      if (!class_fits || !total_fits) {
        if (candidate.required) {
          return failure(ContextSelectionErrorCode::mandatory_capacity_exceeded,
                         !class_fits ? "required context exceeds its class token budget"
                                     : "required context exceeds model input capacity",
                         candidate.content.entry_id, candidate.evidence_id);
        }
        decisions.push_back(
            make_record(candidate, class_fits ? ContextSelectionDecision::omitted_budget
                                              : ContextSelectionDecision::omitted_class_budget));
        continue;
      }

      class_total = proposed_class;
      usage_value(usage, candidate.budget_class) = class_total;
      total = proposed_total;
      if (candidate.alternative_group) {
        admitted_groups.insert(*candidate.alternative_group);
      }
      decisions.push_back(make_record(candidate, ContextSelectionDecision::admitted));
      selected.push_back(std::move(candidate.content));
    }

    auto built = build(
        ContextBuildInput{request.capacity, std::move(request.instructions), std::move(selected)});
    if (!built) {
      return failure(context_error_code(built.error().code),
                     "selected context failed final validation", built.error().entry_id);
    }
    std::ranges::sort(decisions, [](const auto& left, const auto& right) {
      return left.entry_id < right.entry_id;
    });
    return ContextSelectionResult{std::move(*built), std::move(decisions), usage};
  } catch (...) {
    return failure(ContextSelectionErrorCode::internal_failure,
                   "context selection failed internally");
  }
}

}  // namespace aiforge::runtime
