#include <aiforge/surfaces/agent.hpp>

#include <nlohmann/json.hpp>
#include <type_traits>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Json = nlohmann::json;
struct RecordLimit {};

auto restriction_name(const domain::ToolRestrictionLevel value)
    -> std::string_view {
  switch (value) {
    case domain::ToolRestrictionLevel::high: return "high";
    case domain::ToolRestrictionLevel::medium: return "medium";
    case domain::ToolRestrictionLevel::low: return "low";
    case domain::ToolRestrictionLevel::none: return "none";
  }
  throw RecordLimit{};
}

auto approval_name(const domain::ToolApprovalMode value) -> std::string_view {
  switch (value) {
    case domain::ToolApprovalMode::prompt: return "prompt";
    case domain::ToolApprovalMode::automatic: return "automatic";
    case domain::ToolApprovalMode::allow_all: return "allow_all";
  }
  throw RecordLimit{};
}

auto unavailable_name(const domain::ToolRestrictionUnavailableReason value)
    -> std::string_view {
  using Reason = domain::ToolRestrictionUnavailableReason;
  switch (value) {
    case Reason::unsupported_platform: return "unsupported_platform";
    case Reason::unsupported_architecture: return "unsupported_architecture";
    case Reason::unsupported_kernel: return "unsupported_kernel";
    case Reason::missing_delegation: return "missing_delegation";
    case Reason::missing_controller: return "missing_controller";
    case Reason::permission_denied: return "permission_denied";
    case Reason::privilege_changed: return "privilege_changed";
    case Reason::mechanism_absent: return "mechanism_absent";
    case Reason::unsupported_combination: return "unsupported_combination";
    case Reason::setup_race: return "setup_race";
    case Reason::enforcement_failed: return "enforcement_failed";
    case Reason::cleanup_failed: return "cleanup_failed";
    case Reason::internal_error: return "internal_error";
  }
  throw RecordLimit{};
}

auto finish_name(const domain::FinishReason value) -> std::string_view {
  switch (value) {
    case domain::FinishReason::stop: return "stop";
    case domain::FinishReason::length: return "length";
    case domain::FinishReason::tool_call: return "tool_call";
    case domain::FinishReason::content_filter: return "content_filter";
    case domain::FinishReason::other: return "other";
  }
  throw RecordLimit{};
}

auto domain_error_name(const domain::ErrorCode value) -> std::string_view {
  switch (value) {
    case domain::ErrorCode::invalid_event: return "invalid_event";
    case domain::ErrorCode::invalid_state: return "invalid_state";
    case domain::ErrorCode::backend: return "backend";
    case domain::ErrorCode::policy: return "policy";
    case domain::ErrorCode::cancelled: return "cancelled";
    case domain::ErrorCode::unavailable: return "unavailable";
  }
  throw RecordLimit{};
}

auto policy_source_name(const domain::PolicyDecisionSource value)
    -> std::string_view {
  switch (value) {
    case domain::PolicyDecisionSource::fallback: return "fallback";
    case domain::PolicyDecisionSource::permission_profile:
      return "permission_profile";
    case domain::PolicyDecisionSource::automatic_matcher:
      return "automatic_matcher";
    case domain::PolicyDecisionSource::session_grant: return "session_grant";
    case domain::PolicyDecisionSource::saved_grant: return "saved_grant";
    case domain::PolicyDecisionSource::user_approval: return "user_approval";
  }
  throw RecordLimit{};
}

