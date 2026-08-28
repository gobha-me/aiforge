#include <aiforge/runtime/memory_controller.hpp>

#include <aiforge/runtime/context_builder.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto failure(const MemoryControllerErrorCode code,
                           std::string message, const bool retryable = false)
    -> std::unexpected<MemoryControllerError> {
  return std::unexpected(
      MemoryControllerError{code, std::move(message), retryable});
}

[[nodiscard]] auto storage_failure(const storage::SessionStoreError& error)
    -> std::unexpected<MemoryControllerError> {
  return failure(error.code == storage::SessionStoreErrorCode::cancelled
                     ? MemoryControllerErrorCode::cancelled
                     : MemoryControllerErrorCode::storage_failure,
                 error.message, error.retryable);
}

template <typename Id>
[[nodiscard]] auto make_id(const std::string_view prefix,
                           const std::uint64_t suffix)
    -> std::expected<Id, MemoryControllerError> {
  auto value = Id::from(std::string{prefix} + '-' + std::to_string(suffix));
  if (!value) {
    return failure(MemoryControllerErrorCode::internal_failure,
                   "memory identity generation failed");
  }
  return std::move(*value);
}

[[nodiscard]] auto capture_mode(const config::ResolvedConfig& config,
                                const std::string_view key)
    -> std::expected<domain::MemoryCaptureMode, MemoryControllerError> {
  const auto* entry = config.find(key);
  if (entry == nullptr || !entry->value) {
    return failure(MemoryControllerErrorCode::invalid_configuration,
                   "memory capture setting is missing");
  }
  const auto* text = std::get_if<std::string>(&*entry->value);
  if (text == nullptr) {
    return failure(MemoryControllerErrorCode::invalid_configuration,
                   "memory capture setting has the wrong type");
  }
  if (*text == "off") return domain::MemoryCaptureMode::off;
  if (*text == "review") return domain::MemoryCaptureMode::review;
  if (*text == "auto") return domain::MemoryCaptureMode::automatic;
  return failure(MemoryControllerErrorCode::invalid_configuration,
                 "memory capture setting must be off, review, or auto");
}

[[nodiscard]] auto event_text_contains(const domain::RunEvent& event,
                                       const std::string_view excerpt) -> bool {
  const auto* added = std::get_if<domain::UserContentAdded>(&event.payload);
  if (added == nullptr) return false;
  return std::ranges::any_of(added->message.content, [&](const auto& block) {
    const auto* text = std::get_if<domain::TextBlock>(&block);
    return text != nullptr && text->text.contains(excerpt);
  });
}

[[nodiscard]] auto to_record(const domain::MemoryProposal& proposal)
    -> domain::MemoryRecord {
  return {proposal.record_id,     proposal.proposal_id, proposal.scope,
          proposal.repository_id, proposal.kind,        proposal.content,
          proposal.rationale,     proposal.source,      proposal.producer};
}

[[nodiscard]] auto mode_for(const MemorySettings& settings,
                            const domain::MemoryScope scope)
    -> domain::MemoryCaptureMode {
  return scope == domain::MemoryScope::global ? settings.global_capture
                                              : settings.project_capture;
}

[[nodiscard]] auto direct_auto_kind(const domain::MemoryScope scope,
                                    const domain::MemoryKind kind) -> bool {
  if (scope == domain::MemoryScope::global) {
    return kind == domain::MemoryKind::user_preference;
  }
  return kind == domain::MemoryKind::user_preference ||
         kind == domain::MemoryKind::project_convention ||
         kind == domain::MemoryKind::workflow;
}

[[nodiscard]] auto scope_name(const domain::MemoryScope scope)
    -> std::string_view {
  return scope == domain::MemoryScope::project ? "project" : "global";
}

[[nodiscard]] auto kind_name(const domain::MemoryKind kind)
    -> std::string_view {
  switch (kind) {
    case domain::MemoryKind::user_preference: return "user preference";
    case domain::MemoryKind::project_convention: return "project convention";
    case domain::MemoryKind::workflow: return "workflow";
    case domain::MemoryKind::reusable_fact: return "reusable fact";
    case domain::MemoryKind::unknown: return "unknown";
  }
  return "unknown";
}

} // namespace

struct MemoryController::Journal {
  storage::SessionInfo info;
  domain::MemoryProjection projection;
};

