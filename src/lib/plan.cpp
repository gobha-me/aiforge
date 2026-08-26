#include <aiforge/domain/plan_projection.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto
failure(const PlanGraphErrorCode code, std::string message,
        std::optional<PlanId> plan_id = std::nullopt,
        std::optional<PlanRevisionId> revision_id = std::nullopt,
        std::optional<PlanTaskId> task_id = std::nullopt)
    -> std::unexpected<PlanGraphError> {
  return std::unexpected(
      PlanGraphError{code, std::move(message), std::move(plan_id),
                     std::move(revision_id), std::move(task_id)});
}

[[nodiscard]] auto valid_utf8_text(const std::string_view value,
                                   const bool allow_empty = false) -> bool {
  if (!allow_empty && value.empty())
    return false;
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
      if (codepoint < 2)
        return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index)
      return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U)
        return false;
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

[[nodiscard]] auto valid_limits(const PlanGraphLimits &limits) -> bool {
  constexpr PlanGraphLimits maximums;
  return limits.maximum_tasks > 0 &&
         limits.maximum_tasks <= maximums.maximum_tasks &&
         limits.maximum_dependencies_per_task > 0 &&
         limits.maximum_dependencies_per_task <=
             maximums.maximum_dependencies_per_task &&
         limits.maximum_acceptance_criteria_per_task > 0 &&
         limits.maximum_acceptance_criteria_per_task <=
             maximums.maximum_acceptance_criteria_per_task &&
         limits.maximum_resource_intents_per_task > 0 &&
         limits.maximum_resource_intents_per_task <=
             maximums.maximum_resource_intents_per_task &&
         limits.maximum_text_bytes > 0 &&
         limits.maximum_text_bytes <= maximums.maximum_text_bytes &&
         limits.maximum_metadata_bytes > 0 &&
         limits.maximum_metadata_bytes <= maximums.maximum_metadata_bytes &&
         limits.maximum_total_text_bytes > 0 &&
         limits.maximum_total_text_bytes <= maximums.maximum_total_text_bytes &&
         limits.maximum_text_bytes <= limits.maximum_total_text_bytes;
}

[[nodiscard]] auto known_effect(const Effect effect) -> bool {
  switch (effect) {
  case Effect::read:
  case Effect::write:
  case Effect::remove:
  case Effect::execute:
  case Effect::network:
  case Effect::communicate:
  case Effect::spend:
  case Effect::change_infrastructure:
  case Effect::change_privileges:
    return true;
  }
  return false;
}

[[nodiscard]] auto known_decision(const PlanDecision decision) -> bool {
  return decision == PlanDecision::approved ||
         decision == PlanDecision::revision_requested ||
         decision == PlanDecision::rejected;
}

[[nodiscard]] auto known_source(const PlanDecisionSource source) -> bool {
  return source == PlanDecisionSource::user ||
         source == PlanDecisionSource::policy;
}

[[nodiscard]] auto valid_digest(const ContentDigest &digest) -> bool {
  return bounded_identity(digest.algorithm, 128) &&
         bounded_identity(digest.value, 512) &&
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

[[nodiscard]] auto add_text_bytes(std::size_t &total,
                                  const std::string_view value,
                                  const std::size_t maximum) -> bool {
  if (value.size() > std::numeric_limits<std::size_t>::max() - total) {
    return false;
  }
  total += value.size();
  return total <= maximum;
}

template <typename Edges>
[[nodiscard]] auto has_cycle(const std::vector<PlanTask> &tasks,
                             const std::map<PlanTaskId, std::size_t> &indices,
                             Edges edges) -> bool {
  enum class Visit { unseen, active, complete };
  std::vector<Visit> visits(tasks.size(), Visit::unseen);
  std::function<bool(std::size_t)> visit = [&](const std::size_t index) {
    if (visits[index] == Visit::active)
      return true;
    if (visits[index] == Visit::complete)
      return false;
    visits[index] = Visit::active;
    for (const auto &target : edges(tasks[index])) {
      const auto found = indices.find(target);
      if (found != indices.end() && visit(found->second))
        return true;
    }
    visits[index] = Visit::complete;
    return false;
  };
  for (std::size_t index = 0; index < tasks.size(); ++index) {
    if (visit(index))
      return true;
  }
  return false;
}

[[nodiscard]] auto plan_id_from(const RunEventPayload &payload)
    -> const PlanId * {
  if (const auto *proposed = std::get_if<PlanRevisionProposed>(&payload)) {
    return &proposed->revision.plan_id;
  }
  if (const auto *recorded =
          std::get_if<PlanRevisionDecisionRecorded>(&payload)) {
    return &recorded->decision.plan_id;
  }
  return nullptr;
}

} // namespace