class Record final {
 public:
  auto text(const std::string_view value) -> Json {
    if (value.size() > m_remaining / 6) throw RecordLimit{};
    m_remaining -= value.size() * 6;
    return std::string{value};
  }
  auto count(const std::size_t size, const std::size_t maximum = 256) -> void {
    // Charge object keys, punctuation, and JSON node overhead before building
    // each collection. Text-only accounting misses nested short/empty values.
    constexpr std::size_t entry_budget = 512;
    if (size > maximum || size > m_remaining / entry_budget)
      throw RecordLimit{};
    m_remaining -= size * entry_budget;
  }
  template <class Id> auto optional_id(const std::optional<Id>& value) -> Json {
    return value ? text(value->value()) : Json(nullptr);
  }
  auto optional_text(const std::optional<std::string>& value) -> Json {
    return value ? text(*value) : Json(nullptr);
  }
  auto effects(const std::vector<domain::Effect>& values) -> Json {
    count(values.size());
    auto result = Json::array();
    for (const auto value : values)
      result.push_back(effect_name(value));
    return result;
  }
  auto strings(const std::vector<std::string>& values) -> Json {
    count(values.size());
    auto result = Json::array();
    for (const auto& value : values)
      result.push_back(text(value));
    return result;
  }
  auto error(const std::string_view kind, const domain::DomainError& value)
      -> Json {
    return {{"kind", text(kind)},
            {"code", domain_error_name(value.code)},
            {"retryable", value.retryable},
            {"message", text(value.message)}};
  }
  auto profile(const domain::ToolProfileProvenance& value) -> Json {
    return {{"selected_profile_id", text(value.selected_profile_id.value())},
            {"model_maximum_profile_id",
             optional_id(value.model_maximum_profile_id)},
            {"persona_maximum_profile_id",
             optional_id(value.persona_maximum_profile_id)},
            {"desired_tool_names", value.desired_tool_names
                                       ? strings(*value.desired_tool_names)
                                       : Json(nullptr)}};
  }
  auto policy(const domain::ToolPolicyProvenance& value) -> Json {
    return {
        {"identity", text(value.identity)},
        {"permission_profile_id", text(value.permission_profile_id.value())},
        {"restriction_level", restriction_name(value.restriction_level)},
        {"achieved_restriction_level",
         value.achieved_restriction_level
             ? Json(restriction_name(*value.achieved_restriction_level))
             : Json(nullptr)},
        {"restriction_unavailable_reason",
         value.restriction_unavailable_reason
             ? Json(unavailable_name(*value.restriction_unavailable_reason))
             : Json(nullptr)},
        {"approval_mode", approval_name(value.approval_mode)},
        {"effect_ceiling", effects(value.effect_ceiling)},
        {"capability_ceiling", scopes(value.capability_ceiling, 1024)},
        {"automatically_eligible_tools",
         strings(value.automatically_eligible_tools)},
        {"mechanism_identity", text(value.mechanism_identity)},
        {"mechanism_version", text(value.mechanism_version)},
        {"restriction_policy_identity",
         optional_text(value.restriction_policy_identity)},
        {"matcher_policy_identity",
         optional_text(value.matcher_policy_identity)}};
  }
  auto content(const domain::ContentBlock& block) -> Json {
    if (const auto* value = std::get_if<domain::TextBlock>(&block))
      return {{"kind", "text"}, {"text", text(value->text)}};
    if (const auto* value = std::get_if<domain::StructuredDataBlock>(&block))
      return {{"kind", "data"},
              {"media_type", text(value->media_type)},
              {"data", text(value->data)}};
    if (const auto* value = std::get_if<domain::ArtifactReferenceBlock>(&block))
      return {{"kind", "artifact"},
              {"artifact_id", text(value->artifact_id.value())},
              {"label", value->label ? text(*value->label) : Json(nullptr)}};
    if (const auto* value = std::get_if<domain::CitationBlock>(&block))
      return {{"kind", "citation"},
              {"uri", text(value->uri)},
              {"title", value->title ? text(*value->title) : Json(nullptr)}};
    return {{"kind", "unsupported"}};
  }
  auto contents(const std::vector<domain::ContentBlock>& blocks) -> Json {
    count(blocks.size());
    auto result = Json::array();
    for (const auto& block : blocks)
      result.push_back(content(block));
    return result;
  }
  auto scopes(const std::vector<domain::CapabilityScope>& scopes,
              const std::size_t maximum = 256) -> Json {
    count(scopes.size(), maximum);
    auto result = Json::array();
    for (const auto& scope : scopes)
      result.push_back({{"effect", effect_name(scope.effect)},
                        {"kind", text(scope.kind)},
                        {"value", text(scope.value)}});
    return result;
  }
  static auto effect_name(const domain::Effect effect) -> std::string_view {
    switch (effect) {
      case domain::Effect::read: return "read";
      case domain::Effect::write: return "write";
      case domain::Effect::remove: return "remove";
      case domain::Effect::execute: return "execute";
      case domain::Effect::network: return "network";
      case domain::Effect::communicate: return "communicate";
      case domain::Effect::spend: return "spend";
      case domain::Effect::change_infrastructure:
        return "change_infrastructure";
      case domain::Effect::change_privileges: return "change_privileges";
    }
    return "unknown";
  }

