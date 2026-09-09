#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <string_view>

namespace aiforge::runtime {
namespace {
using namespace domain;
class InvalidHistory final : public std::exception {};
class HistoryLimit final : public std::exception {};
auto require(bool condition) -> void {
  if (!condition) throw InvalidHistory{};
}
auto terminal(OpsInvocationPhase phase) -> bool {
  return phase == OpsInvocationPhase::succeeded ||
         phase == OpsInvocationPhase::failed;
}
auto observe_effect(Effect effect) -> bool {
  return effect == Effect::read || effect == Effect::execute ||
         effect == Effect::network;
}
auto bounded_text(std::string_view value, std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         detail::is_safe_utf8_text(value);
}
auto valid_scopes(const std::vector<CapabilityScope>& scopes) -> bool {
  if (scopes.empty() || scopes.size() > 64) return false;
  for (auto it = scopes.begin(); it != scopes.end(); ++it) {
    if (!observe_effect(it->effect) || !bounded_text(it->kind, 256) ||
        !bounded_text(it->value, 4096) ||
        std::find(scopes.begin(), it, *it) != it)
      return false;
  }
  return true;
}
auto covers(const std::vector<CapabilityScope>& ceiling,
            const std::vector<CapabilityScope>& requested) -> bool {
  return std::ranges::all_of(requested, [&](const auto& scope) {
    return std::ranges::any_of(ceiling, [&](const auto& grant) {
      return capability_scope_covers(grant, scope);
    });
  });
}
auto valid_effects(const std::vector<Effect>& effects) -> bool {
  if (effects.empty() || effects.size() > 3 ||
      !std::ranges::contains(effects, Effect::read))
    return false;
  for (auto it = effects.begin(); it != effects.end(); ++it)
    if (!observe_effect(*it) || std::find(effects.begin(), it, *it) != it)
      return false;
  return true;
}
auto policy_bounded(const ToolPolicyProvenance& policy) -> bool {
  if (policy.effect_ceiling.size() > 16 ||
      policy.capability_ceiling.size() > 1024 ||
      policy.automatically_eligible_tools.size() > 256 ||
      policy.identity.size() > 256 || policy.mechanism_identity.size() > 256 ||
      policy.mechanism_version.size() > 256 ||
      (policy.restriction_policy_identity &&
       policy.restriction_policy_identity->size() > 4096) ||
      (policy.matcher_policy_identity &&
       policy.matcher_policy_identity->size() > 4096))
    return false;
  std::size_t bytes{};
  for (const auto& scope : policy.capability_ceiling) {
    if (scope.kind.size() > 256 || scope.value.size() > 4096) return false;
    bytes += scope.kind.size() + scope.value.size();
  }
  for (const auto& name : policy.automatically_eligible_tools) {
    if (name.size() > 256) return false;
    bytes += name.size();
  }
  return bytes <= 65536;
}
struct RunState {
  const RunStarted* start{};
  const HumanObservationRequested* intent{};
  const RunProvenance* provenance{};
  std::optional<std::size_t> manual_index;
  bool closed{};
  bool next_marker{};
  bool next_proposal{};
  bool cancel_requested{};
};
struct InvocationState {
  std::size_t index;
  const ToolProposed* proposal;
  const OpsObservationRecorded* observation{};
  std::optional<PolicyDecision> decision{};
  bool policy_failed{};
  bool pending_result{};
  bool must_error{};
};
class Validator {
 public:
  explicit Validator(const SessionId& session) : m_session(session) {}
  auto apply(const RunEvent& event) -> void {
    require(event.metadata.sequence > m_snapshot.last_sequence &&
            event.metadata.schema_version != 0);
    require(m_events.insert(event.metadata.event_id).second);
    m_snapshot.last_sequence = event.metadata.sequence;
    if (std::holds_alternative<UnknownEvent>(event.payload)) return;
    auto& run = m_runs[event.metadata.run_id];
    if (const auto* started = std::get_if<RunStarted>(&event.payload)) {
      require(run.start == nullptr);
      require((event.metadata.schema_version == 6) ==
              started->manual_observation_required);
      if (started->manual_observation_required) {
        require(started->purpose == RunPurpose::control &&
                !started->conversation_admission &&
                !started->local_context_admission_required &&
                !event.metadata.parent_run_id && !event.metadata.invocation_id);
        run.next_marker = true;
      }
      run.start = started;
      return;
    }
    // Non-Ops event histories keep their existing validator; this boundary
    // requires complete run provenance whenever an Ops event is encountered.
    if (run.next_marker)
      require(std::holds_alternative<HumanObservationRequested>(event.payload));
    if (run.next_proposal)
      require(std::holds_alternative<ToolProposed>(event.payload));
    if (const auto* intent =
            std::get_if<HumanObservationRequested>(&event.payload)) {
      intent_event(event, run, *intent);
      return;
    }
    if (const auto* proposed = std::get_if<ToolProposed>(&event.payload)) {
      proposal_event(event, run, *proposed);
      return;
    }
    if (const auto* provenance =
            std::get_if<RunProvenanceRecorded>(&event.payload)) {
      require(run.intent == nullptr);
      run.provenance = &provenance->provenance;
    }
    guard_manual_event(event, run);
    route_invocation_event(event, run);
    finish_run_event(event, run);
  }

