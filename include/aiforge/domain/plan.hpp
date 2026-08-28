#pragma once

#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/content.hpp>
#include <aiforge/domain/repository.hpp>

namespace aiforge::domain {

struct PlanResourceIntent {
  Effect effect{Effect::read};
  std::string kind;
  std::string value;
  auto operator==(const PlanResourceIntent&) const -> bool = default;
};

struct PlanEvidenceBinding {
  EvidenceId evidence_id;
  ContentDigest digest;
  auto operator==(const PlanEvidenceBinding&) const -> bool = default;
};

struct PlanTask {
  PlanTaskId task_id;
  std::optional<PlanTaskId> parent_task_id;
  std::vector<PlanTaskId> dependency_task_ids;
  std::string title;
  std::vector<std::string> acceptance_criteria;
  std::vector<Effect> intended_effects;
  // Resource intents describe prospective scope and conflict inputs. They do
  // not grant capabilities or authorize effects.
  std::vector<PlanResourceIntent> resource_intents;
  auto operator==(const PlanTask&) const -> bool = default;
};

struct PlanRevision {
  PlanId plan_id;
  PlanRevisionId revision_id;
  std::optional<PlanRevisionId> supersedes_revision_id;
  std::string goal;
  std::optional<RepositorySnapshotIdentity> source_snapshot;
  std::vector<PlanTask> tasks;
  std::vector<PlanEvidenceBinding> evidence;
  auto operator==(const PlanRevision&) const -> bool = default;
};

enum class PlanDecision {
  approved,
  revision_requested,
  rejected,
};

enum class PlanDecisionSource {
  user,
  policy,
};

struct PlanRevisionDecision {
  PlanId plan_id;
  PlanRevisionId revision_id;
  PlanDecision decision{PlanDecision::rejected};
  PlanDecisionSource source{PlanDecisionSource::user};
  std::optional<std::string> reason;
  auto operator==(const PlanRevisionDecision&) const -> bool = default;
};

enum class PlanInvalidationTrigger {
  source_snapshot_changed,
  evidence_changed,
};

struct PlanRevisionInvalidation {
  PlanId plan_id;
  PlanRevisionId revision_id;
  std::vector<PlanInvalidationTrigger> triggers;
  auto operator==(const PlanRevisionInvalidation&) const -> bool = default;
};

} // namespace aiforge::domain