 private:
  std::size_t m_remaining{agent_maximum_record_bytes - 65536};
};

auto encoded(Json value) -> std::expected<std::string, AgentError> {
  auto result = value.dump();
  if (result.size() >= agent_maximum_record_bytes)
    return std::unexpected(AgentError{AgentErrorCode::resource_exhausted,
                                      "agent output record exceeds its bound"});
  result.push_back('\n');
  return result;
}

auto status_name(const AgentStatus status) -> std::string_view {
  switch (status) {
    case AgentStatus::completed: return "completed";
    case AgentStatus::failed: return "failed";
    case AgentStatus::cancelled: return "cancelled";
    case AgentStatus::interaction_required: return "interaction_required";
    case AgentStatus::recovery_required: return "recovery_required";
  }
  return "failed";
}

auto error_name(const AgentErrorCode code) -> std::string_view {
  switch (code) {
    case AgentErrorCode::invalid_request: return "invalid_request";
    case AgentErrorCode::unavailable: return "unavailable";
    case AgentErrorCode::resource_exhausted: return "resource_exhausted";
    case AgentErrorCode::cancelled: return "cancelled";
    case AgentErrorCode::output_failed: return "output_failed";
    case AgentErrorCode::run_failed: return "run_failed";
    case AgentErrorCode::internal_failure: return "internal_failure";
    case AgentErrorCode::cleanup_incomplete: return "cleanup_incomplete";
  }
  return "internal_failure";
}

auto record_failure() -> std::unexpected<AgentError> {
  return std::unexpected(
      AgentError{AgentErrorCode::resource_exhausted,
                 "agent output record could not be encoded within its bounds"});
}

template <class Payload> auto payload_json(const Payload&, Record&) -> Json {
  return {{"kind", "state"}};
}

auto payload_json(const domain::RunStarted& value, Record& record) -> Json {
  return {{"kind", "run_started"},
          {"surface_id", record.text(value.surface_id.value())},
          {"workspace_id", record.text(value.workspace_id.value())},
          {"permission_profile_id",
           record.text(value.permission_profile_id.value())}};
}

auto payload_json(const domain::UserContentAdded& value, Record& record)
    -> Json {
  return {{"kind", "user_content"},
          {"message_id", record.text(value.message.message_id.value())},
          {"content", record.contents(value.message.content)}};
}

auto payload_json(const domain::AssistantContentDeltaAdded& value,
                  Record& record) -> Json {
  return {{"kind", "assistant_content"},
          {"message_id", record.text(value.message_id.value())},
          {"inference_id", record.text(value.inference_id.value())},
          {"content", record.content(value.delta)}};
}

auto payload_json(const domain::InferenceStarted& value, Record& record)
    -> Json {
  return {{"kind", "inference_started"},
          {"inference_id", record.text(value.inference_id.value())},
          {"model_id", record.text(value.model_id.value())}};
}

auto payload_json(const domain::InferenceFinished& value, Record& record)
    -> Json {
  return {{"kind", "inference_finished"},
          {"inference_id", record.text(value.inference_id.value())},
          {"finish_reason", finish_name(value.reason)}};
}

auto payload_json(const domain::InferenceCancelled& value, Record& record)
    -> Json {
  return {{"kind", "inference_cancelled"},
          {"inference_id", record.text(value.inference_id.value())},
          {"reason", record.optional_text(value.reason)}};
}

auto payload_json(const domain::ToolProposed& value, Record& record) -> Json {
  return {{"kind", "tool_proposed"},
          {"invocation_id", record.text(value.invocation_id.value())},
          {"name", record.text(value.tool_name)},
          {"arguments", record.text(value.arguments.data)},
          {"declared_effects", record.effects(value.declared_effects)},
          {"requested_scopes", record.scopes(value.requested_scopes)}};
}

