#include <aiforge/repository/context_parcel.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace aiforge::repository {
namespace {

using namespace domain;

[[nodiscard]] auto failure(
    const ContextParcelErrorCode code, std::string message,
    std::optional<EvidenceId> evidence_id = std::nullopt)
    -> std::unexpected<ContextParcelError> {
  return std::unexpected(
      ContextParcelError{code, std::move(message), std::move(evidence_id)});
}

[[nodiscard]] auto valid_utf8_text(const std::string_view value,
                                   const bool allow_empty = false) -> bool {
  if (!allow_empty && value.empty()) return false;
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || first == 0x7FU ||
        (first < 0x20U && first != '\t' && first != '\n' && first != '\r')) {
      return false;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum,
                                const bool allow_empty = false) -> bool {
  return value.size() <= maximum && valid_utf8_text(value, allow_empty);
}

[[nodiscard]] auto valid_digest(const ContentDigest& digest,
                                const std::uint64_t maximum_bytes) -> bool {
  if (!bounded_text(digest.algorithm, 128) ||
      !bounded_text(digest.value, 512) || digest.byte_size > maximum_bytes) {
    return false;
  }
  return std::ranges::all_of(digest.algorithm, [](const unsigned char value) {
           return std::isalnum(value) != 0 || value == '-' || value == '_' ||
                  value == '.';
         }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] auto valid_snapshot(const RepositorySnapshotIdentity& snapshot,
                                  const std::uint64_t maximum_bytes) -> bool {
  return valid_digest(snapshot.fingerprint, maximum_bytes);
}

[[nodiscard]] auto valid_relative_path(const std::string& value,
                                       const std::size_t maximum) -> bool {
  if (!bounded_text(value, maximum)) return false;
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto& part : path) {
    if (part == "." || part == ".." || part.empty()) return false;
  }
  return path.generic_string() == value;
}

[[nodiscard]] auto add_checked(std::uint64_t& total,
                               const std::uint64_t value) -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

[[nodiscard]] auto validate_source(
    const RepositorySourceIdentity& source,
    const RepositorySnapshotIdentity& target,
    const ContextParcelLimits& limits,
    const EvidenceId& evidence_id)
    -> std::expected<void, ContextParcelError> {
  if (!valid_snapshot(source.snapshot, limits.maximum_total_bytes) ||
      source.snapshot.repository_id != target.repository_id ||
      !valid_relative_path(source.relative_path, limits.maximum_path_bytes) ||
      !valid_digest(source.content_digest, limits.maximum_item_bytes)) {
    return failure(ContextParcelErrorCode::invalid_source,
                   "repository evidence source identity is invalid",
                   evidence_id);
  }
  if (source.range &&
      (source.range->begin >= source.range->end ||
       source.range->end > source.content_digest.byte_size)) {
    return failure(ContextParcelErrorCode::invalid_range,
                   "repository evidence source range is invalid", evidence_id);
  }
  return {};
}

struct ReferenceFacts {
  std::optional<RepositorySnapshotIdentity> source_snapshot;
  std::optional<ArtifactId> artifact_id;
};

[[nodiscard]] auto validate_reference(
    const ContextParcelItem& item, const RepositorySnapshotIdentity& target,
    const ContextParcelLimits& limits)
    -> std::expected<ReferenceFacts, ContextParcelError> {
  return std::visit(
      [&](const auto& reference)
          -> std::expected<ReferenceFacts, ContextParcelError> {
        using Reference = std::decay_t<decltype(reference)>;
        if constexpr (std::is_same_v<Reference, ExactSourceEvidence>) {
          auto valid =
              validate_source(reference.source, target, limits,
                              item.evidence_id);
          if (!valid) return std::unexpected(valid.error());
          if (item.provenance.derivation != EvidenceDerivation::observed) {
            return failure(ContextParcelErrorCode::invalid_reference,
                           "exact source evidence must be observed",
                           item.evidence_id);
          }
          if (item.freshness != EvidenceFreshness::unavailable &&
              !std::ranges::all_of(item.content, [](const ContentBlock& block) {
                return std::holds_alternative<TextBlock>(block);
              })) {
            return failure(ContextParcelErrorCode::invalid_reference,
                           "exact source evidence must contain text blocks",
                           item.evidence_id);
          }
          return ReferenceFacts{reference.source.snapshot, std::nullopt};
        } else if constexpr (std::is_same_v<Reference,
                                            DiagnosticEvidence>) {
          if (reference.source) {
            auto valid = validate_source(*reference.source, target, limits,
                                         item.evidence_id);
            if (!valid) return std::unexpected(valid.error());
          }
          if (item.provenance.derivation != EvidenceDerivation::observed) {
            return failure(ContextParcelErrorCode::invalid_reference,
                           "diagnostic evidence must be observed",
                           item.evidence_id);
          }
          return ReferenceFacts{
              reference.source
                  ? std::optional<RepositorySnapshotIdentity>{
                        reference.source->snapshot}
                  : item.provenance.source_snapshot,
              reference.artifact_id};
        } else if constexpr (std::is_same_v<Reference, DiffEvidence>) {
          if (!valid_snapshot(reference.base_snapshot,
                              limits.maximum_total_bytes) ||
              !valid_snapshot(reference.target_snapshot,
                              limits.maximum_total_bytes) ||
              reference.base_snapshot.repository_id != target.repository_id ||
              reference.target_snapshot.repository_id !=
                  target.repository_id ||
              same_source_state(reference.base_snapshot,
                                reference.target_snapshot) ||
              item.provenance.derivation != EvidenceDerivation::observed) {
            return failure(ContextParcelErrorCode::invalid_reference,
                           "repository diff evidence is invalid",
                           item.evidence_id);
          }
          return ReferenceFacts{reference.target_snapshot,
                                reference.artifact_id};
        } else if constexpr (std::is_same_v<Reference,
                                            ToolResultEvidence>) {
          if (!item.provenance.producing_invocation_id ||
              *item.provenance.producing_invocation_id !=
                  reference.invocation_id ||
              item.provenance.derivation != EvidenceDerivation::observed) {
            return failure(ContextParcelErrorCode::invalid_reference,
                           "tool-result evidence invocation is inconsistent",
                           item.evidence_id);
          }
          return ReferenceFacts{item.provenance.source_snapshot,
                                reference.artifact_id};
        } else if constexpr (std::is_same_v<Reference,
                                            DerivedRecordEvidence>) {
          if (!bounded_text(reference.record_type,
                            limits.maximum_metadata_bytes) ||
              !bounded_text(reference.record_id,
                            limits.maximum_metadata_bytes) ||
              (item.provenance.derivation != EvidenceDerivation::derived &&
               item.provenance.derivation != EvidenceDerivation::inferred)) {
            return failure(ContextParcelErrorCode::invalid_reference,
                           "derived repository evidence is invalid",
                           item.evidence_id);
          }
          return ReferenceFacts{item.provenance.source_snapshot,
                                reference.artifact_id};
        } else {
          if (!bounded_text(reference.type_name,
                            limits.maximum_metadata_bytes)) {
            return failure(ContextParcelErrorCode::invalid_reference,
                           "unknown repository evidence type is invalid",
                           item.evidence_id);
          }
          return ReferenceFacts{item.provenance.source_snapshot,
                                reference.artifact_id};
        }
      },
      item.reference);
}

[[nodiscard]] auto content_bytes(
    const ContextParcelItem& item, const ContextParcelLimits& limits,
    const std::optional<ArtifactId>& expected_artifact)
    -> std::expected<std::uint64_t, ContextParcelError> {
  std::uint64_t total{};
  std::size_t artifact_count{};
  bool artifact_matches{true};
  for (const auto& block : item.content) {
    std::uint64_t bytes{};
    const bool valid = std::visit(
        [&](const auto& value) {
          using Block = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Block, TextBlock>) {
            bytes = value.text.size();
            return bounded_text(value.text, limits.maximum_item_bytes);
          } else if constexpr (std::is_same_v<Block, StructuredDataBlock>) {
            bytes = value.media_type.size() + value.data.size();
            return bounded_text(value.media_type,
                                limits.maximum_metadata_bytes) &&
                   bounded_text(value.data, limits.maximum_item_bytes, true);
          } else if constexpr (std::is_same_v<Block, CitationBlock>) {
            bytes = value.uri.size() +
                    (value.title ? value.title->size() : std::size_t{});
            return bounded_text(value.uri, limits.maximum_metadata_bytes) &&
                   (!value.title ||
                    bounded_text(*value.title,
                                 limits.maximum_metadata_bytes));
          } else if constexpr (std::is_same_v<Block,
                                              ArtifactReferenceBlock>) {
            ++artifact_count;
            artifact_matches = artifact_matches && expected_artifact &&
                               value.artifact_id == *expected_artifact;
            bytes = value.artifact_id.value().size() +
                    (value.label ? value.label->size() : std::size_t{});
            return !value.label ||
                   bounded_text(*value.label, limits.maximum_metadata_bytes);
          } else {
            bytes = value.type_name.size();
            return bounded_text(value.type_name,
                                limits.maximum_metadata_bytes);
          }
        },
        block);
    if (!valid) {
      return failure(ContextParcelErrorCode::invalid_item,
                     "repository evidence content is invalid",
                     item.evidence_id);
    }
    if (!add_checked(total, bytes)) {
      return failure(ContextParcelErrorCode::overflow,
                     "repository evidence inline size overflowed",
                     item.evidence_id);
    }
  }
  if ((expected_artifact &&
       (artifact_count != 1 || !artifact_matches)) ||
      (!expected_artifact && artifact_count != 0)) {
    return failure(ContextParcelErrorCode::invalid_reference,
                   "artifact content does not match its evidence reference",
                   item.evidence_id);
  }
  return total;
}

