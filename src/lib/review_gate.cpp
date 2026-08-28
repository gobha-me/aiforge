#include <aiforge/runtime/review_gate.hpp>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {

using namespace domain;

[[nodiscard]] auto error(const ReviewGateErrorCode code, std::string message,
                         const bool retryable = false) -> ReviewGateError {
  return {code, std::move(message), retryable};
}

[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum = 4096) -> bool {
  return !value.empty() && value.size() <= maximum &&
         std::ranges::none_of(value, [](const unsigned char character) {
           return character < 0x20U || character == 0x7FU;
         });
}

[[nodiscard]] auto known_kind(const ReviewEvidenceKind kind) -> bool {
  return kind == ReviewEvidenceKind::verification ||
         kind == ReviewEvidenceKind::scenario;
}

[[nodiscard]] auto valid_requirement(const ReviewPolicyRequirement& requirement)
    -> bool {
  if (!known_kind(requirement.kind) ||
      !bounded_text(requirement.producer_name) ||
      !bounded_text(requirement.producer_version)) {
    return false;
  }
  const bool scenario = requirement.kind == ReviewEvidenceKind::scenario;
  return scenario == requirement.scenario_id.has_value() &&
         scenario == requirement.scenario_corpus_version.has_value() &&
         (!requirement.scenario_id || bounded_text(*requirement.scenario_id)) &&
         (!requirement.scenario_corpus_version ||
          bounded_text(*requirement.scenario_corpus_version));
}

template <typename Value>
[[nodiscard]] auto unique(const std::vector<Value>& values) -> bool {
  return std::set<Value>{values.begin(), values.end()}.size() == values.size();
}

auto add_trigger(std::vector<ReviewInvalidationTrigger>& triggers,
                 const ReviewInvalidationTrigger trigger) -> void {
  if (std::ranges::find(triggers, trigger) == triggers.end()) {
    triggers.push_back(trigger);
  }
}

[[nodiscard]] auto evidence_invalidated(
    const std::vector<ReviewInvalidationTrigger>& triggers) -> bool {
  return std::ranges::any_of(triggers, [](const auto trigger) {
    switch (trigger) {
      case ReviewInvalidationTrigger::candidate_changed:
      case ReviewInvalidationTrigger::requirement_missing:
      case ReviewInvalidationTrigger::evidence_changed:
      case ReviewInvalidationTrigger::verifier_version_changed:
      case ReviewInvalidationTrigger::scenario_version_changed:
      case ReviewInvalidationTrigger::artifact_missing_or_changed: return true;
      case ReviewInvalidationTrigger::findings_open:
      case ReviewInvalidationTrigger::verdict_missing:
      case ReviewInvalidationTrigger::verdict_conflicted:
      case ReviewInvalidationTrigger::verdict_not_approved:
      case ReviewInvalidationTrigger::approval_revoked:
      case ReviewInvalidationTrigger::override_untrusted: return false;
    }
    return false;
  });
}

auto append_digest_field(std::string& output, const std::string_view value)
    -> void {
  output += std::to_string(value.size());
  output.push_back(':');
  output.append(value);
}

auto append_optional_digest_field(std::string& output,
                                  const std::optional<std::string>& value)
    -> void {
  append_digest_field(output, value ? "present" : "absent");
  if (value) append_digest_field(output, *value);
}

auto append_content_digest(std::string& output, const ContentDigest& digest)
    -> void {
  append_digest_field(output, digest.algorithm);
  append_digest_field(output, digest.value);
  append_digest_field(output, std::to_string(digest.byte_size));
}

auto append_participant(std::string& output,
                        const ReviewParticipantProvenance& participant)
    -> void {
  append_digest_field(output, participant.actor.actor_id);
  append_digest_field(output, participant.actor.display_name);
  append_digest_field(output,
                      participant.run_id ? participant.run_id->value() : "");
  append_optional_digest_field(output, participant.backend_id);
  append_optional_digest_field(output, participant.backend_version);
  append_digest_field(
      output, participant.model_id ? participant.model_id->value() : "");
  append_optional_digest_field(output, participant.model_version);
}

[[nodiscard]] auto same_artifacts(
    const std::vector<ReviewArtifactDigest>& left,
    const std::vector<ReviewArtifactDigest>& right) -> bool {
  if (left.size() != right.size()) return false;
  return std::ranges::all_of(left, [&](const auto& expected) {
    const auto found = std::ranges::find(right, expected.artifact_id,
                                         &ReviewArtifactDigest::artifact_id);
    return found != right.end() && found->digest == expected.digest;
  });
}