auto validate_plan_revision(const PlanRevision &revision,
                            const PlanGraphLimits &limits)
    -> std::expected<void, PlanGraphError> {
  try {
    if (!valid_limits(limits)) {
      return failure(PlanGraphErrorCode::invalid_limits,
                     "plan graph limits are invalid", revision.plan_id,
                     revision.revision_id);
    }
    if (!bounded_text(revision.goal, limits.maximum_text_bytes) ||
        (revision.supersedes_revision_id &&
         *revision.supersedes_revision_id == revision.revision_id) ||
        (revision.source_snapshot &&
         !valid_digest(revision.source_snapshot->fingerprint))) {
      return failure(PlanGraphErrorCode::invalid_plan,
                     "plan revision metadata is invalid", revision.plan_id,
                     revision.revision_id);
    }
    if (revision.tasks.empty() ||
        revision.tasks.size() > limits.maximum_tasks) {
      return failure(PlanGraphErrorCode::resource_exhausted,
                     "plan task count is outside its bounds", revision.plan_id,
                     revision.revision_id);
    }

    std::size_t total_text_bytes{};
    if (!add_text_bytes(total_text_bytes, revision.goal,
                        limits.maximum_total_text_bytes)) {
      return failure(PlanGraphErrorCode::resource_exhausted,
                     "plan text exceeds its aggregate bound", revision.plan_id,
                     revision.revision_id);
    }

    std::map<PlanTaskId, std::size_t> indices;
    for (std::size_t index = 0; index < revision.tasks.size(); ++index) {
      const auto &task = revision.tasks[index];
      if (!indices.emplace(task.task_id, index).second) {
        return failure(PlanGraphErrorCode::duplicate_identity,
                       "plan task identity is duplicated", revision.plan_id,
                       revision.revision_id, task.task_id);
      }
    }

    for (const auto &task : revision.tasks) {
      if (!bounded_text(task.title, limits.maximum_text_bytes) ||
          task.acceptance_criteria.empty() ||
          task.acceptance_criteria.size() >
              limits.maximum_acceptance_criteria_per_task ||
          task.dependency_task_ids.size() >
              limits.maximum_dependencies_per_task ||
          task.resource_intents.size() >
              limits.maximum_resource_intents_per_task) {
        return failure(PlanGraphErrorCode::invalid_task,
                       "plan task fields are invalid or outside their bounds",
                       revision.plan_id, revision.revision_id, task.task_id);
      }
      if (!add_text_bytes(total_text_bytes, task.title,
                          limits.maximum_total_text_bytes)) {
        return failure(PlanGraphErrorCode::resource_exhausted,
                       "plan text exceeds its aggregate bound",
                       revision.plan_id, revision.revision_id, task.task_id);
      }

      std::set<PlanTaskId> dependencies;
      for (const auto &dependency : task.dependency_task_ids) {
        if (dependency == task.task_id || !indices.contains(dependency)) {
          return failure(PlanGraphErrorCode::unknown_reference,
                         "plan task dependency is missing or self-referential",
                         revision.plan_id, revision.revision_id, task.task_id);
        }
        if (!dependencies.insert(dependency).second) {
          return failure(PlanGraphErrorCode::duplicate_identity,
                         "plan task dependency is duplicated", revision.plan_id,
                         revision.revision_id, task.task_id);
        }
      }
      if (task.parent_task_id && (*task.parent_task_id == task.task_id ||
                                  !indices.contains(*task.parent_task_id))) {
        return failure(PlanGraphErrorCode::unknown_reference,
                       "plan task parent is missing or self-referential",
                       revision.plan_id, revision.revision_id, task.task_id);
      }

      std::set<Effect> intended_effects;
      for (const auto effect : task.intended_effects) {
        if (!known_effect(effect) || !intended_effects.insert(effect).second) {
          return failure(PlanGraphErrorCode::invalid_task,
                         "plan task effects are invalid or duplicated",
                         revision.plan_id, revision.revision_id, task.task_id);
        }
      }

      std::set<std::tuple<Effect, std::string, std::string>> intents;
      for (const auto &intent : task.resource_intents) {
        if (!known_effect(intent.effect) ||
            !intended_effects.contains(intent.effect) ||
            !bounded_identity(intent.kind, limits.maximum_metadata_bytes) ||
            !bounded_identity(intent.value, limits.maximum_text_bytes) ||
            !intents.emplace(intent.effect, intent.kind, intent.value).second) {
          return failure(PlanGraphErrorCode::invalid_task,
                         "plan resource intent is invalid or duplicated",
                         revision.plan_id, revision.revision_id, task.task_id);
        }
        if (!add_text_bytes(total_text_bytes, intent.kind,
                            limits.maximum_total_text_bytes) ||
            !add_text_bytes(total_text_bytes, intent.value,
                            limits.maximum_total_text_bytes)) {
          return failure(PlanGraphErrorCode::resource_exhausted,
                         "plan text exceeds its aggregate bound",
                         revision.plan_id, revision.revision_id, task.task_id);
        }
      }

      for (const auto &criterion : task.acceptance_criteria) {
        if (!bounded_text(criterion, limits.maximum_text_bytes)) {
          return failure(PlanGraphErrorCode::invalid_task,
                         "plan acceptance criterion is invalid",
                         revision.plan_id, revision.revision_id, task.task_id);
        }
        if (!add_text_bytes(total_text_bytes, criterion,
                            limits.maximum_total_text_bytes)) {
          return failure(PlanGraphErrorCode::resource_exhausted,
                         "plan acceptance criteria exceed their bounds",
                         revision.plan_id, revision.revision_id, task.task_id);
        }
      }
    }

    const auto parent_edges = [](const PlanTask &task) {
      return task.parent_task_id ? std::vector<PlanTaskId>{*task.parent_task_id}
                                 : std::vector<PlanTaskId>{};
    };
    if (has_cycle(revision.tasks, indices, parent_edges) ||
        has_cycle(revision.tasks, indices, [](const PlanTask &task) {
          return task.dependency_task_ids;
        })) {
      return failure(PlanGraphErrorCode::cyclic_graph,
                     "plan task graph contains a cycle", revision.plan_id,
                     revision.revision_id);
    }
    return {};
  } catch (...) {
    return failure(PlanGraphErrorCode::internal_failure,
                   "plan revision validation failed internally",
                   revision.plan_id, revision.revision_id);
  }
}