auto resolve_memory_settings(const config::ResolvedConfig& config)
    -> std::expected<MemorySettings, MemoryControllerError> {
  auto global = capture_mode(config, "memory.global.capture");
  auto project = capture_mode(config, "memory.project.capture");
  const auto* tokens = config.find("memory.context.max_tokens");
  if (!global) return std::unexpected(std::move(global.error()));
  if (!project) return std::unexpected(std::move(project.error()));
  if (tokens == nullptr || !tokens->value) {
    return failure(MemoryControllerErrorCode::invalid_configuration,
                   "memory context budget is missing");
  }
  const auto* value = std::get_if<std::uint64_t>(&*tokens->value);
  if (value == nullptr || *value == 0 || *value > 1024U * 1024U) {
    return failure(MemoryControllerErrorCode::invalid_configuration,
                   "memory context budget must be between 1 and 1048576");
  }
  return MemorySettings{*global, *project, *value};
}

auto select_memory_context(MemoryController& controller,
                           MemoryContextRequest request)
    -> std::expected<std::vector<domain::ContextContentInput>,
                     MemoryControllerError> {
  if (request.maximum_tokens == 0 || request.available_tokens == 0) return {};
  auto records = controller.current_for_context(request.repository_id);
  if (!records) return std::unexpected(std::move(records.error()));

  runtime::ContextSelectionRequest selection;
  selection.phase = domain::TaskPhase::orientation;
  const auto selectable_tokens = std::min(
      request.available_tokens, std::numeric_limits<std::uint64_t>::max() - 2);
  selection.capacity = {selectable_tokens + 2, 1, 0};
  selection.instructions.push_back(
      {*domain::ContextEntryId::from("memory-selection-runtime"),
       domain::InstructionLayer::application_runtime,
       domain::InstructionOperation::add,
       std::nullopt,
       domain::Message{*domain::MessageId::from("memory-selection-message"),
                       domain::Role::system,
                       {domain::TextBlock{"Select saved memory evidence"}},
                       std::nullopt},
       {*domain::ContextSourceId::from("memory-selection-source"),
        "aiforge:memory-selection", std::nullopt},
       0,
       1,
       1});
  selection.budgets.memory_tokens = request.maximum_tokens;
  selection.candidates.reserve(records->size());
  std::uint64_t order{};
  for (const auto& view : *records) {
    const auto& record = view.projected.record;
    auto entry_id = domain::ContextEntryId::from(
        "memory-entry-" + std::string{record.record_id.value()});
    auto message_id = domain::MessageId::from(
        "memory-message-" + std::string{record.record_id.value()});
    auto source_id = domain::ContextSourceId::from(
        "memory-source-" + std::string{record.record_id.value()});
    if (!entry_id || !message_id || !source_id) {
      return failure(MemoryControllerErrorCode::internal_failure,
                     "saved memory identity cannot enter context");
    }
    auto text = "Saved " + std::string{scope_name(record.scope)} + " " +
                std::string{kind_name(record.kind)} + ": " + record.content;
    domain::ContextContentInput content{
        std::move(*entry_id),
        domain::ContextContentKind::evidence,
        {std::move(*message_id),
         domain::Role::evidence,
         {domain::TextBlock{text}},
         std::nullopt},
        {std::move(*source_id),
         "memory:" + std::string{record.record_id.value()} +
             ";session:" + std::string{record.source.session_id.value()},
         std::nullopt},
        ++order,
        text.size()};
    selection.candidates.push_back(
        {std::move(content), runtime::ContextBudgetClass::memory,
         runtime::ContextRepresentation::derived,
         domain::EvidenceFreshness::current, false, order, std::nullopt});
  }
  auto selected =
      runtime::ContextBuilder{}.select_and_build(std::move(selection));
  if (!selected) {
    return failure(MemoryControllerErrorCode::internal_failure,
                   "saved memory context selection failed: " +
                       selected.error().message);
  }
  std::vector<domain::ContextContentInput> result;
  result.reserve(selected->context.entries.size());
  for (auto& entry : selected->context.entries) {
    if (entry.kind == domain::ContextEntryKind::instruction) continue;
    result.push_back({std::move(entry.entry_id),
                      domain::ContextContentKind::evidence,
                      std::move(entry.message), std::move(entry.provenance),
                      entry.order, entry.estimated_tokens});
  }
  return result;
}

MemoryController::MemoryController(
    storage::SessionStore& store,
    MemoryIdentitySuffixSource identity_suffix_source,
    MemoryTimestampSource timestamp_source, const std::stop_token stop_token,
    domain::MemoryLimits limits)
    : m_store(store),
      m_identity_suffix_source(std::move(identity_suffix_source)),
      m_timestamp_source(std::move(timestamp_source)), m_stop_token(stop_token),
      m_limits(limits) {
}