[[nodiscard]] auto decision_digest(
    const ReviewReceiptDraft& draft,
    const repository::ReviewReceiptProjection& projection,
    const ReviewAuthorizationPolicy& policy,
    const ReviewAuthorizationSource source,
    const repository::ProjectedReviewOverride* selected_override)
    -> ContentDigest {
  std::string canonical;
  append_digest_field(canonical, draft.receipt_id.value());
  append_digest_field(canonical,
                      draft.candidate.snapshot.repository_id.value());
  append_content_digest(canonical, draft.candidate.snapshot.fingerprint);
  append_digest_field(canonical, draft.candidate.revision);
  append_digest_field(canonical,
                      draft.author ? "author-present" : "author-absent");
  if (draft.author) append_participant(canonical, *draft.author);
  append_digest_field(canonical, source == ReviewAuthorizationSource::receipt
                                     ? "receipt"
                                     : "override");
  for (const auto& required : policy.required_evidence) {
    append_digest_field(canonical, required.requirement_id.value());
    append_digest_field(canonical, required.producer_name);
    append_digest_field(canonical, required.producer_version);
    append_optional_digest_field(canonical, required.scenario_id);
    append_optional_digest_field(canonical, required.scenario_corpus_version);
  }
  for (const auto& binding : draft.evidence) {
    append_digest_field(canonical, binding.requirement_id.value());
    append_digest_field(canonical, binding.producer_name);
    append_digest_field(canonical, binding.producer_version);
    append_optional_digest_field(canonical, binding.scenario_id);
    append_optional_digest_field(canonical, binding.scenario_corpus_version);
    append_optional_digest_field(canonical,
                                 binding.scenario_application_revision);
    append_content_digest(canonical, binding.result_digest);
    if (binding.scenario_fake_script_digest) {
      append_digest_field(canonical, "fake-script-present");
      append_content_digest(canonical, *binding.scenario_fake_script_digest);
    } else {
      append_digest_field(canonical, "fake-script-absent");
    }
    if (binding.scenario_terminal_capabilities_digest) {
      append_digest_field(canonical, "terminal-capabilities-present");
      append_content_digest(canonical,
                            *binding.scenario_terminal_capabilities_digest);
    } else {
      append_digest_field(canonical, "terminal-capabilities-absent");
    }
    for (const auto& artifact : binding.artifacts) {
      append_digest_field(canonical, artifact.artifact_id.value());
      append_content_digest(canonical, artifact.digest);
    }
  }
  for (const auto& finding : projection.findings()) {
    append_digest_field(canonical, finding.finding.finding_id.value());
    append_digest_field(canonical, finding.finding.summary);
    append_digest_field(
        canonical, std::to_string(static_cast<int>(finding.finding.severity)));
    append_digest_field(canonical, finding.finding.source ? "source-present"
                                                          : "source-absent");
    if (finding.finding.source) {
      append_digest_field(
          canonical, finding.finding.source->snapshot.repository_id.value());
      append_content_digest(canonical,
                            finding.finding.source->snapshot.fingerprint);
      append_digest_field(canonical, finding.finding.source->relative_path);
      append_content_digest(canonical, finding.finding.source->content_digest);
    }
    for (const auto& artifact : finding.finding.artifacts) {
      append_digest_field(canonical, artifact.value());
    }
    for (const auto& evidence : finding.finding.reproduction_evidence_ids) {
      append_digest_field(canonical, evidence.value());
    }
    append_digest_field(canonical, finding.open ? "open" : "resolved");
  }
  for (const auto& verdict : projection.verdicts()) {
    append_digest_field(canonical, verdict.event_id.value());
    append_digest_field(canonical, verdict.reviewer.actor_id);
    append_digest_field(canonical, verdict.active ? "active" : "revoked");
    append_digest_field(canonical,
                        std::to_string(static_cast<int>(verdict.verdict)));
    append_digest_field(canonical, verdict.reviewer_provenance
                                       ? "provenance-present"
                                       : "provenance-absent");
    if (verdict.reviewer_provenance) {
      append_participant(canonical, *verdict.reviewer_provenance);
    }
  }
  if (selected_override != nullptr) {
    append_digest_field(canonical,
                        selected_override->value.override_id.value());
    append_digest_field(canonical, selected_override->value.actor.actor_id);
    append_digest_field(canonical, selected_override->value.reason);
  }
  std::uint64_t hash{14695981039346656037ULL};
  for (const unsigned char byte : canonical) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream value;
  value << std::hex << std::setfill('0') << std::setw(16) << hash;
  return {"fnv1a64", std::move(value).str(), canonical.size()};
}

} // namespace

