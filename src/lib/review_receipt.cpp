#include <aiforge/repository/review_receipt.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::repository {
namespace {

using namespace domain;

[[nodiscard]] auto failure(
    const ReviewReceiptErrorCode code, std::string message,
    std::optional<ReviewReceiptId> receipt_id = std::nullopt)
    -> std::unexpected<ReviewReceiptError> {
  return std::unexpected(
      ReviewReceiptError{code, std::move(message), std::move(receipt_id)});
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

[[nodiscard]] auto bounded_identity(const std::string_view value,
                                    const std::size_t maximum) -> bool {
  return bounded_text(value, maximum) &&
         std::ranges::none_of(value, [](const unsigned char character) {
           return character < 0x20U || character == 0x7FU;
         });
}

[[nodiscard]] auto valid_digest(const ContentDigest& digest) -> bool {
  return bounded_text(digest.algorithm, 128) &&
         bounded_text(digest.value, 512) &&
         std::ranges::all_of(digest.algorithm, [](const unsigned char value) {
           return std::isalnum(value) != 0 || value == '-' || value == '_' ||
                  value == '.';
         }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] auto valid_candidate(const ReviewCandidate& candidate,
                                   const ReviewReceiptLimits& limits) -> bool {
  return valid_digest(candidate.snapshot.fingerprint) &&
         bounded_identity(candidate.revision, limits.maximum_metadata_bytes);
}

[[nodiscard]] auto valid_actor(const ReviewActor& actor,
                               const ReviewReceiptLimits& limits) -> bool {
  return bounded_identity(actor.actor_id, limits.maximum_metadata_bytes) &&
         bounded_identity(actor.display_name, limits.maximum_metadata_bytes);
}

[[nodiscard]] auto valid_limits(const ReviewReceiptLimits& limits) -> bool {
  constexpr ReviewReceiptLimits maximums;
  return limits.maximum_evidence > 0 &&
         limits.maximum_evidence <= maximums.maximum_evidence &&
         limits.maximum_artifacts_per_evidence > 0 &&
         limits.maximum_artifacts_per_evidence <=
             maximums.maximum_artifacts_per_evidence &&
         limits.maximum_findings > 0 &&
         limits.maximum_findings <= maximums.maximum_findings &&
         limits.maximum_finding_artifacts > 0 &&
         limits.maximum_finding_artifacts <= maximums.maximum_finding_artifacts &&
         limits.maximum_text_bytes > 0 &&
         limits.maximum_text_bytes <= maximums.maximum_text_bytes &&
         limits.maximum_metadata_bytes > 0 &&
         limits.maximum_metadata_bytes <= maximums.maximum_metadata_bytes;
}

template <typename Value>
[[nodiscard]] auto unique(const std::vector<Value>& values) -> bool {
  return std::set<Value>{values.begin(), values.end()}.size() == values.size();
}

[[nodiscard]] auto known_kind(const ReviewEvidenceKind kind) -> bool {
  return kind == ReviewEvidenceKind::verification ||
         kind == ReviewEvidenceKind::scenario;
}

[[nodiscard]] auto known_verdict(const ReviewVerdict verdict) -> bool {
  return verdict == ReviewVerdict::approved ||
         verdict == ReviewVerdict::changes_requested ||
         verdict == ReviewVerdict::rejected;
}

[[nodiscard]] auto review_receipt_id(const RunEventPayload& payload)
    -> const ReviewReceiptId* {
  if (const auto* value = std::get_if<ReviewReceiptDrafted>(&payload)) {
    return &value->draft.receipt_id;
  }
  if (const auto* value = std::get_if<ReviewRequested>(&payload)) {
    return &value->receipt_id;
  }
  if (const auto* value = std::get_if<ReviewFindingOpened>(&payload)) {
    return &value->receipt_id;
  }
  if (const auto* value = std::get_if<ReviewFindingResolved>(&payload)) {
    return &value->receipt_id;
  }
  if (const auto* value = std::get_if<ReviewVerdictRecorded>(&payload)) {
    return &value->receipt_id;
  }
  if (const auto* value = std::get_if<ReviewVerdictRevoked>(&payload)) {
    return &value->receipt_id;
  }
  if (const auto* value = std::get_if<ReviewOverrideRecorded>(&payload)) {
    return &value->override.receipt_id;
  }
  if (const auto* value = std::get_if<ReviewOverrideRevoked>(&payload)) {
    return &value->receipt_id;
  }
  return nullptr;
}

}  // namespace

auto validate_review_evidence_binding(
    const ReviewEvidenceBinding& binding, const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  if (!valid_limits(limits)) {
    return failure(ReviewReceiptErrorCode::invalid_limits,
                   "review receipt limits are invalid");
  }
  if (!known_kind(binding.kind) ||
      !bounded_identity(binding.producer_name, limits.maximum_metadata_bytes) ||
      !bounded_identity(binding.producer_version,
                        limits.maximum_metadata_bytes) ||
      !valid_digest(binding.result_digest) ||
      binding.artifacts.size() > limits.maximum_artifacts_per_evidence) {
    return failure(ReviewReceiptErrorCode::invalid_evidence,
                   "review evidence binding is invalid");
  }
  const bool verification = binding.kind == ReviewEvidenceKind::verification;
  if (verification != binding.verification_evidence_id.has_value() ||
      verification == binding.scenario_id.has_value() ||
      verification == binding.scenario_corpus_version.has_value() ||
      verification == binding.scenario_application_revision.has_value() ||
      verification == binding.scenario_fake_script_digest.has_value() ||
      verification ==
          binding.scenario_terminal_capabilities_digest.has_value() ||
      (binding.scenario_id &&
       !bounded_identity(*binding.scenario_id,
                         limits.maximum_metadata_bytes)) ||
      (binding.scenario_corpus_version &&
       !bounded_identity(*binding.scenario_corpus_version,
                         limits.maximum_metadata_bytes)) ||
      (binding.scenario_application_revision &&
       !bounded_identity(*binding.scenario_application_revision,
                         limits.maximum_metadata_bytes)) ||
      (binding.scenario_fake_script_digest &&
       !valid_digest(*binding.scenario_fake_script_digest)) ||
      (binding.scenario_terminal_capabilities_digest &&
       !valid_digest(*binding.scenario_terminal_capabilities_digest))) {
    return failure(ReviewReceiptErrorCode::invalid_evidence,
                   "review evidence identity does not match its kind");
  }
  std::vector<ArtifactId> artifact_ids;
  artifact_ids.reserve(binding.artifacts.size());
  for (const auto& artifact : binding.artifacts) {
    if (!valid_digest(artifact.digest)) {
      return failure(ReviewReceiptErrorCode::invalid_evidence,
                     "review artifact digest is invalid");
    }
    artifact_ids.push_back(artifact.artifact_id);
  }
  if (!unique(artifact_ids)) {
    return failure(ReviewReceiptErrorCode::duplicate_identity,
                   "review artifact identities must be unique");
  }
  return {};
}

auto validate_review_candidate(const ReviewCandidate& candidate,
                               const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  if (!valid_limits(limits)) {
    return failure(ReviewReceiptErrorCode::invalid_limits,
                   "review receipt limits are invalid");
  }
  if (!valid_candidate(candidate, limits)) {
    return failure(ReviewReceiptErrorCode::invalid_candidate,
                   "review candidate is invalid");
  }
  return {};
}

auto validate_review_actor(const ReviewActor& actor,
                           const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  if (!valid_limits(limits)) {
    return failure(ReviewReceiptErrorCode::invalid_limits,
                   "review receipt limits are invalid");
  }
  if (!valid_actor(actor, limits)) {
    return failure(ReviewReceiptErrorCode::invalid_actor,
                   "review actor is invalid");
  }
  return {};
}

auto validate_review_finding(const ReviewFinding& finding,
                             const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  if (!valid_limits(limits)) {
    return failure(ReviewReceiptErrorCode::invalid_limits,
                   "review receipt limits are invalid");
  }
  if (!bounded_text(finding.summary, limits.maximum_text_bytes) ||
      finding.artifacts.size() > limits.maximum_finding_artifacts ||
      !unique(finding.artifacts)) {
    return failure(ReviewReceiptErrorCode::invalid_finding,
                   "review finding is invalid");
  }
  return {};
}

auto validate_review_override(const ReviewOverride& value,
                              const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  if (auto valid = validate_review_candidate(value.candidate, limits); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  if (auto valid = validate_review_actor(value.actor, limits); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  if (!bounded_text(value.reason, limits.maximum_text_bytes)) {
    return failure(ReviewReceiptErrorCode::invalid_receipt,
                   "review override reason is invalid", value.receipt_id);
  }
  return {};
}

auto validate_review_receipt_draft(const ReviewReceiptDraft& draft,
                                   const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  if (!valid_limits(limits)) {
    return failure(ReviewReceiptErrorCode::invalid_limits,
                   "review receipt limits are invalid", draft.receipt_id);
  }
  if (auto valid = validate_review_candidate(draft.candidate, limits); !valid) {
    auto error = std::move(valid.error());
    error.receipt_id = draft.receipt_id;
    return std::unexpected(std::move(error));
  }
  if (draft.evidence.empty() || draft.evidence.size() > limits.maximum_evidence) {
    return failure(ReviewReceiptErrorCode::resource_exhausted,
                   "review evidence count is outside its bounds",
                   draft.receipt_id);
  }
  std::vector<ReviewRequirementId> requirement_ids;
  requirement_ids.reserve(draft.evidence.size());
  for (const auto& binding : draft.evidence) {
    if (auto valid = validate_review_evidence_binding(binding, limits); !valid) {
      auto error = std::move(valid.error());
      error.receipt_id = draft.receipt_id;
      return std::unexpected(std::move(error));
    }
    if (binding.kind == ReviewEvidenceKind::scenario &&
        binding.scenario_application_revision != draft.candidate.revision) {
      return failure(ReviewReceiptErrorCode::invalid_evidence,
                     "review scenario was not recorded for the candidate revision",
                     draft.receipt_id);
    }
    requirement_ids.push_back(binding.requirement_id);
  }
  if (!unique(requirement_ids)) {
    return failure(ReviewReceiptErrorCode::duplicate_identity,
                   "review requirement identities must be unique",
                   draft.receipt_id);
  }
  return {};
}

auto ReviewReceiptProjection::state() const noexcept -> ReviewReceiptState {
  if (!m_draft) return ReviewReceiptState::not_started;
  const auto active_verdicts = std::ranges::count_if(
      m_verdicts, [](const auto& verdict) { return verdict.active; });
  if (active_verdicts > 1) return ReviewReceiptState::conflicted;
  if (std::ranges::any_of(m_findings,
                          [](const auto& finding) { return finding.open; })) {
    return ReviewReceiptState::findings_open;
  }
  const auto verdict = std::ranges::find_if(
      m_verdicts, [](const auto& value) { return value.active; });
  if (verdict != m_verdicts.end()) {
    switch (verdict->verdict) {
      case ReviewVerdict::approved: return ReviewReceiptState::approved;
      case ReviewVerdict::changes_requested:
        return ReviewReceiptState::changes_requested;
      case ReviewVerdict::rejected: return ReviewReceiptState::rejected;
    }
  }
  if (m_approval_revoked) return ReviewReceiptState::revoked;
  if (m_review_requested) return ReviewReceiptState::review_requested;
  return ReviewReceiptState::draft;
}

auto ReviewReceiptProjection::apply(const RunEvent& event,
                                    const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  try {
    auto candidate = *this;
    if (auto applied = candidate.apply_in_place(event, limits); !applied) {
      return std::unexpected(std::move(applied.error()));
    }
    *this = std::move(candidate);
    return {};
  } catch (...) {
    return failure(ReviewReceiptErrorCode::internal_failure,
                   "review receipt projection failed internally", m_receipt_id);
  }
}

auto ReviewReceiptProjection::rebuild(std::span<const RunEvent> events,
                                      const ReviewReceiptLimits& limits)
    -> std::expected<ReviewReceiptProjection, ReviewReceiptError> {
  ReviewReceiptProjection result;
  for (const auto& event : events) {
    if (auto applied = result.apply(event, limits); !applied) {
      return std::unexpected(std::move(applied.error()));
    }
  }
  return result;
}

auto ReviewReceiptProjection::apply_in_place(
    const RunEvent& event, const ReviewReceiptLimits& limits)
    -> std::expected<void, ReviewReceiptError> {
  if (!valid_limits(limits)) {
    return failure(ReviewReceiptErrorCode::invalid_limits,
                   "review receipt limits are invalid", m_receipt_id);
  }
  if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
    return failure(ReviewReceiptErrorCode::invalid_envelope,
                   "review event envelope is invalid", m_receipt_id);
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return failure(ReviewReceiptErrorCode::non_monotonic_sequence,
                   "review event sequence did not increase", m_receipt_id);
  }
  if (m_event_ids.contains(event.metadata.event_id)) {
    return failure(ReviewReceiptErrorCode::duplicate_event,
                   "review event identity is duplicated", m_receipt_id);
  }

  const auto* payload_receipt = review_receipt_id(event.payload);
  if (payload_receipt && m_receipt_id && *payload_receipt != *m_receipt_id) {
    return failure(ReviewReceiptErrorCode::wrong_receipt,
                   "review event belongs to another receipt", m_receipt_id);
  }

  if (const auto* drafted = std::get_if<ReviewReceiptDrafted>(&event.payload)) {
    if (m_draft) {
      return failure(ReviewReceiptErrorCode::invalid_transition,
                     "review receipt may be drafted only once", m_receipt_id);
    }
    if (auto valid = validate_review_receipt_draft(drafted->draft, limits);
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    m_receipt_id = drafted->draft.receipt_id;
    m_draft = drafted->draft;
  } else if (payload_receipt) {
    if (!m_draft) {
      return failure(ReviewReceiptErrorCode::invalid_transition,
                     "review facts require a drafted receipt", *payload_receipt);
    }
    if (const auto* requested = std::get_if<ReviewRequested>(&event.payload)) {
      if (m_review_requested ||
          !validate_review_actor(requested->requested_by, limits)) {
        return failure(ReviewReceiptErrorCode::invalid_transition,
                       "review request is invalid or duplicated", m_receipt_id);
      }
      m_review_requested = true;
    } else if (const auto* opened =
                   std::get_if<ReviewFindingOpened>(&event.payload)) {
      if (!m_review_requested ||
          !validate_review_finding(opened->finding, limits) ||
          m_findings.size() >= limits.maximum_findings ||
          std::ranges::any_of(m_findings, [&](const auto& finding) {
            return finding.finding.finding_id == opened->finding.finding_id;
          })) {
        return failure(ReviewReceiptErrorCode::invalid_finding,
                       "review finding is invalid or duplicated", m_receipt_id);
      }
      m_findings.push_back({opened->finding, true});
    } else if (const auto* resolved =
                   std::get_if<ReviewFindingResolved>(&event.payload)) {
      if (!valid_actor(resolved->resolved_by, limits) ||
          (resolved->reason &&
           !bounded_text(*resolved->reason, limits.maximum_text_bytes))) {
        return failure(ReviewReceiptErrorCode::invalid_actor,
                       "review finding resolution is invalid", m_receipt_id);
      }
      const auto finding = std::ranges::find_if(
          m_findings, [&](const auto& value) {
            return value.finding.finding_id == resolved->finding_id;
          });
      if (finding == m_findings.end() || !finding->open) {
        return failure(ReviewReceiptErrorCode::unknown_finding,
                       "review finding is unknown or already resolved",
                       m_receipt_id);
      }
      finding->open = false;
    } else if (const auto* recorded =
                   std::get_if<ReviewVerdictRecorded>(&event.payload)) {
      if (!m_review_requested || !known_verdict(recorded->verdict) ||
          !valid_actor(recorded->reviewer, limits)) {
        return failure(ReviewReceiptErrorCode::invalid_transition,
                       "review verdict is invalid", m_receipt_id);
      }
      m_verdicts.push_back(
          {event.metadata.event_id, recorded->verdict, recorded->reviewer, true});
    } else if (const auto* revoked =
                   std::get_if<ReviewVerdictRevoked>(&event.payload)) {
      if (!valid_actor(revoked->revoked_by, limits) ||
          !bounded_text(revoked->reason, limits.maximum_text_bytes)) {
        return failure(ReviewReceiptErrorCode::invalid_actor,
                       "review verdict revocation is invalid", m_receipt_id);
      }
      const auto verdict = std::ranges::find(
          m_verdicts, revoked->verdict_event_id,
          &ProjectedReviewVerdict::event_id);
      if (verdict == m_verdicts.end() || !verdict->active) {
        return failure(ReviewReceiptErrorCode::unknown_verdict,
                       "review verdict is unknown or already revoked",
                       m_receipt_id);
      }
      if (verdict->verdict == ReviewVerdict::approved) m_approval_revoked = true;
      verdict->active = false;
    } else if (const auto* recorded =
                   std::get_if<ReviewOverrideRecorded>(&event.payload)) {
      if (!validate_review_override(recorded->override, limits) ||
          recorded->override.candidate != m_draft->candidate ||
          std::ranges::any_of(m_overrides, [&](const auto& value) {
            return value.value.override_id == recorded->override.override_id;
          })) {
        return failure(ReviewReceiptErrorCode::invalid_transition,
                       "review override is invalid or duplicated", m_receipt_id);
      }
      m_overrides.push_back({recorded->override, true});
    } else if (const auto* revoked =
                   std::get_if<ReviewOverrideRevoked>(&event.payload)) {
      if (!valid_actor(revoked->revoked_by, limits) ||
          !bounded_text(revoked->reason, limits.maximum_text_bytes)) {
        return failure(ReviewReceiptErrorCode::invalid_actor,
                       "review override revocation is invalid", m_receipt_id);
      }
      const auto override = std::ranges::find_if(
          m_overrides, [&](const auto& value) {
            return value.value.override_id == revoked->override_id;
          });
      if (override == m_overrides.end() || !override->active) {
        return failure(ReviewReceiptErrorCode::unknown_override,
                       "review override is unknown or already revoked",
                       m_receipt_id);
      }
      override->active = false;
    }
  }

  m_event_ids.insert(event.metadata.event_id);
  m_last_sequence = event.metadata.sequence;
  return {};
}

}  // namespace aiforge::repository