auto MemoryController::open(MemoryMutationTarget target)
    -> std::expected<Journal, MemoryControllerError> {
  const bool valid =
      (target.scope == domain::MemoryScope::global && !target.repository_id) ||
      (target.scope == domain::MemoryScope::project && target.repository_id);
  if (!valid || !m_identity_suffix_source || !m_timestamp_source) {
    return failure(MemoryControllerErrorCode::invalid_configuration,
                   "memory journal target or identity sources are invalid");
  }
  auto candidate = make_id<domain::SessionId>(
      target.scope == domain::MemoryScope::global ? "memory-global"
                                                  : "memory-project",
      m_identity_suffix_source());
  if (!candidate) return std::unexpected(std::move(candidate.error()));
  auto info = m_store.open_or_create_memory_journal(
      {std::move(*candidate), target.scope, target.repository_id,
       m_timestamp_source()},
      m_stop_token);
  if (!info) return storage_failure(info.error());
  auto events = m_store.replay_events(info->session_id, m_stop_token);
  if (!events) return storage_failure(events.error());
  auto projection = domain::MemoryProjection::rebuild(*events, m_limits);
  if (!projection) {
    return failure(MemoryControllerErrorCode::storage_failure,
                   projection.error().message);
  }
  return Journal{std::move(*info), std::move(*projection)};
}

auto MemoryController::append(Journal& journal, const domain::RunId& run_id,
                              std::optional<domain::InvocationId> invocation_id,
                              std::vector<domain::RunEventPayload> payloads)
    -> std::expected<void, MemoryControllerError> {
  std::vector<domain::RunEvent> events;
  events.reserve(payloads.size());
  auto sequence = journal.info.last_sequence;
  for (auto& payload : payloads) {
    auto event_id =
        make_id<domain::EventId>("memory-event", m_identity_suffix_source());
    if (!event_id) return std::unexpected(std::move(event_id.error()));
    events.push_back(
        {{std::move(*event_id), run_id, ++sequence, 1, m_timestamp_source(),
          std::nullopt, std::nullopt, invocation_id},
         std::move(payload)});
  }
  auto appended =
      m_store.append_events(journal.info.session_id, events, m_stop_token);
  if (!appended) return storage_failure(appended.error());
  for (const auto& event : events) {
    auto applied = journal.projection.apply(event, m_limits);
    if (!applied) {
      return failure(MemoryControllerErrorCode::internal_failure,
                     "committed memory event could not be projected");
    }
  }
  journal.info.last_sequence = sequence;
  return {};
}

auto MemoryController::append_capture(Journal& journal,
                                      const domain::RunId& run_id,
                                      const domain::InvocationId& invocation_id,
                                      const domain::MemoryProposal& proposal,
                                      const domain::MemoryPolicyAction action,
                                      std::string reason)
    -> std::expected<void, MemoryControllerError> {
  std::vector<domain::RunEvent> events;
  events.reserve(action == domain::MemoryPolicyAction::stage ? 2U : 3U);
  auto sequence = journal.info.last_sequence;
  const auto add = [&](domain::RunEventPayload payload)
      -> std::expected<domain::EventId, MemoryControllerError> {
    auto event_id =
        make_id<domain::EventId>("memory-event", m_identity_suffix_source());
    if (!event_id) return std::unexpected(std::move(event_id.error()));
    auto result = *event_id;
    events.push_back(
        {{std::move(*event_id), run_id, ++sequence, 1, m_timestamp_source(),
          std::nullopt, std::nullopt, invocation_id},
         std::move(payload)});
    return result;
  };
  auto proposal_event_id = add(domain::MemoryProposed{proposal});
  if (!proposal_event_id) {
    return std::unexpected(std::move(proposal_event_id.error()));
  }
  domain::MemoryPolicyEvaluation evaluation{
      proposal.proposal_id, action, domain::MemoryDecisionSource::policy,
      std::move(reason), *proposal_event_id};
  auto policy_event_id = add(domain::MemoryPolicyDecided{evaluation});
  if (!policy_event_id) {
    return std::unexpected(std::move(policy_event_id.error()));
  }
  if (action == domain::MemoryPolicyAction::accept) {
    auto accepted = add(domain::MemoryAccepted{domain::MemoryAcceptance{
        to_record(proposal), domain::MemoryDecisionSource::policy,
        *proposal_event_id}});
    if (!accepted) return std::unexpected(std::move(accepted.error()));
  } else if (action == domain::MemoryPolicyAction::reject) {
    auto rejected = add(domain::MemoryRejected{domain::MemoryRejection{
        proposal.proposal_id, domain::MemoryDecisionSource::policy,
        evaluation.reason, *proposal_event_id}});
    if (!rejected) return std::unexpected(std::move(rejected.error()));
  }
  auto appended =
      m_store.append_events(journal.info.session_id, events, m_stop_token);
  if (!appended) return storage_failure(appended.error());
  for (const auto& event : events) {
    auto applied = journal.projection.apply(event, m_limits);
    if (!applied) {
      return failure(MemoryControllerErrorCode::internal_failure,
                     "committed memory capture could not be projected");
    }
  }
  journal.info.last_sequence = sequence;
  return {};
}