[[nodiscard]] auto validate_provenance(
    const ContextParcelItem& item, const ContextParcelLimits& limits)
    -> std::expected<void, ContextParcelError> {
  const auto& provenance = item.provenance;
  if (!bounded_text(provenance.producer, limits.maximum_metadata_bytes) ||
      !bounded_text(provenance.producer_version,
                    limits.maximum_metadata_bytes) ||
      provenance.source_event_ids.size() >
          limits.maximum_provenance_references ||
      provenance.derivation_inputs.size() >
          limits.maximum_provenance_references) {
    return failure(ContextParcelErrorCode::invalid_provenance,
                   "repository evidence provenance is invalid",
                   item.evidence_id);
  }

  const std::set<EventId> source_events{provenance.source_event_ids.begin(),
                                        provenance.source_event_ids.end()};
  const std::set<EvidenceId> derivation_inputs{
      provenance.derivation_inputs.begin(), provenance.derivation_inputs.end()};
  if (source_events.size() != provenance.source_event_ids.size() ||
      derivation_inputs.size() != provenance.derivation_inputs.size() ||
      derivation_inputs.contains(item.evidence_id)) {
    return failure(ContextParcelErrorCode::invalid_provenance,
                   "repository evidence provenance references are invalid",
                   item.evidence_id);
  }

  const bool is_derived =
      provenance.derivation == EvidenceDerivation::derived ||
      provenance.derivation == EvidenceDerivation::inferred;
  if (is_derived != !provenance.derivation_inputs.empty() ||
      (provenance.derivation == EvidenceDerivation::observed &&
       !provenance.derivation_inputs.empty())) {
    return failure(ContextParcelErrorCode::invalid_provenance,
                   "repository evidence derivation inputs are inconsistent",
                   item.evidence_id);
  }
  return {};
}