  auto finish() -> OpsHistorySnapshot {
    for (const auto& [id, run] : m_runs) {
      require(!run.next_marker && !run.next_proposal);
      if (run.manual_index && !run.closed) {
        require(!terminal(m_snapshot.invocations[*run.manual_index].phase));
        m_snapshot.unfinished_manual_runs.push_back(id);
      }
    }
    for (const auto& [id, invocation] : m_invocations) {
      static_cast<void>(id);
      require(!invocation.pending_result && !invocation.must_error);
      const auto& record = m_snapshot.invocations[invocation.index];
      require(invocation.decision != PolicyDecision::require_approval ||
              record.phase != OpsInvocationPhase::proposed);
      if (m_runs.at(record.run_id).closed) require(terminal(record.phase));
    }
    return std::move(m_snapshot);
  }

 private:
  auto route_invocation_event(const RunEvent& event, RunState& run) -> void {
    std::visit(
        [&](const auto& value) {
          if constexpr (requires { value.invocation_id; }) {
            const auto found = m_invocations.find(value.invocation_id);
            if (found != m_invocations.end())
              require(event.metadata.invocation_id == value.invocation_id);
          }
        },
        event.payload);
    const auto invocation =
        event.metadata.invocation_id
            ? m_invocations.find(*event.metadata.invocation_id)
            : m_invocations.end();
    if (invocation != m_invocations.end()) require(!run.closed);
    if (const auto* observed =
            std::get_if<OpsObservationRecorded>(&event.payload)) {
      require(invocation != m_invocations.end() &&
              event.metadata.schema_version == 1);
      observation_event(event, invocation->second, *observed);
    } else if (invocation != m_invocations.end()) {
      invocation_event(event, invocation->second);
    } else {
      // A typed Ops result must never disappear through an absent/forged
      // metadata invocation; manual tool lifecycle events require their ID.
      if (run.start != nullptr && run.start->manual_observation_required)
        require(std::holds_alternative<RunCancelRequested>(event.payload) ||
                std::holds_alternative<RunCancelled>(event.payload) ||
                std::holds_alternative<RunFailed>(event.payload) ||
                std::holds_alternative<RunCompleted>(event.payload));
    }
  }
  auto guard_manual_event(const RunEvent& event, RunState& run) -> void {
    if (run.start != nullptr && run.start->manual_observation_required) {
      require(!run.closed && run.manual_index.has_value());
      require(manual_event(event.payload));
      if (run.cancel_requested)
        require(std::holds_alternative<ToolErrored>(event.payload) ||
                std::holds_alternative<RunCancelled>(event.payload) ||
                std::holds_alternative<RunFailed>(event.payload));
      if (std::holds_alternative<RunCancelRequested>(event.payload))
        run.cancel_requested = true;
      require(!event.metadata.parent_run_id);
    }
  }
  auto finish_run_event(const RunEvent& event, RunState& run) -> void {
    if (std::holds_alternative<RunCompleted>(event.payload) ||
        std::holds_alternative<RunFailed>(event.payload) ||
        std::holds_alternative<RunCancelled>(event.payload)) {
      if (run.manual_index) {
        const auto phase = m_snapshot.invocations[*run.manual_index].phase;
        require(terminal(phase));
        require(std::holds_alternative<RunCompleted>(event.payload)
                    ? phase == OpsInvocationPhase::succeeded
                    : phase == OpsInvocationPhase::failed);
      }
      run.closed = true;
    }
  }
  static auto manual_event(const RunEventPayload& payload) -> bool {
    return std::visit(
        [](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          return std::is_same_v<T, ToolPolicyDecided> ||
                 std::is_same_v<T, ToolPolicyFailed> ||
                 std::is_same_v<T, ToolApprovalRequested> ||
                 std::is_same_v<T, ToolApprovalDecided> ||
                 std::is_same_v<T, ToolStarted> ||
                 std::is_same_v<T, ToolErrored> ||
                 std::is_same_v<T, ToolResultRecorded> ||
                 std::is_same_v<T, OpsObservationRecorded> ||
                 std::is_same_v<T, RunCancelRequested> ||
                 std::is_same_v<T, RunCancelled> ||
                 std::is_same_v<T, RunFailed> ||
                 std::is_same_v<T, RunCompleted>;
        },
        payload);
  }
  auto intent_event(const RunEvent& event, RunState& run,
                    const HumanObservationRequested& value) -> void {
    require(run.start != nullptr && run.start->manual_observation_required &&
            run.next_marker && run.intent == nullptr && !run.closed &&
            event.metadata.schema_version == 1 &&
            !event.metadata.parent_run_id &&
            event.metadata.invocation_id == value.invocation_id);
    require(validate_recorded_ops_request(value.request).has_value() &&
            value.request.session_id == m_session);
    require(validate_tool_provenance_entry(value.tool).has_value() &&
            value.tool.registration_digest.has_value());
    require(policy_bounded(value.policy) &&
            validate_tool_policy_provenance(value.policy).has_value());
    require(value.policy.permission_profile_id ==
            run.start->permission_profile_id);
    run.intent = &value;
    run.next_marker = false;
    run.next_proposal = true;
  }
  auto proposal_event(const RunEvent& event, RunState& run,
                      const ToolProposed& value) -> void {
    require(m_proposed.insert(value.invocation_id).second);
    require((event.metadata.schema_version == 3) ==
            value.observation_request.has_value());
    if (!value.observation_request) {
      require(run.start == nullptr || !run.start->manual_observation_required);
      return;
    }
    require(run.start != nullptr && !run.closed &&
            !event.metadata.parent_run_id &&
            event.metadata.invocation_id == value.invocation_id &&
            !value.parent_invocation_id && value.result_message_id &&
            !value.spend_quote && value.validated_arguments &&
            value.validated_arguments->media_type == "application/json" &&
            bounded_text(value.validated_arguments->data, 16384) &&
            value.arguments.media_type == "application/json" &&
            bounded_text(value.arguments.data, 16384));
    require(
        validate_recorded_ops_request(*value.observation_request).has_value() &&
        value.observation_request->session_id == m_session &&
        valid_effects(value.declared_effects) &&
        valid_scopes(value.requested_scopes));
    require(std::ranges::all_of(value.requested_scopes, [&](const auto& scope) {
      return std::ranges::contains(value.declared_effects, scope.effect);
    }));
    require(m_requests.insert(value.observation_request->request_id).second);
    require(value.validated_required_scopes.empty() ||
            (valid_scopes(value.validated_required_scopes) &&
             value.validated_required_scopes == value.requested_scopes));
    const ToolProvenanceEntry* tool{};
    const ToolPolicyProvenance* policy{};
    if (run.start->manual_observation_required) {
      require(run.next_proposal && run.intent != nullptr && !run.manual_index &&
              run.intent->invocation_id == value.invocation_id &&
              run.intent->request == *value.observation_request);
      tool = &run.intent->tool;
      policy = &run.intent->policy;
    } else {
      require(run.start->purpose == RunPurpose::conversation &&
              run.provenance != nullptr && run.provenance->tool_policy);
      require(run.provenance->tools.size() <= 256);
      const auto found =
          std::ranges::find(run.provenance->tools, value.tool_name,
                            &ToolProvenanceEntry::tool_name);
      require(found != run.provenance->tools.end());
      tool = &*found;
      const auto& stored_policy = run.provenance->tool_policy;
      if (!stored_policy.has_value()) throw InvalidHistory{};
      policy = &*stored_policy;
    }
    require(tool->tool_name == value.tool_name &&
            validate_tool_provenance_entry(*tool).has_value() &&
            tool->registration_digest && policy_bounded(*policy) &&
            validate_tool_policy_provenance(*policy).has_value() &&
            policy->permission_profile_id == run.start->permission_profile_id);
    require(std::ranges::all_of(value.declared_effects, [&](Effect effect) {
      return std::ranges::contains(tool->declared_effects, effect) &&
             std::ranges::contains(policy->effect_ceiling, effect);
    }));
    require(covers(tool->capability_scopes, value.requested_scopes) &&
            covers(policy->capability_ceiling, value.requested_scopes));
    if (m_snapshot.invocations.size() >= maximum_ops_history_invocations ||
        m_request_bytes > maximum_ops_history_request_bytes - 4096)
      throw HistoryLimit{};
    // The largest admitted request has 2487 UTF-8 field bytes: five 128-byte
    // IDs, 828 bytes of Kubernetes target identity, and 1019 bytes of exact
    // pod/container identity. The remaining 1609 bytes cover fixed fields.
    // This is neutral retained-request accounting, not sizeof/allocator usage.
    m_request_bytes += 4096;
    const auto index = m_snapshot.invocations.size();
    m_snapshot.invocations.push_back(
        {event.metadata.run_id, value.invocation_id, *value.observation_request,
         run.start->manual_observation_required});
    m_invocations.emplace(value.invocation_id, InvocationState{index, &value});
    if (run.start->manual_observation_required) run.manual_index = index;
    run.next_proposal = false;
  }
  auto observation_event(const RunEvent& event, InvocationState& state,
                         const OpsObservationRecorded& value) -> void {
    auto& record = m_snapshot.invocations[state.index];
    require(record.run_id == event.metadata.run_id &&
            value.invocation_id == record.invocation_id &&
            record.phase == OpsInvocationPhase::running &&
            state.observation == nullptr &&
            validate_recorded_ops_observation(value.observation).has_value() &&
            value.observation.request == record.request);
    state.observation = &value;
    state.pending_result = true;
    record.observation_event_id = event.metadata.event_id;
  }
  auto invocation_event(const RunEvent& event, InvocationState& state) -> void {
    auto& record = m_snapshot.invocations[state.index];
    require(std::holds_alternative<ToolPolicyDecided>(event.payload)
                ? event.metadata.schema_version == 1 ||
                      event.metadata.schema_version == 2
                : event.metadata.schema_version == 1);
    require(record.run_id == event.metadata.run_id && !terminal(record.phase));
    require(!state.pending_result ||
            std::holds_alternative<ToolResultRecorded>(event.payload));
    require(!state.must_error ||
            std::holds_alternative<ToolErrored>(event.payload));
    std::visit(
        [&](const auto& value) {
          if constexpr (requires { value.invocation_id; })
            require(value.invocation_id == record.invocation_id);
          step(event, state, value);
        },
        event.payload);
  }
  template <class T>
  auto step(const RunEvent&, InvocationState&, const T&) -> void {
    throw InvalidHistory{};
  }
  auto step(const RunEvent& event, InvocationState& state,
            const ToolPolicyDecided& value) -> void {
    auto& record = m_snapshot.invocations[state.index];
    static_cast<void>(event);

    require(record.phase == OpsInvocationPhase::proposed && !state.decision &&
            !state.policy_failed);
    require(value.decision == PolicyDecision::allow ||
            value.decision == PolicyDecision::deny ||
            value.decision == PolicyDecision::require_approval);
    if (value.decision == PolicyDecision::deny) {
      require(value.scopes.empty());
      state.must_error = true;
    } else
      require(valid_scopes(value.scopes) &&
              covers(value.scopes, state.proposal->requested_scopes) &&
              covers(state.proposal->requested_scopes, value.scopes));
    state.decision = value.decision;
    if (value.decision == PolicyDecision::allow)
      record.phase = OpsInvocationPhase::allowed;
  }
  auto step(const RunEvent& event, InvocationState& state,
            const ToolPolicyFailed&) -> void {
    auto& record = m_snapshot.invocations[state.index];
    static_cast<void>(event);

    const bool initial =
        record.phase == OpsInvocationPhase::proposed && !state.decision;
    const bool approval =
        record.phase == OpsInvocationPhase::awaiting_approval &&
        state.decision == PolicyDecision::require_approval;
    require((initial || approval) && !state.policy_failed);
    state.policy_failed = true;
    state.must_error = true;
  }
  auto step(const RunEvent& event, InvocationState& state,
            const ToolApprovalRequested& value) -> void {
    auto& record = m_snapshot.invocations[state.index];
    static_cast<void>(event);

    require(record.phase == OpsInvocationPhase::proposed &&
            state.decision == PolicyDecision::require_approval &&
            value.requested_scopes == state.proposal->requested_scopes);
    record.phase = OpsInvocationPhase::awaiting_approval;
  }
  auto step(const RunEvent& event, InvocationState& state,
            const ToolApprovalDecided& value) -> void {
    auto& record = m_snapshot.invocations[state.index];
    static_cast<void>(event);

    require(record.phase == OpsInvocationPhase::awaiting_approval);
    if (value.decision == ApprovalDecision::approved) {
      require(valid_scopes(value.granted_scopes) &&
              covers(value.granted_scopes, state.proposal->requested_scopes) &&
              covers(state.proposal->requested_scopes, value.granted_scopes));
      record.phase = OpsInvocationPhase::allowed;
    } else {
      require((value.decision == ApprovalDecision::denied ||
               value.decision == ApprovalDecision::cancelled) &&
              value.granted_scopes.empty());
      record.phase = OpsInvocationPhase::proposed;
      state.must_error = true;
    }
  }
  auto step(const RunEvent& event, InvocationState& state, const ToolStarted&)
      -> void {
    auto& record = m_snapshot.invocations[state.index];
    static_cast<void>(event);

    require(record.phase == OpsInvocationPhase::allowed &&
            state.observation == nullptr);
    record.phase = OpsInvocationPhase::running;
  }
  auto step(const RunEvent& event, InvocationState& state,
            const ToolResultRecorded& value) -> void {
    auto& record = m_snapshot.invocations[state.index];
    static_cast<void>(event);

    require(record.phase == OpsInvocationPhase::running &&
            state.observation != nullptr && state.pending_result &&
            value.result_message_id == state.proposal->result_message_id);
    const auto content =
        format_ops_observation_content(state.observation->observation);
    require(content.has_value() && value.content == *content);
    state.pending_result = false;
    record.phase = OpsInvocationPhase::succeeded;
    record.result_event_id = event.metadata.event_id;
  }
  auto step(const RunEvent& event, InvocationState& state,
            const ToolErrored& value) -> void {
    auto& record = m_snapshot.invocations[state.index];
    static_cast<void>(event);

    require(state.observation == nullptr &&
            value.result_message_id == state.proposal->result_message_id);
    record.phase = OpsInvocationPhase::failed;
    state.must_error = false;
    record.result_event_id = event.metadata.event_id;
  }