auto MemoryController::capture_committed(
    const domain::SessionId& source_session_id,
    const std::span<const domain::RunEvent> source_events,
    const MemorySettings& settings,
    std::optional<domain::RepositoryId> repository_id,
    std::string runtime_version)
    -> std::expected<std::size_t, MemoryControllerError> {
  try {
    std::size_t captured{};
    for (std::size_t index{}; index < source_events.size(); ++index) {
      const auto* result = std::get_if<domain::ToolResultRecorded>(
          &source_events[index].payload);
      if (result == nullptr) continue;
      const domain::ToolProposed* proposed{};
      for (std::size_t candidate{}; candidate < index; ++candidate) {
        const auto* value = std::get_if<domain::ToolProposed>(
            &source_events[candidate].payload);
        if (value != nullptr && value->invocation_id == result->invocation_id &&
            value->tool_name == "propose_memory") {
          proposed = value;
        }
      }
      if (proposed == nullptr) continue;

      MemoryToolConfiguration tool_configuration{
          settings.global_capture != domain::MemoryCaptureMode::off,
          settings.project_capture != domain::MemoryCaptureMode::off, m_limits};
      auto draft =
          parse_memory_proposal_draft(proposed->arguments, tool_configuration);
      if (!draft) continue;
      if (mode_for(settings, draft->scope) == domain::MemoryCaptureMode::off ||
          (draft->scope == domain::MemoryScope::project && !repository_id)) {
        continue;
      }
      MemoryMutationTarget target{draft->scope,
                                  draft->scope == domain::MemoryScope::project
                                      ? repository_id
                                      : std::nullopt};
      auto journal = open(target);
      if (!journal) return std::unexpected(std::move(journal.error()));
      if (std::ranges::any_of(
              journal->projection.proposals(), [&](const auto& value) {
                return value.proposal.source.session_id == source_session_id &&
                       value.proposal.source.invocation_id ==
                           result->invocation_id;
              })) {
        continue;
      }
      std::optional<domain::ModelId> model_id;
      std::optional<domain::EventId> evidence_event_id;
      std::vector<domain::EventId> source_ids;
      for (std::size_t source_index{}; source_index <= index; ++source_index) {
        const auto& event = source_events[source_index];
        if (event.metadata.run_id != source_events[index].metadata.run_id)
          continue;
        if (const auto* inference =
                std::get_if<domain::InferenceStarted>(&event.payload)) {
          model_id = inference->model_id;
        }
        if (event_text_contains(event, draft->evidence_excerpt)) {
          evidence_event_id = event.metadata.event_id;
        }
        if (source_ids.size() < m_limits.maximum_source_events) {
          source_ids.push_back(event.metadata.event_id);
        }
      }
      if (!model_id) {
        return failure(MemoryControllerErrorCode::invalid_source,
                       "memory proposal source lacks model provenance");
      }
      if (source_ids.size() == m_limits.maximum_source_events) {
        source_ids.back() = source_events[index].metadata.event_id;
      }
      auto proposal_id = make_id<domain::MemoryProposalId>(
          "memory-proposal", m_identity_suffix_source());
      auto record_id = make_id<domain::MemoryRecordId>(
          "memory-record", m_identity_suffix_source());
      if (!proposal_id) return std::unexpected(std::move(proposal_id.error()));
      if (!record_id) return std::unexpected(std::move(record_id.error()));
      domain::MemoryProposal proposal{
          std::move(*proposal_id),
          std::move(*record_id),
          draft->scope,
          target.repository_id,
          draft->kind,
          std::move(draft->content),
          std::move(draft->rationale),
          std::move(draft->evidence_excerpt),
          {source_session_id, source_events[index].metadata.run_id,
           result->invocation_id, std::move(source_ids)},
          {*model_id, "aiforge", runtime_version},
          std::move(draft->replacement_record_id),
          std::move(draft->overlap_record_ids)};
      for (const auto& value : journal->projection.records()) {
        if (value.state == domain::ProjectedMemoryRecordState::current &&
            value.record.content == proposal.content &&
            !std::ranges::contains(proposal.overlap_record_ids,
                                   value.record.record_id) &&
            proposal.overlap_record_ids.size() <
                m_limits.maximum_relationships) {
          proposal.overlap_record_ids.push_back(value.record.record_id);
        }
      }
      if (auto valid = domain::validate_memory_proposal(proposal, m_limits);
          !valid) {
        continue;
      }
      const auto duplicate = std::ranges::any_of(
          journal->projection.records(), [&](const auto& value) {
            return value.state == domain::ProjectedMemoryRecordState::current &&
                   value.record.content == proposal.content;
          });
      const auto mode = mode_for(settings, proposal.scope);
      domain::MemoryPolicyAction action = domain::MemoryPolicyAction::stage;
      std::string reason{"capture mode requires user review"};
      if (mode == domain::MemoryCaptureMode::automatic) {
        const bool allowed = evidence_event_id &&
                             direct_auto_kind(proposal.scope, proposal.kind) &&
                             proposal.overlap_record_ids.empty() && !duplicate;
        action = allowed ? domain::MemoryPolicyAction::accept
                         : domain::MemoryPolicyAction::reject;
        reason = allowed ? "accepted by conservative direct-evidence policy"
                         : "rejected by conservative auto-capture policy";
      }

      auto appended = append_capture(
          *journal, source_events[index].metadata.run_id, result->invocation_id,
          proposal, action, std::move(reason));
      if (!appended) return std::unexpected(std::move(appended.error()));
      ++captured;
    }
    return captured;
  } catch (...) {
    return failure(MemoryControllerErrorCode::internal_failure,
                   "memory capture failed internally");
  }
}

