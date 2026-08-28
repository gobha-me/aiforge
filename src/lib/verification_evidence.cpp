#include <aiforge/repository/verification_evidence.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::repository {
namespace {

using namespace domain;

[[nodiscard]] auto failure(
    const VerificationEvidenceErrorCode code, std::string message,
    std::optional<VerificationEvidenceId> evidence_id = std::nullopt)
    -> std::unexpected<VerificationEvidenceError> {
  return std::unexpected(VerificationEvidenceError{code, std::move(message),
                                                   std::move(evidence_id)});
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

[[nodiscard]] auto valid_digest(const ContentDigest& digest) -> bool {
  return bounded_text(digest.algorithm, 128) &&
         bounded_text(digest.value, 512) &&
         std::ranges::all_of(digest.algorithm,
                             [](const unsigned char value) {
                               return std::isalnum(value) != 0 ||
                                      value == '-' || value == '_' ||
                                      value == '.';
                             }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] auto valid_snapshot(const RepositorySnapshotIdentity& snapshot)
    -> bool {
  return valid_digest(snapshot.fingerprint);
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

[[nodiscard]] auto add_checked(std::uint64_t& total, const std::uint64_t value)
    -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

[[nodiscard]] auto known_kind(const VerificationKind value) -> bool {
  switch (value) {
    case VerificationKind::build:
    case VerificationKind::test:
    case VerificationKind::static_analysis:
    case VerificationKind::diagnostic:
    case VerificationKind::diff:
    case VerificationKind::runtime:
    case VerificationKind::unknown: return true;
  }
  return false;
}

[[nodiscard]] auto known_outcome(const VerificationOutcome value) -> bool {
  switch (value) {
    case VerificationOutcome::passed:
    case VerificationOutcome::failed:
    case VerificationOutcome::partial:
    case VerificationOutcome::cancelled:
    case VerificationOutcome::timed_out:
    case VerificationOutcome::unavailable: return true;
    case VerificationOutcome::unknown: return false;
  }
  return false;
}

[[nodiscard]] auto known_stream(const VerificationOutputStream value) -> bool {
  switch (value) {
    case VerificationOutputStream::standard_output:
    case VerificationOutputStream::standard_error: return true;
  }
  return false;
}

[[nodiscard]] auto known_severity(const VerificationDiagnosticSeverity value)
    -> bool {
  switch (value) {
    case VerificationDiagnosticSeverity::note:
    case VerificationDiagnosticSeverity::warning:
    case VerificationDiagnosticSeverity::error:
    case VerificationDiagnosticSeverity::fatal: return true;
    case VerificationDiagnosticSeverity::unknown: return false;
  }
  return false;
}

[[nodiscard]] auto known_freshness(const EvidenceFreshness value) -> bool {
  switch (value) {
    case EvidenceFreshness::current:
    case EvidenceFreshness::possibly_stale:
    case EvidenceFreshness::stale:
    case EvidenceFreshness::unavailable: return true;
  }
  return false;
}

[[nodiscard]] auto artifact_ids(const VerificationEvidence& evidence)
    -> std::vector<ArtifactId> {
  auto result = evidence.artifacts;
  for (const auto& output : evidence.output) {
    if (output.complete_artifact_id) {
      result.push_back(*output.complete_artifact_id);
    }
  }
  return result;
}

[[nodiscard]] auto valid_limits(const VerificationEvidenceLimits& limits)
    -> bool {
  return limits.maximum_summary_bytes > 0 &&
         limits.maximum_output_excerpts > 0 &&
         limits.maximum_excerpt_bytes > 0 &&
         limits.maximum_total_inline_bytes > 0 &&
         limits.maximum_diagnostics > 0 &&
         limits.maximum_diagnostic_bytes > 0 && limits.maximum_artifacts > 0 &&
         limits.maximum_metadata_bytes > 0 &&
         limits.maximum_excerpt_bytes <= limits.maximum_total_inline_bytes &&
         limits.maximum_diagnostic_bytes <= limits.maximum_total_inline_bytes;
}

[[nodiscard]] auto validate_impl(const VerificationEvidence& evidence,
                                 const VerificationEvidenceLimits& limits)
    -> std::expected<void, VerificationEvidenceError> {
  if (!valid_limits(limits)) {
    return failure(VerificationEvidenceErrorCode::invalid_limits,
                   "verification evidence limits are invalid");
  }
  const auto evidence_id = std::optional{evidence.evidence_id};
  const bool unknown_kind = evidence.kind == VerificationKind::unknown;
  if (!known_kind(evidence.kind) ||
      unknown_kind != evidence.extension_name.has_value() ||
      (evidence.extension_name &&
       !bounded_text(*evidence.extension_name,
                     limits.maximum_metadata_bytes)) ||
      !known_outcome(evidence.outcome) ||
      !valid_snapshot(evidence.source_snapshot) ||
      !bounded_text(evidence.summary, limits.maximum_summary_bytes)) {
    return failure(
        VerificationEvidenceErrorCode::invalid_evidence,
        "verification evidence identity, kind, outcome, or summary is invalid",
        evidence_id);
  }
  if ((evidence.kind == VerificationKind::diff) !=
      evidence.baseline_snapshot.has_value()) {
    return failure(VerificationEvidenceErrorCode::invalid_source,
                   "only diff verification requires a baseline snapshot",
                   evidence_id);
  }
  if (evidence.baseline_snapshot &&
      (!valid_snapshot(*evidence.baseline_snapshot) ||
       evidence.baseline_snapshot->repository_id !=
           evidence.source_snapshot.repository_id ||
       same_source_state(*evidence.baseline_snapshot,
                         evidence.source_snapshot))) {
    return failure(VerificationEvidenceErrorCode::invalid_source,
                   "verification baseline snapshot is invalid", evidence_id);
  }
  if (evidence.build_configuration &&
      !valid_digest(*evidence.build_configuration)) {
    return failure(VerificationEvidenceErrorCode::invalid_source,
                   "verification build configuration is invalid", evidence_id);
  }
  if (!bounded_text(evidence.producer.name, limits.maximum_metadata_bytes) ||
      !bounded_text(evidence.producer.version, limits.maximum_metadata_bytes) ||
      !bounded_text(evidence.producer.tool_name,
                    limits.maximum_metadata_bytes) ||
      evidence.observed_at.time_since_epoch().count() <= 0) {
    return failure(VerificationEvidenceErrorCode::invalid_provenance,
                   "verification producer provenance is invalid", evidence_id);
  }
  if (evidence.output.size() > limits.maximum_output_excerpts ||
      evidence.diagnostics.size() > limits.maximum_diagnostics) {
    return failure(VerificationEvidenceErrorCode::resource_exhausted,
                   "verification inline evidence exceeds its item limit",
                   evidence_id);
  }

  std::set<VerificationOutputStream> streams;
  std::uint64_t inline_bytes = evidence.summary.size();
  for (const auto& output : evidence.output) {
    const bool omits_bytes = output.represented_bytes > output.text.size();
    if (!known_stream(output.stream) || !streams.insert(output.stream).second ||
        !bounded_text(output.text, limits.maximum_excerpt_bytes, true) ||
        (output.text.empty() && !output.complete_artifact_id) ||
        output.represented_bytes < output.text.size() ||
        output.truncated != omits_bytes ||
        (evidence.outcome == VerificationOutcome::passed && omits_bytes &&
         !output.complete_artifact_id)) {
      return failure(VerificationEvidenceErrorCode::invalid_output,
                     "verification output excerpt is invalid", evidence_id);
    }
    if (!add_checked(inline_bytes, output.text.size())) {
      return failure(VerificationEvidenceErrorCode::overflow,
                     "verification inline output size overflowed", evidence_id);
    }
  }

  for (const auto& diagnostic : evidence.diagnostics) {
    if (!known_severity(diagnostic.severity) ||
        !bounded_text(diagnostic.code, limits.maximum_metadata_bytes, true) ||
        !bounded_text(diagnostic.message, limits.maximum_diagnostic_bytes)) {
      return failure(VerificationEvidenceErrorCode::invalid_diagnostic,
                     "verification diagnostic is invalid", evidence_id);
    }
    if (diagnostic.source &&
        (!valid_snapshot(diagnostic.source->snapshot) ||
         diagnostic.source->snapshot != evidence.source_snapshot ||
         !valid_relative_path(diagnostic.source->relative_path,
                              limits.maximum_metadata_bytes) ||
         !valid_digest(diagnostic.source->content_digest) ||
         (diagnostic.source->range &&
          (diagnostic.source->range->begin >= diagnostic.source->range->end ||
           diagnostic.source->range->end >
               diagnostic.source->content_digest.byte_size)))) {
      return failure(VerificationEvidenceErrorCode::invalid_diagnostic,
                     "verification diagnostic source is invalid", evidence_id);
    }
    if (!add_checked(inline_bytes, diagnostic.code.size()) ||
        !add_checked(inline_bytes, diagnostic.message.size())) {
      return failure(VerificationEvidenceErrorCode::overflow,
                     "verification diagnostic size overflowed", evidence_id);
    }
  }
  if (inline_bytes > limits.maximum_total_inline_bytes) {
    return failure(VerificationEvidenceErrorCode::resource_exhausted,
                   "verification total inline evidence exceeds its limit",
                   evidence_id);
  }

  const auto artifacts = artifact_ids(evidence);
  const std::set<ArtifactId> unique_artifacts{artifacts.begin(),
                                              artifacts.end()};
  if (artifacts.size() > limits.maximum_artifacts) {
    return failure(VerificationEvidenceErrorCode::resource_exhausted,
                   "verification evidence references too many artifacts",
                   evidence_id);
  }
  if (artifacts.size() != unique_artifacts.size()) {
    return failure(
        VerificationEvidenceErrorCode::duplicate_artifact,
        "verification evidence contains a duplicate artifact reference",
        evidence_id);
  }
  return {};
}

} // namespace

auto validate_verification_evidence(
    const domain::VerificationEvidence& evidence,
    const VerificationEvidenceLimits& limits)
    -> std::expected<void, VerificationEvidenceError> {
  try {
    return validate_impl(evidence, limits);
  } catch (...) {
    return failure(VerificationEvidenceErrorCode::internal_failure,
                   "verification evidence validation failed internally",
                   evidence.evidence_id);
  }
}

auto assess_verification_evidence(
    const domain::VerificationEvidence& evidence,
    const VerificationEvidenceEnvironment& environment,
    const VerificationEvidenceLimits& limits)
    -> std::expected<VerificationEvidenceAssessment,
                     VerificationEvidenceError> {
  try {
    if (auto valid = validate_impl(evidence, limits); !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    if ((environment.source_snapshot &&
         !valid_snapshot(*environment.source_snapshot)) ||
        (environment.build_configuration &&
         !valid_digest(*environment.build_configuration))) {
      return failure(VerificationEvidenceErrorCode::invalid_source,
                     "verification assessment environment is invalid",
                     evidence.evidence_id);
    }
    const std::set<ArtifactId> unique_available{
        environment.available_artifacts.begin(),
        environment.available_artifacts.end()};
    if (unique_available.size() != environment.available_artifacts.size()) {
      return failure(VerificationEvidenceErrorCode::invalid_evidence,
                     "verification artifact observations are duplicated",
                     evidence.evidence_id);
    }
    VerificationEvidenceAssessment result{domain::EvidenceFreshness::current,
                                          {}};
    if (!environment.source_snapshot ||
        environment.source_snapshot->repository_id !=
            evidence.source_snapshot.repository_id) {
      result.freshness = domain::EvidenceFreshness::unavailable;
      result.affected_triggers.push_back(
          VerificationInvalidationTrigger::source_snapshot_changed);
      return result;
    }
    if (!domain::same_source_state(*environment.source_snapshot,
                                   evidence.source_snapshot)) {
      result.freshness = domain::EvidenceFreshness::stale;
      result.affected_triggers.push_back(
          VerificationInvalidationTrigger::source_snapshot_changed);
    }
    if (evidence.build_configuration) {
      if (!environment.build_configuration ||
          *environment.build_configuration != *evidence.build_configuration) {
        result.affected_triggers.push_back(
            VerificationInvalidationTrigger::build_configuration_changed);
        if (result.freshness != domain::EvidenceFreshness::stale) {
          result.freshness = environment.build_configuration
                                 ? domain::EvidenceFreshness::stale
                                 : domain::EvidenceFreshness::possibly_stale;
        }
      }
    }
    const auto artifacts = artifact_ids(evidence);
    const bool missing = std::ranges::any_of(artifacts, [&](const auto& id) {
      return !unique_available.contains(id);
    });
    if (missing) {
      result.affected_triggers.push_back(
          VerificationInvalidationTrigger::artifact_unavailable);
      if (environment.artifact_observation_complete) {
        result.freshness = domain::EvidenceFreshness::unavailable;
      } else if (result.freshness == domain::EvidenceFreshness::current) {
        result.freshness = domain::EvidenceFreshness::possibly_stale;
      }
    }
    return result;
  } catch (...) {
    return failure(VerificationEvidenceErrorCode::internal_failure,
                   "verification evidence assessment failed internally",
                   evidence.evidence_id);
  }
}

auto make_verification_context_item(
    const domain::VerificationEvidence& evidence,
    domain::EvidenceId context_evidence_id,
    const domain::EvidenceFreshness freshness,
    const std::uint64_t estimated_tokens,
    const VerificationEvidenceLimits& limits)
    -> std::expected<domain::ContextParcelItem, VerificationEvidenceError> {
  try {
    if (auto valid = validate_impl(evidence, limits); !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    constexpr std::size_t maximum_context_blocks{1024};
    std::size_t content_blocks{1 + evidence.diagnostics.size() +
                               artifact_ids(evidence).size()};
    content_blocks += std::ranges::count_if(
        evidence.output, [](const VerificationOutputExcerpt& output) {
          return !output.text.empty();
        });
    if (!known_freshness(freshness) ||
        (freshness != EvidenceFreshness::unavailable &&
         estimated_tokens == 0)) {
      return failure(VerificationEvidenceErrorCode::invalid_evidence,
                     "verification context parameters are invalid",
                     evidence.evidence_id);
    }
    if (content_blocks > maximum_context_blocks) {
      return failure(VerificationEvidenceErrorCode::resource_exhausted,
                     "verification context contains too many content blocks",
                     evidence.evidence_id);
    }
    auto artifacts = artifact_ids(evidence);
    domain::ContextParcelItem result{
        std::move(context_evidence_id),
        domain::VerificationEvidenceReference{evidence.evidence_id, artifacts},
        freshness,
        {domain::EvidenceDerivation::observed,
         evidence.producer.name,
         evidence.producer.version,
         evidence.observed_at,
         evidence.source_snapshot,
         {},
         {},
         evidence.producer.invocation_id},
        {},
        0,
        freshness == domain::EvidenceFreshness::unavailable ? std::uint64_t{}
                                                            : estimated_tokens};
    if (freshness == domain::EvidenceFreshness::unavailable) return result;

    result.content.emplace_back(domain::TextBlock{evidence.summary});
    result.estimated_bytes = evidence.summary.size();
    for (const auto& output : evidence.output) {
      if (!output.text.empty()) {
        result.content.emplace_back(domain::TextBlock{output.text});
      }
      if (!add_checked(result.estimated_bytes, output.represented_bytes)) {
        return failure(VerificationEvidenceErrorCode::overflow,
                       "verification represented output size overflowed",
                       evidence.evidence_id);
      }
    }
    for (const auto& diagnostic : evidence.diagnostics) {
      auto text = diagnostic.code.empty()
                      ? diagnostic.message
                      : diagnostic.code + ": " + diagnostic.message;
      result.content.emplace_back(domain::TextBlock{std::move(text)});
      if (!add_checked(result.estimated_bytes, diagnostic.code.size()) ||
          !add_checked(result.estimated_bytes, diagnostic.message.size()) ||
          (!diagnostic.code.empty() &&
           !add_checked(result.estimated_bytes, 2))) {
        return failure(VerificationEvidenceErrorCode::overflow,
                       "verification represented diagnostic size overflowed",
                       evidence.evidence_id);
      }
    }
    for (const auto& artifact : artifacts) {
      result.content.emplace_back(domain::ArtifactReferenceBlock{
          artifact, std::string{"verification output"}});
      if (!add_checked(result.estimated_bytes, artifact.value().size()) ||
          !add_checked(result.estimated_bytes,
                       std::string_view{"verification output"}.size())) {
        return failure(VerificationEvidenceErrorCode::overflow,
                       "verification artifact reference size overflowed",
                       evidence.evidence_id);
      }
    }
    return result;
  } catch (...) {
    return failure(VerificationEvidenceErrorCode::internal_failure,
                   "verification context projection failed internally",
                   evidence.evidence_id);
  }
}

} // namespace aiforge::repository