  const SessionId& m_session;
  OpsHistorySnapshot m_snapshot;
  std::size_t m_request_bytes{};
  std::set<EventId> m_events;
  std::set<InvocationId> m_proposed;
  std::set<OpsRequestId> m_requests;
  std::map<RunId, RunState> m_runs;
  std::map<InvocationId, InvocationState> m_invocations;
};
} // namespace

auto recorded_ops_observations(
    const domain::SessionEventLog& log,
    std::span<const domain::RunEvent> prospective_suffix)
    -> std::expected<OpsHistorySnapshot, OpsHistoryError> {
  try {
    if (log.events().size() > maximum_ops_history_events ||
        prospective_suffix.size() >
            maximum_ops_history_events - log.events().size())
      throw HistoryLimit{};
    Validator validator{log.session_id()};
    for (const auto& event : log.events())
      validator.apply(event);
    for (const auto& event : prospective_suffix)
      validator.apply(event);
    return validator.finish();
  } catch (const HistoryLimit&) {
    return std::unexpected(
        OpsHistoryError{OpsHistoryErrorCode::resource_exhausted,
                        "Ops history exceeds its resource budget"});
  } catch (const InvalidHistory&) {
    return std::unexpected(
        OpsHistoryError{OpsHistoryErrorCode::invalid_history,
                        "Ops history is incomplete or inconsistent"});
  } catch (...) {
    return std::unexpected(
        OpsHistoryError{OpsHistoryErrorCode::internal_failure,
                        "Ops history validation failed"});
  }
}
} // namespace aiforge::runtime