auto payload_json(const domain::ToolPolicyDecided& value, Record& record)
    -> Json {
  auto result = Json{
      {"kind", "tool_policy"},
      {"invocation_id", record.text(value.invocation_id.value())},
      {"decision", value.decision == domain::PolicyDecision::allow ? "allow"
                   : value.decision == domain::PolicyDecision::deny
                       ? "deny"
                       : "require_approval"},
      {"scopes", record.scopes(value.scopes)}};
  result["source"] = policy_source_name(value.source);
  if (value.automatic_approval) {
    result["matcher_policy_id"] =
        record.text(value.automatic_approval->policy_identity);
    result["rule_id"] = record.text(value.automatic_approval->rule_identity);
  }
  return result;
}

auto payload_json(const domain::ToolStarted& value, Record& record) -> Json {
  return {{"kind", "tool_started"},
          {"invocation_id", record.text(value.invocation_id.value())}};
}

template <class Payload>
  requires(std::is_same_v<Payload, domain::ToolProgressed> ||
           std::is_same_v<Payload, domain::ToolResultRecorded>)
auto payload_json(const Payload& value, Record& record) -> Json {
  auto result = Json{
      {"kind", std::is_same_v<Payload, domain::ToolProgressed> ? "tool_progress"
                                                               : "tool_result"},
      {"invocation_id", record.text(value.invocation_id.value())},
      {"content", record.contents(value.content)}};
  if constexpr (std::is_same_v<Payload, domain::ToolResultRecorded>)
    result["result_message_id"] = record.optional_id(value.result_message_id);
  return result;
}

template <class Payload>
  requires(std::is_same_v<Payload, domain::ToolErrored> ||
           std::is_same_v<Payload, domain::ToolPolicyFailed>)
auto payload_json(const Payload& value, Record& record) -> Json {
  auto result = record.error(std::is_same_v<Payload, domain::ToolErrored>
                                 ? "tool_error"
                                 : "tool_policy_error",
                             value.error);
  result["invocation_id"] = record.text(value.invocation_id.value());
  if constexpr (std::is_same_v<Payload, domain::ToolErrored>)
    result["result_message_id"] = record.optional_id(value.result_message_id);
  return result;
}

auto payload_json(const domain::ToolApprovalRequested& value, Record& record)
    -> Json {
  return {{"kind", "interaction_required"},
          {"interaction", "tool_approval"},
          {"invocation_id", record.text(value.invocation_id.value())},
          {"requested_scopes", record.scopes(value.requested_scopes)}};
}

auto payload_json(const domain::RunAwaitingInput& value, Record& record)
    -> Json {
  return {{"kind", "interaction_required"},
          {"interaction", "question"},
          {"question_id", record.text(value.question_id.value())}};
}

auto payload_json(const domain::UsageRecorded& value, Record& record) -> Json {
  return {{"kind", "usage"},
          {"inference_id", record.text(value.inference_id.value())},
          {"input_tokens", value.usage.input_tokens},
          {"output_tokens", value.usage.output_tokens},
          {"cached_input_tokens", value.usage.cached_input_tokens},
          {"reasoning_tokens", value.usage.reasoning_tokens}};
}

auto payload_json(const domain::InferenceCostRecorded& value, Record& record)
    -> Json {
  record.count(value.cost.amounts().size());
  auto amounts = Json::array();
  for (const auto& amount : value.cost.amounts())
    amounts.push_back({{"unit", record.text(amount.unit())},
                       {"amount", record.text(amount.amount().to_string())}});
  return {{"kind", "cost"},
          {"inference_id", record.text(value.inference_id.value())},
          {"amounts", std::move(amounts)}};
}

auto payload_json(const domain::ArtifactCreated& value, Record& record)
    -> Json {
  return {{"kind", "artifact"},
          {"artifact_id", record.text(value.artifact.artifact_id.value())},
          {"media_type", record.text(value.artifact.media_type)},
          {"bytes", value.artifact.byte_size},
          {"digest", record.text(value.artifact.digest)},
          {"producing_invocation_id",
           record.optional_id(value.artifact.producing_invocation_id)},
          {"producing_inference_id",
           record.optional_id(value.artifact.producing_inference_id)},
          {"width",
           value.artifact.width ? Json(*value.artifact.width) : Json(nullptr)},
          {"height", value.artifact.height ? Json(*value.artifact.height)
                                           : Json(nullptr)}};
}

auto payload_json(const domain::RunCompleted&, Record&) -> Json {
  return {{"kind", "run_completed"}};
}