auto validate_plan_decision(const PlanRevisionDecision &decision,
                            const PlanGraphLimits &limits)
    -> std::expected<void, PlanGraphError> {
  try {
    if (!valid_limits(limits)) {
      return failure(PlanGraphErrorCode::invalid_limits,
                     "plan graph limits are invalid", decision.plan_id,
                     decision.revision_id);
    }
    if (!known_decision(decision.decision) || !known_source(decision.source) ||
        (decision.reason &&
         !bounded_text(*decision.reason, limits.maximum_text_bytes))) {
      return failure(PlanGraphErrorCode::invalid_decision,
                     "plan revision decision is invalid", decision.plan_id,
                     decision.revision_id);
    }
    return {};
  } catch (...) {
    return failure(PlanGraphErrorCode::internal_failure,
                   "plan decision validation failed internally",
                   decision.plan_id, decision.revision_id);
  }
}

auto PlanGraphProjection::current_revision() const noexcept
    -> const ProjectedPlanRevision * {
  return m_revisions.empty() ? nullptr : &m_revisions.back();
}

auto PlanGraphProjection::state() const noexcept -> PlanGraphState {
  const auto *current = current_revision();
  if (current == nullptr)
    return PlanGraphState::not_started;
  if (!current->decision)
    return PlanGraphState::proposed;
  switch (current->decision->decision) {
  case PlanDecision::approved:
    return PlanGraphState::approved;
  case PlanDecision::revision_requested:
    return PlanGraphState::revision_requested;
  case PlanDecision::rejected:
    return PlanGraphState::rejected;
  }
  return PlanGraphState::proposed;
}