auto ReviewMergeGate::evaluate(
    const repository::ReviewReceiptProjection& receipt,
    const ReviewAuthorizationPolicy& policy,
    const ReviewGateEnvironment& environment,
    HostedReviewCheckPort* hosted_check) const
    -> std::expected<ReviewGateDecision, ReviewGateError> {
  try {
    if (!receipt.draft() || !receipt.receipt_id()) {
      return std::unexpected(error(ReviewGateErrorCode::invalid_projection,
                                   "review receipt has not been drafted"));
    }
    if (policy.required_evidence.empty() ||
        std::ranges::any_of(policy.required_evidence,
                            [](const auto& requirement) {
                              return !valid_requirement(requirement);
                            })) {
      return std::unexpected(error(ReviewGateErrorCode::invalid_policy,
                                   "review authorization policy is invalid"));
    }
    std::vector<ReviewRequirementId> policy_ids;
    policy_ids.reserve(policy.required_evidence.size());
    for (const auto& required : policy.required_evidence) {
      policy_ids.push_back(required.requirement_id);
    }
    if (!unique(policy_ids) || !unique(policy.trusted_override_actor_ids) ||
        std::ranges::any_of(
            policy.trusted_override_actor_ids,
            [](const auto& actor) { return !bounded_text(actor); }) ||
        (!policy.allow_human_override &&
         !policy.trusted_override_actor_ids.empty())) {
      return std::unexpected(
          error(ReviewGateErrorCode::invalid_policy,
                "review authorization policy is inconsistent"));
    }

    if (auto valid = repository::validate_review_candidate(
            environment.current_candidate);
        !valid) {
      return std::unexpected(error(ReviewGateErrorCode::invalid_environment,
                                   "current review candidate is invalid"));
    }
    std::vector<ReviewRequirementId> environment_ids;
    environment_ids.reserve(environment.current_evidence.size());
    for (const auto& binding : environment.current_evidence) {
      if (!repository::validate_review_evidence_binding(binding)) {
        return std::unexpected(error(ReviewGateErrorCode::invalid_environment,
                                     "current review evidence is invalid"));
      }
      environment_ids.push_back(binding.requirement_id);
    }
    if (!unique(environment_ids)) {
      return std::unexpected(error(ReviewGateErrorCode::invalid_environment,
                                   "current review evidence is duplicated"));
    }

    ReviewGateDecision result;
    const auto& draft = *receipt.draft();
    if (environment.current_candidate != draft.candidate) {
      add_trigger(result.triggers,
                  ReviewInvalidationTrigger::candidate_changed);
    }

    for (const auto& required : policy.required_evidence) {
      const auto recorded =
          std::ranges::find(draft.evidence, required.requirement_id,
                            &ReviewEvidenceBinding::requirement_id);
      if (recorded == draft.evidence.end()) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::requirement_missing);
        continue;
      }
      if (recorded->producer_name != required.producer_name ||
          recorded->producer_version != required.producer_version ||
          recorded->kind != required.kind) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::verifier_version_changed);
      }
      if (recorded->scenario_id != required.scenario_id ||
          recorded->scenario_corpus_version !=
              required.scenario_corpus_version) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::scenario_version_changed);
      }
      const auto current = std::ranges::find(
          environment.current_evidence, required.requirement_id,
          &ReviewEvidenceBinding::requirement_id);
      if (current == environment.current_evidence.end()) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::requirement_missing);
        continue;
      }
      if (current->kind != required.kind ||
          current->producer_name != required.producer_name ||
          current->producer_version != required.producer_version) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::verifier_version_changed);
      }
      if (current->scenario_id != required.scenario_id ||
          current->scenario_corpus_version !=
              required.scenario_corpus_version) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::scenario_version_changed);
      }
      if (current->result_digest != recorded->result_digest ||
          current->verification_evidence_id !=
              recorded->verification_evidence_id ||
          current->scenario_application_revision !=
              recorded->scenario_application_revision ||
          current->scenario_fake_script_digest !=
              recorded->scenario_fake_script_digest ||
          current->scenario_terminal_capabilities_digest !=
              recorded->scenario_terminal_capabilities_digest) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::evidence_changed);
      }
      if (!same_artifacts(recorded->artifacts, current->artifacts)) {
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::artifact_missing_or_changed);
      }
    }

    switch (receipt.state()) {
      case repository::ReviewReceiptState::approved: break;
      case repository::ReviewReceiptState::findings_open:
        add_trigger(result.triggers, ReviewInvalidationTrigger::findings_open);
        break;
      case repository::ReviewReceiptState::conflicted:
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::verdict_conflicted);
        break;
      case repository::ReviewReceiptState::changes_requested:
      case repository::ReviewReceiptState::rejected:
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::verdict_not_approved);
        break;
      case repository::ReviewReceiptState::revoked:
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::approval_revoked);
        break;
      case repository::ReviewReceiptState::not_started:
      case repository::ReviewReceiptState::draft:
      case repository::ReviewReceiptState::review_requested:
        add_trigger(result.triggers,
                    ReviewInvalidationTrigger::verdict_missing);
        break;
    }

    const auto active_override =
        std::ranges::find_if(receipt.overrides(), [&](const auto& value) {
          return value.active &&
                 std::ranges::find(policy.trusted_override_actor_ids,
                                   value.value.actor.actor_id) !=
                     policy.trusted_override_actor_ids.end();
        });
    const bool trusted_override = policy.allow_human_override &&
                                  active_override != receipt.overrides().end();
    if (!trusted_override &&
        std::ranges::any_of(receipt.overrides(),
                            [](const auto& value) { return value.active; })) {
      add_trigger(result.triggers,
                  ReviewInvalidationTrigger::override_untrusted);
    }

    if (std::ranges::find(result.triggers,
                          ReviewInvalidationTrigger::candidate_changed) !=
        result.triggers.end()) {
      result.state = ReviewGateState::invalidated;
      result.explanation =
          "merge authorization denied because the candidate changed";
      return result;
    }

    std::optional<ReviewAuthorizationSource> source;
    if (result.triggers.empty()) {
      source = ReviewAuthorizationSource::receipt;
    } else if (trusted_override) {
      source = ReviewAuthorizationSource::human_override;
    } else {
      if (evidence_invalidated(result.triggers)) {
        result.state = ReviewGateState::invalidated;
      }
      result.explanation = "merge authorization denied because current review "
                           "policy is not satisfied";
      return result;
    }

    const auto* selected_override =
        *source == ReviewAuthorizationSource::human_override ? &*active_override
                                                             : nullptr;
    const auto digest =
        decision_digest(draft, receipt, policy, *source, selected_override);
    if (policy.require_hosted_check) {
      if (hosted_check == nullptr) {
        return std::unexpected(
            error(ReviewGateErrorCode::hosted_check_failure,
                  "required hosted review check is unavailable", true));
      }
      HostedReviewCheckUpdate update{
          draft.receipt_id, draft.candidate, HostedReviewCheckState::success,
          digest,
          *source == ReviewAuthorizationSource::human_override
              ? "APPROVED BY EXPLICIT HUMAN OVERRIDE"
              : "review receipt approved for the exact candidate"};
      auto confirmation = hosted_check->publish(update);
      if (!confirmation) {
        return std::unexpected(
            error(ReviewGateErrorCode::hosted_check_failure,
                  "required hosted review check could not be published",
                  confirmation.error().retryable));
      }
      if (confirmation->candidate != draft.candidate ||
          confirmation->state != HostedReviewCheckState::success ||
          confirmation->decision_digest != digest) {
        return std::unexpected(error(
            ReviewGateErrorCode::hosted_check_failure,
            "hosted review check confirmation did not match the candidate"));
      }
      result.hosted_check = *confirmation;
    }
    result.authorization =
        MergeAuthorization{draft.receipt_id, draft.candidate, *source, digest};
    result.state = *source == ReviewAuthorizationSource::human_override
                       ? ReviewGateState::overridden
                       : ReviewGateState::authorized;
    result.explanation =
        *source == ReviewAuthorizationSource::human_override
            ? "merge authorized by an explicit trusted human override"
            : "merge authorized by the current review receipt";
    return result;
  } catch (...) {
    return std::unexpected(error(ReviewGateErrorCode::internal_failure,
                                 "review merge gate failed internally"));
  }
}

} // namespace aiforge::runtime