auto payload_json(const domain::RunCancelled&, Record&) -> Json {
  return {{"kind", "run_cancelled"}};
}

template <class Payload>
  requires(std::is_same_v<Payload, domain::RunFailed> ||
           std::is_same_v<Payload, domain::InferenceFailed>)
auto payload_json(const Payload& value, Record& record) -> Json {
  auto result = record.error(std::is_same_v<Payload, domain::RunFailed>
                                 ? "run_error"
                                 : "inference_error",
                             value.error);
  if constexpr (std::is_same_v<Payload, domain::InferenceFailed>)
    result["inference_id"] = record.text(value.inference_id.value());
  return result;
}

auto payload_json(const domain::RunProvenanceRecorded& value, Record& record)
    -> Json {
  record.count(value.provenance.tools.size());
  auto tools = Json::array();
  for (const auto& tool : value.provenance.tools) {
    tools.push_back(
        {{"name", record.text(tool.tool_name)},
         {"declared_effects", record.effects(tool.declared_effects)},
         {"scopes", record.scopes(tool.capability_scopes)},
         {"registration_digest", tool.registration_digest
                                     ? record.text(*tool.registration_digest)
                                     : Json(nullptr)}});
  }
  return {{"kind", "provenance"},
          {"backend", record.text(value.provenance.backend_id)},
          {"model_id", record.text(value.provenance.model_id.value())},
          {"tool_profile", value.provenance.tool_profile
                               ? record.profile(*value.provenance.tool_profile)
                               : Json(nullptr)},
          {"tool_policy", value.provenance.tool_policy
                              ? record.policy(*value.provenance.tool_policy)
                              : Json(nullptr)},
          {"tools", std::move(tools)}};
}

} // namespace

auto agent_accepted_record(const domain::SessionId& session,
                           const domain::RunId& run,
                           const std::vector<backend::ToolDeclaration>& tools)
    -> std::expected<std::string, AgentError> {
  try {
    Record record;
    record.count(tools.size());
    auto declarations = Json::array();
    for (const auto& tool : tools) {
      declarations.push_back(
          {{"name", record.text(tool.name)},
           {"declared_effects", record.effects(tool.effects)},
           {"input_schema", record.text(tool.input_schema.data)},
           {"scopes", record.scopes(tool.capability_scopes)}});
    }
    return encoded({{"version", 1},
                    {"type", "accepted"},
                    {"session_id", record.text(session.value())},
                    {"run_id", record.text(run.value())},
                    {"tools", std::move(declarations)}});
  } catch (...) {
    return record_failure();
  }
}

auto agent_event_record(const domain::SessionId& session,
                        const domain::RunEvent& event)
    -> std::expected<std::string, AgentError> {
  try {
    Record record;
    auto payload = std::visit(
        [&](const auto& value) { return payload_json(value, record); },
        event.payload);
    return encoded(
        {{"version", 1},
         {"type", "event"},
         {"session_id", record.text(session.value())},
         {"run_id", record.text(event.metadata.run_id.value())},
         {"event_id", record.text(event.metadata.event_id.value())},
         {"sequence", event.metadata.sequence},
         {"invocation_id", record.optional_id(event.metadata.invocation_id)},
         {"payload", std::move(payload)}});
  } catch (...) {
    return record_failure();
  }
}

auto agent_error_record(const AgentError& error)
    -> std::expected<std::string, AgentError> {
  try {
    Record record;
    return encoded({{"version", 1},
                    {"type", "error"},
                    {"code", error_name(error.code)},
                    {"message", record.text(error.message)}});
  } catch (...) {
    return record_failure();
  }
}

auto agent_terminal_record(const AgentOutcome outcome)
    -> std::expected<std::string, AgentError> {
  try {
    Record record;
    Json result{{"version", 1},
                {"type", "terminal"},
                {"status", status_name(outcome.status)},
                {"durable_terminal", outcome.durable_terminal},
                {"reason", record.text(outcome.reason)}};
    if (outcome.session_id)
      result["session_id"] = record.text(outcome.session_id->value());
    if (outcome.run_id) result["run_id"] = record.text(outcome.run_id->value());
    return encoded(std::move(result));
  } catch (...) {
    return record_failure();
  }
}
} // namespace aiforge::surfaces