auto PlanGraphProjection::apply(const RunEvent &event,
                                const PlanGraphLimits &limits)
    -> std::expected<void, PlanGraphError> {
  try {
    auto candidate = *this;
    if (auto applied = candidate.apply_in_place(event, limits); !applied) {
      return std::unexpected(std::move(applied.error()));
    }
    *this = std::move(candidate);
    return {};
  } catch (...) {
    return failure(PlanGraphErrorCode::internal_failure,
                   "plan graph projection failed internally", m_plan_id);
  }
}

auto PlanGraphProjection::rebuild(std::span<const RunEvent> events,
                                  const PlanGraphLimits &limits)
    -> std::expected<PlanGraphProjection, PlanGraphError> {
  PlanGraphProjection result;
  for (const auto &event : events) {
    if (auto applied = result.apply(event, limits); !applied) {
      return std::unexpected(std::move(applied.error()));
    }
  }
  return result;
}

auto PlanGraphProjection::apply_in_place(const RunEvent &event,
                                         const PlanGraphLimits &limits)
    -> std::expected<void, PlanGraphError> {
  if (!valid_limits(limits)) {
    return failure(PlanGraphErrorCode::invalid_limits,
                   "plan graph limits are invalid", m_plan_id);
  }
  if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
    return failure(PlanGraphErrorCode::invalid_envelope,
                   "plan event envelope is invalid", m_plan_id);
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return failure(PlanGraphErrorCode::non_monotonic_sequence,
                   "plan event sequence did not increase", m_plan_id);
  }
  if (m_event_ids.contains(event.metadata.event_id)) {
    return failure(PlanGraphErrorCode::duplicate_event,
                   "plan event identity is duplicated", m_plan_id);
  }

  const auto *payload_plan_id = plan_id_from(event.payload);
  if (payload_plan_id && m_plan_id && *payload_plan_id != *m_plan_id) {
    return failure(PlanGraphErrorCode::wrong_plan,
                   "plan event belongs to another plan", m_plan_id);
  }

  if (const auto *proposed =
          std::get_if<PlanRevisionProposed>(&event.payload)) {
    if (auto valid = validate_plan_revision(proposed->revision, limits);
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    const auto *current = current_revision();
    if ((current == nullptr && proposed->revision.supersedes_revision_id) ||
        (current != nullptr &&
         proposed->revision.supersedes_revision_id !=
             std::optional{current->revision.revision_id})) {
      return failure(PlanGraphErrorCode::invalid_transition,
                     "plan revision does not supersede the current revision",
                     proposed->revision.plan_id,
                     proposed->revision.revision_id);
    }
    if (std::ranges::any_of(m_revisions, [&](const auto &revision) {
          return revision.revision.revision_id ==
                 proposed->revision.revision_id;
        })) {
      return failure(PlanGraphErrorCode::duplicate_identity,
                     "plan revision identity is duplicated",
                     proposed->revision.plan_id,
                     proposed->revision.revision_id);
    }
    if (!m_plan_id)
      m_plan_id = proposed->revision.plan_id;
    m_revisions.push_back({proposed->revision, event.metadata.event_id,
                           std::nullopt, std::nullopt});
  } else if (const auto *recorded =
                 std::get_if<PlanRevisionDecisionRecorded>(&event.payload)) {
    if (auto valid = validate_plan_decision(recorded->decision, limits);
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    auto *current = m_revisions.empty() ? nullptr : &m_revisions.back();
    if (current == nullptr) {
      return failure(PlanGraphErrorCode::invalid_transition,
                     "plan decision requires a proposed revision",
                     recorded->decision.plan_id,
                     recorded->decision.revision_id);
    }
    if (recorded->decision.revision_id != current->revision.revision_id) {
      return failure(PlanGraphErrorCode::wrong_revision,
                     "plan decision does not target the current revision",
                     recorded->decision.plan_id,
                     recorded->decision.revision_id);
    }
    if (current->decision) {
      return failure(PlanGraphErrorCode::invalid_transition,
                     "plan revision already has a decision",
                     recorded->decision.plan_id,
                     recorded->decision.revision_id);
    }
    current->decision_event_id = event.metadata.event_id;
    current->decision = recorded->decision;
  }

  m_event_ids.insert(event.metadata.event_id);
  m_last_sequence = event.metadata.sequence;
  return {};
}

} // namespace aiforge::domain