auto MemoryController::inspect(MemoryMutationTarget target)
    -> std::expected<MemoryState, MemoryControllerError> {
  auto journal = open(target);
  if (!journal) return std::unexpected(std::move(journal.error()));
  MemoryState state{target.scope, target.repository_id, {}, {}};
  std::map<domain::SessionId, std::optional<std::set<domain::EventId>>> sources;
  const auto source_available = [&](const domain::MemorySource& source)
      -> std::expected<bool, MemoryControllerError> {
    auto found = sources.find(source.session_id);
    if (found == sources.end()) {
      auto events = m_store.replay_events(source.session_id, m_stop_token);
      if (!events) {
        if (events.error().code != storage::SessionStoreErrorCode::not_found) {
          return storage_failure(events.error());
        }
        found = sources.emplace(source.session_id, std::nullopt).first;
      } else {
        std::set<domain::EventId> ids;
        for (const auto& event : *events)
          ids.insert(event.metadata.event_id);
        found = sources.emplace(source.session_id, std::move(ids)).first;
      }
    }
    if (!found->second) return false;
    return std::ranges::all_of(source.event_ids, [&](const auto& id) {
      return found->second->contains(id);
    });
  };
  state.proposals.reserve(journal->projection.proposals().size());
  for (const auto& projected : journal->projection.proposals()) {
    auto available = source_available(projected.proposal.source);
    if (!available) return std::unexpected(std::move(available.error()));
    state.proposals.push_back({projected, *available});
  }
  state.records.reserve(journal->projection.records().size());
  for (const auto& projected : journal->projection.records()) {
    auto available = source_available(projected.record.source);
    if (!available) return std::unexpected(std::move(available.error()));
    state.records.push_back({projected, *available});
  }
  return state;
}

auto MemoryController::current_for_context(
    std::optional<domain::RepositoryId> repository_id)
    -> std::expected<std::vector<MemoryRecordView>, MemoryControllerError> {
  std::vector<MemoryRecordView> result;
  if (repository_id) {
    auto project = inspect({domain::MemoryScope::project, repository_id});
    if (!project) return std::unexpected(std::move(project.error()));
    for (auto& record : project->records) {
      if (record.source_available &&
          record.projected.state ==
              domain::ProjectedMemoryRecordState::current &&
          record.projected.record.kind != domain::MemoryKind::unknown)
        result.push_back(std::move(record));
    }
  }
  auto global = inspect({domain::MemoryScope::global, std::nullopt});
  if (!global) return std::unexpected(std::move(global.error()));
  for (auto& record : global->records) {
    if (record.source_available &&
        record.projected.state == domain::ProjectedMemoryRecordState::current &&
        record.projected.record.kind != domain::MemoryKind::unknown)
      result.push_back(std::move(record));
  }
  std::ranges::stable_sort(result, [](const auto& left, const auto& right) {
    if (left.projected.record.scope != right.projected.record.scope) {
      return left.projected.record.scope == domain::MemoryScope::project;
    }
    return left.projected.record.record_id > right.projected.record.record_id;
  });
  return result;
}

