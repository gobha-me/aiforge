#include <aiforge/surfaces/agent.hpp>

#include <nlohmann/json.hpp>
#include <type_traits>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Json = nlohmann::json;
struct RecordLimit {};

class Record final {
 public:
  auto text(const std::string_view value) -> Json {
    if (value.size() > m_remaining / 6) throw RecordLimit{};
    m_remaining -= value.size() * 6;
    return std::string{value};
  }
  auto count(const std::size_t size) -> void {
    if (size > 256) throw RecordLimit{};
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
  auto scopes(const std::vector<domain::CapabilityScope>& scopes) -> Json {
    count(scopes.size());
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

template <class Payload>
auto payload_json(const Payload& value, Record& record) -> Json {
  using namespace domain;
  if constexpr (std::is_same_v<Payload, RunStarted>) {
    return {{"kind", "run_started"},
            {"surface_id", record.text(value.surface_id.value())},
            {"workspace_id", record.text(value.workspace_id.value())},
            {"permission_profile_id",
             record.text(value.permission_profile_id.value())}};
  } else if constexpr (std::is_same_v<Payload, UserContentAdded>) {
    return {{"kind", "user_content"},
            {"message_id", record.text(value.message.message_id.value())},
            {"content", record.contents(value.message.content)}};
  } else if constexpr (std::is_same_v<Payload, AssistantContentDeltaAdded>) {
    return {{"kind", "assistant_content"},
            {"message_id", record.text(value.message_id.value())},
            {"inference_id", record.text(value.inference_id.value())},
            {"content", record.content(value.delta)}};
  } else if constexpr (std::is_same_v<Payload, InferenceStarted>) {
    return {{"kind", "inference_started"},
            {"inference_id", record.text(value.inference_id.value())},
            {"model_id", record.text(value.model_id.value())}};
  } else if constexpr (std::is_same_v<Payload, InferenceFinished>) {
    return {{"kind", "inference_finished"},
            {"inference_id", record.text(value.inference_id.value())}};
  } else if constexpr (std::is_same_v<Payload, ToolProposed>) {
    return {{"kind", "tool_proposed"},
            {"invocation_id", record.text(value.invocation_id.value())},
            {"name", record.text(value.tool_name)},
            {"arguments", record.text(value.arguments.data)},
            {"requested_scopes", record.scopes(value.requested_scopes)}};
  } else if constexpr (std::is_same_v<Payload, ToolPolicyDecided>) {
    auto result =
        Json{{"kind", "tool_policy"},
             {"invocation_id", record.text(value.invocation_id.value())},
             {"decision", value.decision == PolicyDecision::allow ? "allow"
                          : value.decision == PolicyDecision::deny
                              ? "deny"
                              : "require_approval"},
             {"scopes", record.scopes(value.scopes)}};
    if (value.automatic_approval) {
      result["matcher_policy_id"] =
          record.text(value.automatic_approval->policy_identity);
      result["rule_id"] = record.text(value.automatic_approval->rule_identity);
    }
    return result;
  } else if constexpr (std::is_same_v<Payload, ToolStarted>) {
    return {{"kind", "tool_started"},
            {"invocation_id", record.text(value.invocation_id.value())}};
  } else if constexpr (std::is_same_v<Payload, ToolProgressed> ||
                       std::is_same_v<Payload, ToolResultRecorded>) {
    return {{"kind", std::is_same_v<Payload, ToolProgressed> ? "tool_progress"
                                                             : "tool_result"},
            {"invocation_id", record.text(value.invocation_id.value())},
            {"content", record.contents(value.content)}};
  } else if constexpr (std::is_same_v<Payload, ToolErrored> ||
                       std::is_same_v<Payload, ToolPolicyFailed>) {
    return {{"kind", "tool_error"},
            {"invocation_id", record.text(value.invocation_id.value())},
            {"message", record.text(value.error.message)}};
  } else if constexpr (std::is_same_v<Payload, ToolApprovalRequested> ||
                       std::is_same_v<Payload, RunAwaitingInput>) {
    return {{"kind", "interaction_required"}};
  } else if constexpr (std::is_same_v<Payload, UsageRecorded>) {
    return {{"kind", "usage"},
            {"inference_id", record.text(value.inference_id.value())},
            {"input_tokens", value.usage.input_tokens},
            {"output_tokens", value.usage.output_tokens},
            {"cached_input_tokens", value.usage.cached_input_tokens},
            {"reasoning_tokens", value.usage.reasoning_tokens}};
  } else if constexpr (std::is_same_v<Payload, InferenceCostRecorded>) {
    record.count(value.cost.amounts().size());
    auto amounts = Json::array();
    for (const auto& amount : value.cost.amounts())
      amounts.push_back({{"unit", record.text(amount.unit())},
                         {"amount", record.text(amount.amount().to_string())}});
    return {{"kind", "cost"},
            {"inference_id", record.text(value.inference_id.value())},
            {"amounts", std::move(amounts)}};
  } else if constexpr (std::is_same_v<Payload, ArtifactCreated>) {
    return {{"kind", "artifact"},
            {"artifact_id", record.text(value.artifact.artifact_id.value())},
            {"media_type", record.text(value.artifact.media_type)},
            {"bytes", value.artifact.byte_size},
            {"digest", record.text(value.artifact.digest)}};
  } else if constexpr (std::is_same_v<Payload, RunCompleted>) {
    return {{"kind", "run_completed"}};
  } else if constexpr (std::is_same_v<Payload, RunCancelled>) {
    return {{"kind", "run_cancelled"}};
  } else if constexpr (std::is_same_v<Payload, RunFailed> ||
                       std::is_same_v<Payload, InferenceFailed>) {
    return {{"kind", "run_error"},
            {"message", record.text(value.error.message)}};
  } else if constexpr (std::is_same_v<Payload, RunProvenanceRecorded>) {
    record.count(value.provenance.tools.size());
    auto tools = Json::array();
    for (const auto& tool : value.provenance.tools) {
      tools.push_back(
          {{"name", record.text(tool.tool_name)},
           {"scopes", record.scopes(tool.capability_scopes)},
           {"registration_digest", tool.registration_digest
                                       ? record.text(*tool.registration_digest)
                                       : Json(nullptr)}});
    }
    return {{"kind", "provenance"},
            {"backend", record.text(value.provenance.backend_id)},
            {"model_id", record.text(value.provenance.model_id.value())},
            {"tools", std::move(tools)}};
  }
  return {{"kind", "state"}};
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
    return encoded({{"version", 1},
                    {"type", "event"},
                    {"session_id", record.text(session.value())},
                    {"run_id", record.text(event.metadata.run_id.value())},
                    {"event_id", record.text(event.metadata.event_id.value())},
                    {"sequence", event.metadata.sequence},
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