[[nodiscard]] auto validate_context_parcel_impl(
    const ContextParcel& parcel, const ContextParcelLimits& limits)
    -> std::expected<ContextParcelEstimate, ContextParcelError> {
  if (limits.maximum_items == 0 || limits.maximum_purpose_bytes == 0 ||
      limits.maximum_path_bytes == 0 || limits.maximum_metadata_bytes == 0 ||
      limits.maximum_provenance_references == 0 ||
      limits.maximum_content_blocks_per_item == 0 ||
      limits.maximum_item_bytes == 0 || limits.maximum_total_bytes == 0 ||
      limits.maximum_total_tokens == 0 ||
      limits.maximum_item_bytes > limits.maximum_total_bytes) {
    return failure(ContextParcelErrorCode::invalid_limits,
                   "context parcel limits are invalid");
  }
  if (!bounded_text(parcel.purpose, limits.maximum_purpose_bytes) ||
      parcel.phase == TaskPhase::unknown ||
      !valid_snapshot(parcel.target_snapshot, limits.maximum_total_bytes)) {
    return failure(parcel.phase == TaskPhase::unknown ||
                           !bounded_text(parcel.purpose,
                                         limits.maximum_purpose_bytes)
                       ? ContextParcelErrorCode::invalid_parcel
                       : ContextParcelErrorCode::invalid_source,
                   "context parcel identity or purpose is invalid");
  }
  if (parcel.items.size() > limits.maximum_items) {
    return failure(ContextParcelErrorCode::resource_exhausted,
                   "context parcel contains too many evidence items");
  }
  if (parcel.items.empty()) {
    return failure(ContextParcelErrorCode::invalid_parcel,
                   "context parcel requires at least one evidence item");
  }

  std::set<EvidenceId> evidence_ids;
  ContextParcelEstimate estimate{parcel.items.size(), 0, 0, 0};
  for (const auto& item : parcel.items) {
    if (!evidence_ids.insert(item.evidence_id).second) {
      return failure(ContextParcelErrorCode::duplicate_item,
                     "context parcel evidence IDs must be unique",
                     item.evidence_id);
    }
    auto provenance = validate_provenance(item, limits);
    if (!provenance) return std::unexpected(provenance.error());

    auto facts = validate_reference(item, parcel.target_snapshot, limits);
    if (!facts) return std::unexpected(facts.error());
    if (!std::holds_alternative<UnknownRepositoryEvidence>(item.reference) &&
        !item.provenance.source_snapshot) {
      return failure(ContextParcelErrorCode::invalid_provenance,
                     "known repository evidence requires snapshot provenance",
                     item.evidence_id);
    }
    if (facts->source_snapshot &&
        (!item.provenance.source_snapshot ||
         !same_source_state(*facts->source_snapshot,
                            *item.provenance.source_snapshot))) {
      return failure(ContextParcelErrorCode::conflicting_provenance,
                     "evidence reference and provenance snapshots conflict",
                     item.evidence_id);
    }
    if (item.provenance.source_snapshot) {
      if (!valid_snapshot(*item.provenance.source_snapshot,
                          limits.maximum_total_bytes) ||
          item.provenance.source_snapshot->repository_id !=
              parcel.target_snapshot.repository_id) {
        return failure(ContextParcelErrorCode::invalid_source,
                       "evidence provenance belongs to another repository",
                       item.evidence_id);
      }
    }

    const bool matches_target = item.provenance.source_snapshot &&
                                same_source_state(
                                    *item.provenance.source_snapshot,
                                    parcel.target_snapshot);
    if ((item.freshness == EvidenceFreshness::current && !matches_target) ||
        (item.freshness == EvidenceFreshness::stale &&
         !item.provenance.source_snapshot)) {
      return failure(ContextParcelErrorCode::invalid_freshness,
                     "evidence freshness contradicts its source snapshot",
                     item.evidence_id);
    }

    if (item.freshness == EvidenceFreshness::unavailable) {
      if (!item.content.empty() || item.estimated_bytes != 0 ||
          item.estimated_tokens != 0) {
        return failure(ContextParcelErrorCode::invalid_freshness,
                       "unavailable evidence cannot carry model content",
                       item.evidence_id);
      }
      continue;
    }
    if (item.content.size() > limits.maximum_content_blocks_per_item) {
      return failure(ContextParcelErrorCode::resource_exhausted,
                     "evidence contains too many content blocks",
                     item.evidence_id);
    }
    if (item.content.empty() || item.estimated_bytes == 0 ||
        item.estimated_tokens == 0) {
      return failure(ContextParcelErrorCode::invalid_item,
                     "available evidence requires content and estimates",
                     item.evidence_id);
    }

    auto inline_size = content_bytes(item, limits, facts->artifact_id);
    if (!inline_size) return std::unexpected(inline_size.error());
    if (const auto* exact =
            std::get_if<ExactSourceEvidence>(&item.reference)) {
      const auto expected_bytes = exact->source.range
                                      ? exact->source.range->end -
                                            exact->source.range->begin
                                      : exact->source.content_digest.byte_size;
      if (*inline_size != expected_bytes) {
        return failure(ContextParcelErrorCode::invalid_reference,
                       "exact source content does not match its source range",
                       item.evidence_id);
      }
    }
    if (*inline_size > item.estimated_bytes) {
      return failure(ContextParcelErrorCode::invalid_item,
                     "evidence byte estimate is smaller than inline content",
                     item.evidence_id);
    }
    if (item.estimated_bytes > limits.maximum_item_bytes) {
      return failure(ContextParcelErrorCode::resource_exhausted,
                     "evidence byte estimate exceeds the item limit",
                     item.evidence_id);
    }
    if (!add_checked(estimate.inline_bytes, *inline_size) ||
        !add_checked(estimate.represented_bytes, item.estimated_bytes) ||
        !add_checked(estimate.estimated_tokens, item.estimated_tokens)) {
      return failure(ContextParcelErrorCode::overflow,
                     "context parcel estimates overflowed", item.evidence_id);
    }
    if (estimate.represented_bytes > limits.maximum_total_bytes ||
        estimate.estimated_tokens > limits.maximum_total_tokens) {
      return failure(ContextParcelErrorCode::resource_exhausted,
                     "context parcel exceeds its capacity", item.evidence_id);
    }
  }
  return estimate;
}

}  // namespace

auto validate_context_parcel(const ContextParcel& parcel,
                             const ContextParcelLimits& limits)
    -> std::expected<ContextParcelEstimate, ContextParcelError> {
  try {
    return validate_context_parcel_impl(parcel, limits);
  } catch (...) {
    return failure(ContextParcelErrorCode::internal_failure,
                   "context parcel validation failed internally");
  }
}

}  // namespace aiforge::repository