auto MemoryController::accept(MemoryAcceptRequest request)
    -> std::expected<void, MemoryControllerError> {
  auto journal = open(request.target);
  if (!journal) return std::unexpected(std::move(journal.error()));
  const auto* proposal = journal->projection.find_proposal(request.proposal_id);
  if (proposal == nullptr ||
      proposal->state != domain::ProjectedMemoryProposalState::pending ||
      proposal->proposal_event_id != request.expected_proposal_event_id) {
    return failure(MemoryControllerErrorCode::stale_state,
                   "memory proposal acceptance is stale");
  }
  auto record = to_record(proposal->proposal);
  if (request.edited_content)
    record.content = std::move(*request.edited_content);
  if (auto valid = domain::validate_memory_record(record, m_limits); !valid) {
    return failure(MemoryControllerErrorCode::invalid_transition,
                   valid.error().message);
  }
  std::vector<domain::RunEventPayload> payloads;
  if (request.replacement_record_id) {
    if (!request.expected_record_event_id ||
        proposal->proposal.overlap_record_ids.size() != 1) {
      return failure(MemoryControllerErrorCode::stale_state,
                     "memory replacement requires exactly one current record");
    }
    payloads.push_back(domain::MemoryEditedAndAccepted{
        {{record, domain::MemoryDecisionSource::user,
          proposal->proposal_event_id},
         *request.replacement_record_id,
         *request.expected_record_event_id}});
  } else {
    if (!proposal->proposal.overlap_record_ids.empty()) {
      return failure(MemoryControllerErrorCode::invalid_transition,
                     "contradictory memory requires explicit replacement");
    }
    payloads.push_back(
        domain::MemoryAccepted{{record, domain::MemoryDecisionSource::user,
                                proposal->proposal_event_id}});
  }
  return append(*journal, proposal->proposal.source.run_id,
                proposal->proposal.source.invocation_id, std::move(payloads));
}

auto MemoryController::reject(MemoryRejectRequest request)
    -> std::expected<void, MemoryControllerError> {
  if (!domain::memory_text_is_safe(request.reason)) {
    return failure(MemoryControllerErrorCode::invalid_transition,
                   "memory rejection reason is invalid");
  }
  auto journal = open(request.target);
  if (!journal) return std::unexpected(std::move(journal.error()));
  const auto* proposal = journal->projection.find_proposal(request.proposal_id);
  if (proposal == nullptr ||
      proposal->state != domain::ProjectedMemoryProposalState::pending ||
      proposal->proposal_event_id != request.expected_proposal_event_id) {
    return failure(MemoryControllerErrorCode::stale_state,
                   "memory rejection is stale");
  }
  return append(
      *journal, proposal->proposal.source.run_id,
      proposal->proposal.source.invocation_id,
      {domain::MemoryRejected{
          {proposal->proposal.proposal_id, domain::MemoryDecisionSource::user,
           std::move(request.reason), proposal->proposal_event_id}}});
}

auto MemoryController::expire(MemoryExpireRequest request)
    -> std::expected<void, MemoryControllerError> {
  if (!domain::memory_text_is_safe(request.reason)) {
    return failure(MemoryControllerErrorCode::invalid_transition,
                   "memory expiry reason is invalid");
  }
  auto journal = open(request.target);
  if (!journal) return std::unexpected(std::move(journal.error()));
  const auto* record = journal->projection.find_record(request.record_id);
  if (record == nullptr ||
      record->state != domain::ProjectedMemoryRecordState::current ||
      record->record_event_id != request.expected_record_event_id) {
    return failure(MemoryControllerErrorCode::stale_state,
                   "memory expiry is stale");
  }
  return append(
      *journal, record->record.source.run_id,
      record->record.source.invocation_id,
      {domain::MemoryExpired{
          {record->record.record_id, domain::MemoryDecisionSource::user,
           std::move(request.reason), record->record_event_id}}});
}

} // namespace aiforge::runtime
