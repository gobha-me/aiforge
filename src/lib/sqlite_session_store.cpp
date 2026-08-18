#include <aiforge/adapters/sqlite_session_store.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace aiforge::adapters {
namespace {

using Json = nlohmann::json;
using storage::SessionStoreError;
using storage::SessionStoreErrorCode;
constexpr int storage_format_version = 1;

template <typename... Visitors>
struct Overloaded : Visitors... {
  using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

class CodecFailure final : public std::exception {
 public:
  explicit CodecFailure(std::string message) : m_message(std::move(message)) {}
  [[nodiscard]] auto what() const noexcept -> const char* override {
    return m_message.c_str();
  }

 private:
  std::string m_message;
};

class DuplicateJsonKey final : public std::exception {
 public:
  [[nodiscard]] auto what() const noexcept -> const char* override {
    return "duplicate JSON key";
  }
};

[[nodiscard]] auto store_error(const SessionStoreErrorCode code,
                               std::string message,
                               const bool retryable = false)
    -> SessionStoreError {
  return {code, std::move(message), retryable};
}

[[nodiscard]] auto cancelled_error() -> SessionStoreError {
  return store_error(SessionStoreErrorCode::cancelled,
                     "session-store operation cancelled");
}

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  for (const unsigned char character : value) {
    if (character < 0x20U || character == 0x7FU) return true;
  }
  return false;
}

template <typename IdType>
[[nodiscard]] auto id_text(const IdType& id) -> std::string {
  return std::string{id.value()};
}

template <typename IdType>
[[nodiscard]] auto parse_id(const Json& value) -> IdType {
  if (!value.is_string()) throw CodecFailure{"event ID field is not text"};
  auto parsed = IdType::from(value.get<std::string>());
  if (!parsed) throw CodecFailure{"event ID field is invalid"};
  return std::move(*parsed);
}

template <typename IdType>
[[nodiscard]] auto optional_id_json(const std::optional<IdType>& id) -> Json {
  return id ? Json(id_text(*id)) : Json(nullptr);
}

template <typename IdType>
[[nodiscard]] auto parse_optional_id(const Json& value)
    -> std::optional<IdType> {
  if (value.is_null()) return std::nullopt;
  return parse_id<IdType>(value);
}

[[nodiscard]] auto optional_string_json(const std::optional<std::string>& value)
    -> Json {
  return value ? Json(*value) : Json(nullptr);
}

[[nodiscard]] auto parse_optional_string(const Json& value)
    -> std::optional<std::string> {
  if (value.is_null()) return std::nullopt;
  if (!value.is_string()) throw CodecFailure{"optional text field is invalid"};
  return value.get<std::string>();
}

template <typename Enum>
[[nodiscard]] auto enum_value(const std::string_view name,
                              const std::initializer_list<
                                  std::pair<std::string_view, Enum>> values)
    -> Enum {
  for (const auto& [candidate, value] : values) {
    if (candidate == name) return value;
  }
  throw CodecFailure{"event enum field is unknown"};
}

[[nodiscard]] auto role_name(const domain::Role value) -> std::string_view {
  switch (value) {
    case domain::Role::system: return "system";
    case domain::Role::user: return "user";
    case domain::Role::assistant: return "assistant";
    case domain::Role::tool: return "tool";
    case domain::Role::evidence: return "evidence";
  }
  throw CodecFailure{"invalid role"};
}

[[nodiscard]] auto parse_role(const Json& value) -> domain::Role {
  const auto name = value.get<std::string>();
  return enum_value<domain::Role>(
      name, {{"system", domain::Role::system}, {"user", domain::Role::user},
             {"assistant", domain::Role::assistant}, {"tool", domain::Role::tool},
             {"evidence", domain::Role::evidence}});
}

[[nodiscard]] auto finish_name(const domain::FinishReason value)
    -> std::string_view {
  switch (value) {
    case domain::FinishReason::stop: return "stop";
    case domain::FinishReason::length: return "length";
    case domain::FinishReason::tool_call: return "tool_call";
    case domain::FinishReason::content_filter: return "content_filter";
    case domain::FinishReason::other: return "other";
  }
  throw CodecFailure{"invalid finish reason"};
}

[[nodiscard]] auto parse_finish(const Json& value) -> domain::FinishReason {
  const auto name = value.get<std::string>();
  return enum_value<domain::FinishReason>(
      name, {{"stop", domain::FinishReason::stop},
             {"length", domain::FinishReason::length},
             {"tool_call", domain::FinishReason::tool_call},
             {"content_filter", domain::FinishReason::content_filter},
             {"other", domain::FinishReason::other}});
}

[[nodiscard]] auto error_name(const domain::ErrorCode value)
    -> std::string_view {
  switch (value) {
    case domain::ErrorCode::invalid_event: return "invalid_event";
    case domain::ErrorCode::invalid_state: return "invalid_state";
    case domain::ErrorCode::backend: return "backend";
    case domain::ErrorCode::policy: return "policy";
    case domain::ErrorCode::cancelled: return "cancelled";
    case domain::ErrorCode::unavailable: return "unavailable";
  }
  throw CodecFailure{"invalid error code"};
}

[[nodiscard]] auto parse_error_code(const Json& value) -> domain::ErrorCode {
  const auto name = value.get<std::string>();
  return enum_value<domain::ErrorCode>(
      name, {{"invalid_event", domain::ErrorCode::invalid_event},
             {"invalid_state", domain::ErrorCode::invalid_state},
             {"backend", domain::ErrorCode::backend},
             {"policy", domain::ErrorCode::policy},
             {"cancelled", domain::ErrorCode::cancelled},
             {"unavailable", domain::ErrorCode::unavailable}});
}

[[nodiscard]] auto effect_name(const domain::Effect value) -> std::string_view {
  switch (value) {
    case domain::Effect::read: return "read";
    case domain::Effect::write: return "write";
    case domain::Effect::remove: return "remove";
    case domain::Effect::execute: return "execute";
    case domain::Effect::network: return "network";
    case domain::Effect::communicate: return "communicate";
    case domain::Effect::spend: return "spend";
    case domain::Effect::change_infrastructure: return "change_infrastructure";
    case domain::Effect::change_privileges: return "change_privileges";
  }
  throw CodecFailure{"invalid effect"};
}

[[nodiscard]] auto parse_effect(const Json& value) -> domain::Effect {
  const auto name = value.get<std::string>();
  return enum_value<domain::Effect>(
      name, {{"read", domain::Effect::read}, {"write", domain::Effect::write},
             {"remove", domain::Effect::remove},
             {"execute", domain::Effect::execute},
             {"network", domain::Effect::network},
             {"communicate", domain::Effect::communicate},
             {"spend", domain::Effect::spend},
             {"change_infrastructure", domain::Effect::change_infrastructure},
             {"change_privileges", domain::Effect::change_privileges}});
}

[[nodiscard]] auto policy_name(const domain::PolicyDecision value)
    -> std::string_view {
  switch (value) {
    case domain::PolicyDecision::allow: return "allow";
    case domain::PolicyDecision::deny: return "deny";
    case domain::PolicyDecision::require_approval: return "require_approval";
  }
  throw CodecFailure{"invalid policy decision"};
}

[[nodiscard]] auto parse_policy(const Json& value) -> domain::PolicyDecision {
  const auto name = value.get<std::string>();
  return enum_value<domain::PolicyDecision>(
      name, {{"allow", domain::PolicyDecision::allow},
             {"deny", domain::PolicyDecision::deny},
             {"require_approval", domain::PolicyDecision::require_approval}});
}

[[nodiscard]] auto approval_name(const domain::ApprovalDecision value)
    -> std::string_view {
  switch (value) {
    case domain::ApprovalDecision::approved: return "approved";
    case domain::ApprovalDecision::denied: return "denied";
    case domain::ApprovalDecision::cancelled: return "cancelled";
  }
  throw CodecFailure{"invalid approval decision"};
}

[[nodiscard]] auto parse_approval(const Json& value)
    -> domain::ApprovalDecision {
  const auto name = value.get<std::string>();
  return enum_value<domain::ApprovalDecision>(
      name, {{"approved", domain::ApprovalDecision::approved},
             {"denied", domain::ApprovalDecision::denied},
             {"cancelled", domain::ApprovalDecision::cancelled}});
}

[[nodiscard]] auto selection_name(const domain::QuestionSelection value)
    -> std::string_view {
  switch (value) {
    case domain::QuestionSelection::one: return "one";
    case domain::QuestionSelection::many: return "many";
  }
  throw CodecFailure{"invalid question selection"};
}

[[nodiscard]] auto parse_selection(const Json& value)
    -> domain::QuestionSelection {
  const auto name = value.get<std::string>();
  return enum_value<domain::QuestionSelection>(
      name, {{"one", domain::QuestionSelection::one},
             {"many", domain::QuestionSelection::many}});
}

[[nodiscard]] auto structured_json(const domain::StructuredDataBlock& value)
    -> Json {
  return {{"media_type", value.media_type}, {"data", value.data}};
}

[[nodiscard]] auto parse_structured(const Json& value)
    -> domain::StructuredDataBlock {
  return {value.at("media_type").get<std::string>(),
          value.at("data").get<std::string>()};
}

[[nodiscard]] auto content_json(const domain::ContentBlock& block) -> Json {
  return std::visit(
      Overloaded{
          [](const domain::TextBlock& value) -> Json {
            return {{"kind", "text"}, {"text", value.text}};
          },
          [](const domain::StructuredDataBlock& value) -> Json {
            return {{"kind", "structured"},
                    {"media_type", value.media_type},
                    {"data", value.data}};
          },
          [](const domain::CitationBlock& value) -> Json {
            return {{"kind", "citation"}, {"uri", value.uri},
                    {"title", optional_string_json(value.title)}};
          },
          [](const domain::ArtifactReferenceBlock& value) -> Json {
            return {{"kind", "artifact_reference"},
                    {"artifact_id", id_text(value.artifact_id)},
                    {"label", optional_string_json(value.label)}};
          },
          [](const domain::UnknownContentBlock& value) -> Json {
            return {{"kind", "unknown"}, {"type_name", value.type_name}};
          }},
      block);
}

[[nodiscard]] auto parse_content(const Json& value) -> domain::ContentBlock {
  const auto kind = value.at("kind").get<std::string>();
  if (kind == "text") return domain::TextBlock{value.at("text").get<std::string>()};
  if (kind == "structured") {
    return domain::StructuredDataBlock{value.at("media_type").get<std::string>(),
                                       value.at("data").get<std::string>()};
  }
  if (kind == "citation") {
    return domain::CitationBlock{value.at("uri").get<std::string>(),
                                 parse_optional_string(value.at("title"))};
  }
  if (kind == "artifact_reference") {
    return domain::ArtifactReferenceBlock{
        parse_id<domain::ArtifactId>(value.at("artifact_id")),
        parse_optional_string(value.at("label"))};
  }
  if (kind == "unknown") {
    return domain::UnknownContentBlock{value.at("type_name").get<std::string>()};
  }
  throw CodecFailure{"unknown content block kind"};
}

[[nodiscard]] auto content_list_json(const std::vector<domain::ContentBlock>& values)
    -> Json {
  auto result = Json::array();
  for (const auto& value : values) result.push_back(content_json(value));
  return result;
}

[[nodiscard]] auto parse_content_list(const Json& values)
    -> std::vector<domain::ContentBlock> {
  if (!values.is_array()) throw CodecFailure{"content list is invalid"};
  std::vector<domain::ContentBlock> result;
  result.reserve(values.size());
  for (const auto& value : values) result.push_back(parse_content(value));
  return result;
}

[[nodiscard]] auto message_json(const domain::Message& message) -> Json {
  return {{"message_id", id_text(message.message_id)},
          {"role", role_name(message.role)},
          {"content", content_list_json(message.content)},
          {"invocation_id", optional_id_json(message.invocation_id)}};
}

[[nodiscard]] auto parse_message(const Json& value) -> domain::Message {
  return {parse_id<domain::MessageId>(value.at("message_id")),
          parse_role(value.at("role")),
          parse_content_list(value.at("content")),
          parse_optional_id<domain::InvocationId>(value.at("invocation_id"))};
}

[[nodiscard]] auto domain_error_json(const domain::DomainError& error) -> Json {
  return {{"code", error_name(error.code)}, {"message", error.message},
          {"retryable", error.retryable}};
}

[[nodiscard]] auto parse_domain_error(const Json& value) -> domain::DomainError {
  return {parse_error_code(value.at("code")),
          value.at("message").get<std::string>(),
          value.at("retryable").get<bool>()};
}

[[nodiscard]] auto usage_json(const domain::Usage& usage) -> Json {
  return {{"input_tokens", usage.input_tokens},
          {"output_tokens", usage.output_tokens},
          {"cached_input_tokens", usage.cached_input_tokens},
          {"reasoning_tokens", usage.reasoning_tokens}};
}

[[nodiscard]] auto parse_usage(const Json& value) -> domain::Usage {
  return {value.at("input_tokens").get<std::uint64_t>(),
          value.at("output_tokens").get<std::uint64_t>(),
          value.at("cached_input_tokens").get<std::uint64_t>(),
          value.at("reasoning_tokens").get<std::uint64_t>()};
}

[[nodiscard]] auto metadata_json(const domain::Metadata& metadata) -> Json {
  auto result = Json::array();
  for (const auto& [key, value] : metadata) {
    result.push_back(Json{{"key", key}, {"value", value}});
  }
  return result;
}

[[nodiscard]] auto parse_metadata(const Json& values) -> domain::Metadata {
  if (!values.is_array()) throw CodecFailure{"metadata list is invalid"};
  domain::Metadata result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value.at("key").get<std::string>(),
                        value.at("value").get<std::string>());
  }
  return result;
}

[[nodiscard]] auto effects_json(const std::vector<domain::Effect>& effects) -> Json {
  auto result = Json::array();
  for (const auto effect : effects) result.push_back(effect_name(effect));
  return result;
}

[[nodiscard]] auto parse_effects(const Json& values) -> std::vector<domain::Effect> {
  if (!values.is_array()) throw CodecFailure{"effect list is invalid"};
  std::vector<domain::Effect> result;
  result.reserve(values.size());
  for (const auto& value : values) result.push_back(parse_effect(value));
  return result;
}

[[nodiscard]] auto scope_json(const domain::CapabilityScope& scope) -> Json {
  return {{"effect", effect_name(scope.effect)}, {"kind", scope.kind},
          {"value", scope.value}};
}

[[nodiscard]] auto scopes_json(const std::vector<domain::CapabilityScope>& scopes)
    -> Json {
  auto result = Json::array();
  for (const auto& scope : scopes) result.push_back(scope_json(scope));
  return result;
}

[[nodiscard]] auto parse_scopes(const Json& values)
    -> std::vector<domain::CapabilityScope> {
  if (!values.is_array()) throw CodecFailure{"capability-scope list is invalid"};
  std::vector<domain::CapabilityScope> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.push_back({parse_effect(value.at("effect")),
                      value.at("kind").get<std::string>(),
                      value.at("value").get<std::string>()});
  }
  return result;
}

[[nodiscard]] auto question_json(const domain::QuestionDefinition& question)
    -> Json {
  auto options = Json::array();
  for (const auto& option : question.options) {
    options.push_back({{"option_id", option.option_id}, {"label", option.label},
                       {"description", optional_string_json(option.description)}});
  }
  return {{"question_id", id_text(question.question_id)},
          {"prompt", question.prompt},
          {"selection", selection_name(question.selection)},
          {"options", std::move(options)},
          {"free_form_allowed", question.free_form_allowed},
          {"answer_optional", question.answer_optional}};
}

[[nodiscard]] auto parse_question(const Json& value)
    -> domain::QuestionDefinition {
  std::vector<domain::QuestionOption> options;
  for (const auto& option : value.at("options")) {
    options.push_back({option.at("option_id").get<std::string>(),
                       option.at("label").get<std::string>(),
                       parse_optional_string(option.at("description"))});
  }
  return {parse_id<domain::QuestionId>(value.at("question_id")),
          value.at("prompt").get<std::string>(),
          parse_selection(value.at("selection")), std::move(options),
          value.at("free_form_allowed").get<bool>(),
          value.at("answer_optional").get<bool>()};
}

[[nodiscard]] auto artifact_json(const domain::ArtifactMetadata& artifact)
    -> Json {
  return {{"artifact_id", id_text(artifact.artifact_id)},
          {"media_type", artifact.media_type},
          {"byte_size", artifact.byte_size},
          {"digest", artifact.digest},
          {"producing_invocation_id",
           optional_id_json(artifact.producing_invocation_id)},
          {"width", artifact.width ? Json(*artifact.width) : Json(nullptr)},
          {"height", artifact.height ? Json(*artifact.height) : Json(nullptr)}};
}

[[nodiscard]] auto parse_optional_u32(const Json& value)
    -> std::optional<std::uint32_t> {
  if (value.is_null()) return std::nullopt;
  return value.get<std::uint32_t>();
}

[[nodiscard]] auto parse_artifact(const Json& value)
    -> domain::ArtifactMetadata {
  return {parse_id<domain::ArtifactId>(value.at("artifact_id")),
          value.at("media_type").get<std::string>(),
          value.at("byte_size").get<std::uint64_t>(),
          value.at("digest").get<std::string>(),
          parse_optional_id<domain::InvocationId>(
              value.at("producing_invocation_id")),
          parse_optional_u32(value.at("width")),
          parse_optional_u32(value.at("height"))};
}

[[nodiscard]] auto payload_type(const domain::RunEventPayload& payload)
    -> std::string {
  return std::visit(
      Overloaded{
          [](const domain::RunStarted&) { return std::string{"run.started"}; },
          [](const domain::RunAwaitingInput&) { return std::string{"run.awaiting_input"}; },
          [](const domain::RunResumed&) { return std::string{"run.resumed"}; },
          [](const domain::RunCompletionRequested&) { return std::string{"run.completion_requested"}; },
          [](const domain::RunCompleted&) { return std::string{"run.completed"}; },
          [](const domain::RunFailed&) { return std::string{"run.failed"}; },
          [](const domain::RunCancelRequested&) { return std::string{"run.cancel_requested"}; },
          [](const domain::RunCancelled&) { return std::string{"run.cancelled"}; },
          [](const domain::UserContentAdded&) { return std::string{"content.user_added"}; },
          [](const domain::AssistantContentStarted&) { return std::string{"content.assistant_started"}; },
          [](const domain::AssistantContentDeltaAdded&) { return std::string{"content.assistant_delta_added"}; },
          [](const domain::AssistantContentFinished&) { return std::string{"content.assistant_finished"}; },
          [](const domain::InferenceStarted&) { return std::string{"inference.started"}; },
          [](const domain::ReasoningMetadataAdded&) { return std::string{"inference.reasoning_metadata_added"}; },
          [](const domain::UsageRecorded&) { return std::string{"inference.usage_recorded"}; },
          [](const domain::InferenceFinished&) { return std::string{"inference.finished"}; },
          [](const domain::InferenceFailed&) { return std::string{"inference.failed"}; },
          [](const domain::InferenceCancelled&) { return std::string{"inference.cancelled"}; },
          [](const domain::ToolProposed&) { return std::string{"tool.proposed"}; },
          [](const domain::ToolPolicyDecided&) { return std::string{"tool.policy_decided"}; },
          [](const domain::ToolApprovalRequested&) { return std::string{"tool.approval_requested"}; },
          [](const domain::ToolApprovalDecided&) { return std::string{"tool.approval_decided"}; },
          [](const domain::ToolStarted&) { return std::string{"tool.started"}; },
          [](const domain::ToolProgressed&) { return std::string{"tool.progressed"}; },
          [](const domain::ToolResultRecorded&) { return std::string{"tool.result_recorded"}; },
          [](const domain::ToolErrored&) { return std::string{"tool.errored"}; },
          [](const domain::QuestionRequested&) { return std::string{"question.requested"}; },
          [](const domain::QuestionAnswered&) { return std::string{"question.answered"}; },
          [](const domain::QuestionCancelled&) { return std::string{"question.cancelled"}; },
          [](const domain::ArtifactCreated&) { return std::string{"artifact.created"}; },
          [](const domain::ArtifactReferenced&) { return std::string{"artifact.referenced"}; },
          [](const domain::ArtifactDisplayed&) { return std::string{"artifact.displayed"}; },
          [](const domain::ArtifactRemovedFromView&) { return std::string{"artifact.removed_from_view"}; },
          [](const domain::ChildRunCreated&) { return std::string{"run.child_created"}; },
          [](const domain::InterRunMessageSent&) { return std::string{"run.inter_message_sent"}; },
          [](const domain::UnknownEvent& value) { return value.type_name; }},
      payload);
}

[[nodiscard]] auto known_payload_type(const std::string_view type) -> bool {
  static const std::set<std::string_view> types{
      "run.started", "run.awaiting_input", "run.resumed",
      "run.completion_requested", "run.completed", "run.failed",
      "run.cancel_requested", "run.cancelled", "content.user_added",
      "content.assistant_started", "content.assistant_delta_added",
      "content.assistant_finished", "inference.started",
      "inference.reasoning_metadata_added", "inference.usage_recorded",
      "inference.finished", "inference.failed", "inference.cancelled",
      "tool.proposed", "tool.policy_decided", "tool.approval_requested",
      "tool.approval_decided", "tool.started", "tool.progressed",
      "tool.result_recorded", "tool.errored", "question.requested",
      "question.answered", "question.cancelled", "artifact.created",
      "artifact.referenced", "artifact.displayed",
      "artifact.removed_from_view", "run.child_created",
      "run.inter_message_sent"};
  return types.contains(type);
}

[[nodiscard]] auto payload_json(const domain::RunEventPayload& payload) -> Json {
  return std::visit(
      Overloaded{
          [](const domain::RunStarted& value) -> Json {
            return {{"surface_id", id_text(value.surface_id)},
                    {"workspace_id", id_text(value.workspace_id)},
                    {"permission_profile_id", id_text(value.permission_profile_id)},
                    {"persona_id", optional_id_json(value.persona_id)}};
          },
          [](const domain::RunAwaitingInput& value) -> Json {
            return {{"question_id", id_text(value.question_id)}};
          },
          [](const domain::RunResumed& value) -> Json {
            return {{"question_id", optional_id_json(value.question_id)}};
          },
          [](const domain::RunCompletionRequested&) -> Json { return Json::object(); },
          [](const domain::RunCompleted&) -> Json { return Json::object(); },
          [](const domain::RunFailed& value) -> Json {
            return {{"error", domain_error_json(value.error)}};
          },
          [](const domain::RunCancelRequested& value) -> Json {
            return {{"reason", optional_string_json(value.reason)}};
          },
          [](const domain::RunCancelled& value) -> Json {
            return {{"reason", optional_string_json(value.reason)}};
          },
          [](const domain::UserContentAdded& value) -> Json {
            return {{"message", message_json(value.message)}};
          },
          [](const domain::AssistantContentStarted& value) -> Json {
            return {{"message_id", id_text(value.message_id)},
                    {"inference_id", id_text(value.inference_id)}};
          },
          [](const domain::AssistantContentDeltaAdded& value) -> Json {
            return {{"message_id", id_text(value.message_id)},
                    {"inference_id", id_text(value.inference_id)},
                    {"delta", content_json(value.delta)}};
          },
          [](const domain::AssistantContentFinished& value) -> Json {
            return {{"message_id", id_text(value.message_id)},
                    {"inference_id", id_text(value.inference_id)}};
          },
          [](const domain::InferenceStarted& value) -> Json {
            return {{"inference_id", id_text(value.inference_id)},
                    {"model_id", id_text(value.model_id)}};
          },
          [](const domain::ReasoningMetadataAdded& value) -> Json {
            return {{"inference_id", id_text(value.inference_id)},
                    {"text", optional_string_json(value.text)},
                    {"metadata", metadata_json(value.metadata)}};
          },
          [](const domain::UsageRecorded& value) -> Json {
            return {{"inference_id", id_text(value.inference_id)},
                    {"usage", usage_json(value.usage)}};
          },
          [](const domain::InferenceFinished& value) -> Json {
            return {{"inference_id", id_text(value.inference_id)},
                    {"reason", finish_name(value.reason)}};
          },
          [](const domain::InferenceFailed& value) -> Json {
            return {{"inference_id", id_text(value.inference_id)},
                    {"error", domain_error_json(value.error)}};
          },
          [](const domain::InferenceCancelled& value) -> Json {
            return {{"inference_id", id_text(value.inference_id)},
                    {"reason", optional_string_json(value.reason)}};
          },
          [](const domain::ToolProposed& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"tool_name", value.tool_name},
                    {"arguments", structured_json(value.arguments)},
                    {"declared_effects", effects_json(value.declared_effects)}};
          },
          [](const domain::ToolPolicyDecided& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"decision", policy_name(value.decision)},
                    {"scopes", scopes_json(value.scopes)},
                    {"reason", optional_string_json(value.reason)}};
          },
          [](const domain::ToolApprovalRequested& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"requested_scopes", scopes_json(value.requested_scopes)}};
          },
          [](const domain::ToolApprovalDecided& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"decision", approval_name(value.decision)},
                    {"granted_scopes", scopes_json(value.granted_scopes)}};
          },
          [](const domain::ToolStarted& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)}};
          },
          [](const domain::ToolProgressed& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"content", content_list_json(value.content)}};
          },
          [](const domain::ToolResultRecorded& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"content", content_list_json(value.content)}};
          },
          [](const domain::ToolErrored& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"error", domain_error_json(value.error)}};
          },
          [](const domain::QuestionRequested& value) -> Json {
            return {{"question", question_json(value.question)}};
          },
          [](const domain::QuestionAnswered& value) -> Json {
            return {{"question_id", id_text(value.answer.question_id)},
                    {"selected_option_ids", value.answer.selected_option_ids},
                    {"free_form", optional_string_json(value.answer.free_form)}};
          },
          [](const domain::QuestionCancelled& value) -> Json {
            return {{"question_id", id_text(value.question_id)},
                    {"reason", optional_string_json(value.reason)}};
          },
          [](const domain::ArtifactCreated& value) -> Json {
            return {{"artifact", artifact_json(value.artifact)}};
          },
          [](const domain::ArtifactReferenced& value) -> Json {
            return {{"artifact_id", id_text(value.artifact_id)},
                    {"message_id", optional_id_json(value.message_id)}};
          },
          [](const domain::ArtifactDisplayed& value) -> Json {
            return {{"artifact_id", id_text(value.artifact_id)},
                    {"view_id", id_text(value.view_id)},
                    {"semantic_slot", value.semantic_slot}};
          },
          [](const domain::ArtifactRemovedFromView& value) -> Json {
            return {{"artifact_id", id_text(value.artifact_id)},
                    {"view_id", id_text(value.view_id)}};
          },
          [](const domain::ChildRunCreated& value) -> Json {
            return {{"child_run_id", id_text(value.child_run_id)}};
          },
          [](const domain::InterRunMessageSent& value) -> Json {
            return {{"target_run_id", id_text(value.target_run_id)},
                    {"content", content_list_json(value.content)}};
          },
          [](const domain::UnknownEvent& value) -> Json {
            if (value.payload.media_type != "application/json") {
              throw CodecFailure{"unknown event payload is not JSON"};
            }
            std::vector<std::unordered_set<std::string>> keys;
            const auto callback = [&keys](const int, const Json::parse_event_t event,
                                          Json& parsed) {
              if (event == Json::parse_event_t::object_start) {
                keys.emplace_back();
              } else if (event == Json::parse_event_t::key) {
                if (keys.empty() ||
                    !keys.back().insert(parsed.get<std::string>()).second) {
                  throw DuplicateJsonKey{};
                }
              } else if (event == Json::parse_event_t::object_end) {
                keys.pop_back();
              }
              return true;
            };
            auto parsed = Json::parse(value.payload.data, callback, true, false);
            if (parsed.dump() != value.payload.data) {
              throw CodecFailure{"unknown event payload is not canonical JSON"};
            }
            return parsed;
          }},
      payload);
}

[[nodiscard]] auto parse_payload(const std::string_view type, const Json& value)
    -> domain::RunEventPayload {
  if (type == "run.started") {
    return domain::RunStarted{
        parse_id<domain::SurfaceId>(value.at("surface_id")),
        parse_id<domain::WorkspaceId>(value.at("workspace_id")),
        parse_id<domain::PermissionProfileId>(value.at("permission_profile_id")),
        parse_optional_id<domain::PersonaId>(value.at("persona_id"))};
  }
  if (type == "run.awaiting_input") {
    return domain::RunAwaitingInput{
        parse_id<domain::QuestionId>(value.at("question_id"))};
  }
  if (type == "run.resumed") {
    return domain::RunResumed{
        parse_optional_id<domain::QuestionId>(value.at("question_id"))};
  }
  if (type == "run.completion_requested") return domain::RunCompletionRequested{};
  if (type == "run.completed") return domain::RunCompleted{};
  if (type == "run.failed") {
    return domain::RunFailed{parse_domain_error(value.at("error"))};
  }
  if (type == "run.cancel_requested") {
    return domain::RunCancelRequested{parse_optional_string(value.at("reason"))};
  }
  if (type == "run.cancelled") {
    return domain::RunCancelled{parse_optional_string(value.at("reason"))};
  }
  if (type == "content.user_added") {
    return domain::UserContentAdded{parse_message(value.at("message"))};
  }
  if (type == "content.assistant_started") {
    return domain::AssistantContentStarted{
        parse_id<domain::MessageId>(value.at("message_id")),
        parse_id<domain::InferenceId>(value.at("inference_id"))};
  }
  if (type == "content.assistant_delta_added") {
    return domain::AssistantContentDeltaAdded{
        parse_id<domain::MessageId>(value.at("message_id")),
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_content(value.at("delta"))};
  }
  if (type == "content.assistant_finished") {
    return domain::AssistantContentFinished{
        parse_id<domain::MessageId>(value.at("message_id")),
        parse_id<domain::InferenceId>(value.at("inference_id"))};
  }
  if (type == "inference.started") {
    return domain::InferenceStarted{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_id<domain::ModelId>(value.at("model_id"))};
  }
  if (type == "inference.reasoning_metadata_added") {
    return domain::ReasoningMetadataAdded{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_optional_string(value.at("text")),
        parse_metadata(value.at("metadata"))};
  }
  if (type == "inference.usage_recorded") {
    return domain::UsageRecorded{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_usage(value.at("usage"))};
  }
  if (type == "inference.finished") {
    return domain::InferenceFinished{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_finish(value.at("reason"))};
  }
  if (type == "inference.failed") {
    return domain::InferenceFailed{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_domain_error(value.at("error"))};
  }
  if (type == "inference.cancelled") {
    return domain::InferenceCancelled{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_optional_string(value.at("reason"))};
  }
  if (type == "tool.proposed") {
    return domain::ToolProposed{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        value.at("tool_name").get<std::string>(),
        parse_structured(value.at("arguments")),
        parse_effects(value.at("declared_effects"))};
  }
  if (type == "tool.policy_decided") {
    return domain::ToolPolicyDecided{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_policy(value.at("decision")), parse_scopes(value.at("scopes")),
        parse_optional_string(value.at("reason"))};
  }
  if (type == "tool.approval_requested") {
    return domain::ToolApprovalRequested{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_scopes(value.at("requested_scopes"))};
  }
  if (type == "tool.approval_decided") {
    return domain::ToolApprovalDecided{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_approval(value.at("decision")),
        parse_scopes(value.at("granted_scopes"))};
  }
  if (type == "tool.started") {
    return domain::ToolStarted{
        parse_id<domain::InvocationId>(value.at("invocation_id"))};
  }
  if (type == "tool.progressed") {
    return domain::ToolProgressed{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_content_list(value.at("content"))};
  }
  if (type == "tool.result_recorded") {
    return domain::ToolResultRecorded{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_content_list(value.at("content"))};
  }
  if (type == "tool.errored") {
    return domain::ToolErrored{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_domain_error(value.at("error"))};
  }
  if (type == "question.requested") {
    return domain::QuestionRequested{parse_question(value.at("question"))};
  }
  if (type == "question.answered") {
    return domain::QuestionAnswered{domain::QuestionAnswer{
        parse_id<domain::QuestionId>(value.at("question_id")),
        value.at("selected_option_ids").get<std::vector<std::string>>(),
        parse_optional_string(value.at("free_form"))}};
  }
  if (type == "question.cancelled") {
    return domain::QuestionCancelled{
        parse_id<domain::QuestionId>(value.at("question_id")),
        parse_optional_string(value.at("reason"))};
  }
  if (type == "artifact.created") {
    return domain::ArtifactCreated{parse_artifact(value.at("artifact"))};
  }
  if (type == "artifact.referenced") {
    return domain::ArtifactReferenced{
        parse_id<domain::ArtifactId>(value.at("artifact_id")),
        parse_optional_id<domain::MessageId>(value.at("message_id"))};
  }
  if (type == "artifact.displayed") {
    return domain::ArtifactDisplayed{
        parse_id<domain::ArtifactId>(value.at("artifact_id")),
        parse_id<domain::ViewId>(value.at("view_id")),
        value.at("semantic_slot").get<std::string>()};
  }
  if (type == "artifact.removed_from_view") {
    return domain::ArtifactRemovedFromView{
        parse_id<domain::ArtifactId>(value.at("artifact_id")),
        parse_id<domain::ViewId>(value.at("view_id"))};
  }
  if (type == "run.child_created") {
    return domain::ChildRunCreated{
        parse_id<domain::RunId>(value.at("child_run_id"))};
  }
  if (type == "run.inter_message_sent") {
    return domain::InterRunMessageSent{
        parse_id<domain::RunId>(value.at("target_run_id")),
        parse_content_list(value.at("content"))};
  }
  throw CodecFailure{"unknown event payload type"};
}

[[nodiscard]] auto parse_json_document(const std::string& text)
    -> std::expected<Json, SessionStoreError> {
  try {
    std::vector<std::unordered_set<std::string>> keys;
    const auto callback = [&keys](const int, const Json::parse_event_t event,
                                  Json& parsed) {
      if (event == Json::parse_event_t::object_start) {
        keys.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        if (keys.empty() ||
            !keys.back().insert(parsed.get<std::string>()).second) {
          throw DuplicateJsonKey{};
        }
      } else if (event == Json::parse_event_t::object_end) {
        keys.pop_back();
      }
      return true;
    };
    auto parsed = Json::parse(text, callback, true, false);
    if (parsed.dump() != text) {
      return std::unexpected(store_error(
          SessionStoreErrorCode::corrupt,
          "persisted event payload is not in canonical form"));
    }
    return parsed;
  } catch (const DuplicateJsonKey&) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "persisted event payload has a duplicate key"));
  } catch (const Json::exception&) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "persisted event payload is invalid UTF-8 JSON"));
  }
}

struct EncodedPayload {
  std::string type;
  std::string document;
};

[[nodiscard]] auto encode_payload(const domain::RunEvent& event)
    -> std::expected<EncodedPayload, SessionStoreError> {
  try {
    const auto type = payload_type(event.payload);
    if (type.empty() || type.size() > 256 || has_control_character(type)) {
      return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                         "event payload type is invalid"));
    }
    if (event.metadata.schema_version != 1 &&
        !std::holds_alternative<domain::UnknownEvent>(event.payload)) {
      return std::unexpected(store_error(
          SessionStoreErrorCode::unsupported_version,
          "known event payload uses an unsupported schema version"));
    }
    if (event.metadata.schema_version == 1 && known_payload_type(type) &&
        std::holds_alternative<domain::UnknownEvent>(event.payload)) {
      return std::unexpected(store_error(
          SessionStoreErrorCode::invalid_argument,
          "known schema-v1 event type cannot carry an opaque payload"));
    }
    return EncodedPayload{type, payload_json(event.payload).dump()};
  } catch (const DuplicateJsonKey&) {
    return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                       "unknown event payload has a duplicate key"));
  } catch (const CodecFailure&) {
    return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                       "event payload is invalid"));
  } catch (const Json::exception&) {
    return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                       "event payload cannot be encoded as UTF-8 JSON"));
  }
}

class Statement final {
 public:
  Statement() = default;
  explicit Statement(sqlite3_stmt* statement) : m_statement(statement) {}
  Statement(const Statement&) = delete;
  auto operator=(const Statement&) -> Statement& = delete;
  Statement(Statement&& other) noexcept
      : m_statement(std::exchange(other.m_statement, nullptr)) {}
  auto operator=(Statement&& other) noexcept -> Statement& {
    if (this != &other) {
      reset();
      m_statement = std::exchange(other.m_statement, nullptr);
    }
    return *this;
  }
  ~Statement() { reset(); }

  [[nodiscard]] auto get() const noexcept -> sqlite3_stmt* { return m_statement; }
  auto reset() noexcept -> void {
    if (m_statement != nullptr) static_cast<void>(sqlite3_finalize(m_statement));
    m_statement = nullptr;
  }

 private:
  sqlite3_stmt* m_statement{};
};

[[nodiscard]] auto sqlite_error(const int result) -> SessionStoreError {
  const auto primary = result & 0xFF;
  switch (primary) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
      return store_error(SessionStoreErrorCode::contention,
                         "session store is busy", true);
    case SQLITE_CONSTRAINT:
      return store_error(SessionStoreErrorCode::conflict,
                         "session-store constraint rejected the operation");
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_SCHEMA:
      return store_error(SessionStoreErrorCode::corrupt,
                         "session store is corrupt or inconsistent");
    case SQLITE_FULL:
    case SQLITE_NOMEM:
    case SQLITE_TOOBIG:
      return store_error(SessionStoreErrorCode::resource_exhausted,
                         "session-store resource limit was exhausted");
    case SQLITE_READONLY:
    case SQLITE_PERM:
    case SQLITE_AUTH:
      return store_error(SessionStoreErrorCode::permission_denied,
                         "session-store access was denied");
    case SQLITE_CANTOPEN:
      return store_error(SessionStoreErrorCode::io_failure,
                         "session store could not be opened");
    case SQLITE_IOERR:
      return store_error(SessionStoreErrorCode::io_failure,
                         "session-store I/O failed");
    case SQLITE_INTERRUPT:
      return cancelled_error();
    default:
      return store_error(SessionStoreErrorCode::internal_failure,
                         "session-store operation failed internally");
  }
}

[[nodiscard]] auto execute(sqlite3* database, const std::string_view sql)
    -> std::expected<void, SessionStoreError> {
  char* message{};
  const auto result = sqlite3_exec(database, std::string{sql}.c_str(), nullptr,
                                   nullptr, &message);
  if (message != nullptr) sqlite3_free(message);
  if (result != SQLITE_OK) return std::unexpected(sqlite_error(result));
  return {};
}

[[nodiscard]] auto prepare(sqlite3* database, const std::string_view sql)
    -> std::expected<Statement, SessionStoreError> {
  sqlite3_stmt* statement{};
  const auto result = sqlite3_prepare_v2(database, sql.data(),
                                         static_cast<int>(sql.size()),
                                         &statement, nullptr);
  if (result != SQLITE_OK) return std::unexpected(sqlite_error(result));
  return Statement{statement};
}

[[nodiscard]] auto bind_text(sqlite3_stmt* statement, const int index,
                             const std::string_view value)
    -> std::expected<void, SessionStoreError> {
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected(store_error(SessionStoreErrorCode::resource_exhausted,
                                       "session-store text value is too large"));
  }
  const auto result = sqlite3_bind_text(statement, index, value.data(),
                                        static_cast<int>(value.size()),
                                        SQLITE_TRANSIENT);
  if (result != SQLITE_OK) return std::unexpected(sqlite_error(result));
  return {};
}

template <typename IdType>
[[nodiscard]] auto bind_optional_id(sqlite3_stmt* statement, const int index,
                                    const std::optional<IdType>& id)
    -> std::expected<void, SessionStoreError> {
  if (id) return bind_text(statement, index, id->value());
  const auto result = sqlite3_bind_null(statement, index);
  if (result != SQLITE_OK) return std::unexpected(sqlite_error(result));
  return {};
}

[[nodiscard]] auto step_done(sqlite3_stmt* statement)
    -> std::expected<void, SessionStoreError> {
  const auto result = sqlite3_step(statement);
  if (result != SQLITE_DONE) return std::unexpected(sqlite_error(result));
  return {};
}

[[nodiscard]] auto column_text(sqlite3_stmt* statement, const int index)
    -> std::expected<std::string, SessionStoreError> {
  if (sqlite3_column_type(statement, index) != SQLITE_TEXT) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "session-store text column is invalid"));
  }
  const auto* bytes = sqlite3_column_text(statement, index);
  const auto size = sqlite3_column_bytes(statement, index);
  if (bytes == nullptr || size < 0) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "session-store text column is invalid"));
  }
  return std::string{reinterpret_cast<const char*>(bytes),
                     static_cast<std::size_t>(size)};
}

template <typename IdType>
[[nodiscard]] auto column_id(sqlite3_stmt* statement, const int index)
    -> std::expected<IdType, SessionStoreError> {
  auto text = column_text(statement, index);
  if (!text) return std::unexpected(std::move(text.error()));
  auto parsed = IdType::from(std::move(*text));
  if (!parsed) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "persisted event identity is invalid"));
  }
  return std::move(*parsed);
}

template <typename IdType>
[[nodiscard]] auto column_optional_id(sqlite3_stmt* statement, const int index)
    -> std::expected<std::optional<IdType>, SessionStoreError> {
  if (sqlite3_column_type(statement, index) == SQLITE_NULL) {
    return std::optional<IdType>{};
  }
  auto id = column_id<IdType>(statement, index);
  if (!id) return std::unexpected(std::move(id.error()));
  return std::optional<IdType>{std::move(*id)};
}

[[nodiscard]] auto timestamp_from_count(const sqlite3_int64 count)
    -> domain::EventTimestamp {
  return domain::EventTimestamp{std::chrono::milliseconds{count}};
}

[[nodiscard]] auto timestamp_count(const domain::EventTimestamp value)
    -> std::expected<sqlite3_int64, SessionStoreError> {
  const auto count = value.time_since_epoch().count();
  if constexpr (sizeof(count) > sizeof(sqlite3_int64)) {
    if (count < std::numeric_limits<sqlite3_int64>::min() ||
        count > std::numeric_limits<sqlite3_int64>::max()) {
      return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                         "event timestamp is outside storage range"));
    }
  }
  return static_cast<sqlite3_int64>(count);
}

[[nodiscard]] auto path_error(const SessionStoreErrorCode code,
                              std::string message) -> SessionStoreError {
  return store_error(code, std::move(message));
}

[[nodiscard]] auto check_directory(const std::filesystem::path& directory,
                                   const bool create)
    -> std::expected<bool, SessionStoreError> {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot inspect the session-state directory"));
  }
  if (std::filesystem::exists(status)) {
    if (std::filesystem::is_symlink(status)) {
      return std::unexpected(path_error(SessionStoreErrorCode::permission_denied,
                                        "session-state directory cannot be a symlink"));
    }
    if (!std::filesystem::is_directory(status)) {
      return std::unexpected(path_error(SessionStoreErrorCode::permission_denied,
                                        "session-state path is not a directory"));
    }
    struct stat info {};
    if (::stat(directory.c_str(), &info) != 0) {
      return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                        "cannot inspect session-state permissions"));
    }
    if ((info.st_mode & 0777) != 0700) {
      return std::unexpected(path_error(SessionStoreErrorCode::permission_denied,
                                        "session-state directory must have mode 0700"));
    }
    return false;
  }
  if (!create) return false;

  std::filesystem::create_directories(directory.parent_path(), error);
  if (error) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot create the state base directory"));
  }
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot create the session-state directory"));
  }
  if (::chmod(directory.c_str(), 0700) != 0) {
    return std::unexpected(path_error(SessionStoreErrorCode::permission_denied,
                                      "cannot secure the session-state directory"));
  }
  return true;
}

[[nodiscard]] auto check_regular_secure_file(const std::filesystem::path& path)
    -> std::expected<bool, SessionStoreError> {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot inspect the session database"));
  }
  if (!std::filesystem::exists(status)) return false;
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return std::unexpected(path_error(SessionStoreErrorCode::permission_denied,
                                      "session database must be a regular non-symlink file"));
  }
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot inspect session database permissions"));
  }
  if ((info.st_mode & 0777) != 0600) {
    return std::unexpected(path_error(SessionStoreErrorCode::permission_denied,
                                      "session database must have mode 0600"));
  }
  return true;
}

[[nodiscard]] auto precreate_database(const std::filesystem::path& path)
    -> std::expected<void, SessionStoreError> {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL |
                                                  O_CLOEXEC | O_NOFOLLOW,
                                0600);
  if (descriptor < 0) {
    return std::unexpected(path_error(
        errno == EACCES || errno == EPERM ? SessionStoreErrorCode::permission_denied
                                          : SessionStoreErrorCode::io_failure,
        "cannot create the session database"));
  }
  const auto close_result = ::close(descriptor);
  if (close_result != 0) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot close the new session database"));
  }
  return {};
}

[[nodiscard]] auto sync_directory(const std::filesystem::path& directory)
    -> std::expected<void, SessionStoreError> {
  const int descriptor =
      ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot open the session-state directory for sync"));
  }
  const auto synced = ::fsync(descriptor);
  const auto saved_errno = errno;
  static_cast<void>(::close(descriptor));
  errno = saved_errno;
  if (synced != 0) {
    return std::unexpected(path_error(SessionStoreErrorCode::io_failure,
                                      "cannot sync the session-state directory"));
  }
  return {};
}

[[nodiscard]] auto valid_limits(const storage::SessionStoreLimits& limits) -> bool {
  return limits.maximum_batch_events != 0 && limits.maximum_payload_bytes != 0 &&
         limits.maximum_replay_events != 0 && limits.maximum_replay_bytes != 0 &&
         limits.busy_timeout.count() > 0 &&
         limits.busy_timeout.count() <= std::numeric_limits<int>::max();
}

auto rollback(sqlite3* database) noexcept -> void {
  static_cast<void>(sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr));
}

class Transaction final {
 public:
  explicit Transaction(sqlite3* database) : m_database(database) {}
  Transaction(const Transaction&) = delete;
  auto operator=(const Transaction&) -> Transaction& = delete;
  ~Transaction() {
    if (m_active) rollback(m_database);
  }
  auto complete() noexcept -> void { m_active = false; }

 private:
  sqlite3* m_database{};
  bool m_active{true};
};

[[nodiscard]] auto begin_immediate(sqlite3* database)
    -> std::expected<void, SessionStoreError> {
  return execute(database, "BEGIN IMMEDIATE");
}

[[nodiscard]] auto commit(sqlite3* database)
    -> std::expected<void, SessionStoreError> {
  auto result = execute(database, "COMMIT");
  if (!result) rollback(database);
  return result;
}

[[nodiscard]] auto migrate(sqlite3* database)
    -> std::expected<void, SessionStoreError> {
  auto version_statement = prepare(database, "PRAGMA user_version");
  if (!version_statement) return std::unexpected(std::move(version_statement.error()));
  const auto version_step = sqlite3_step(version_statement->get());
  if (version_step != SQLITE_ROW) return std::unexpected(sqlite_error(version_step));
  const auto version = sqlite3_column_int(version_statement->get(), 0);
  if (version > storage_format_version) {
    return std::unexpected(store_error(SessionStoreErrorCode::unsupported_version,
                                       "session database uses a newer storage version"));
  }
  if (version == storage_format_version) return {};
  if (version != 0) {
    return std::unexpected(store_error(SessionStoreErrorCode::unsupported_version,
                                       "session database version is unsupported"));
  }

  auto schema_count = prepare(
      database,
      "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'");
  if (!schema_count) return std::unexpected(std::move(schema_count.error()));
  if (sqlite3_step(schema_count->get()) != SQLITE_ROW ||
      sqlite3_column_int64(schema_count->get(), 0) != 0) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "unversioned session database is not empty"));
  }

  auto begun = begin_immediate(database);
  if (!begun) return begun;
  const auto schema = execute(
      database,
      "CREATE TABLE sessions("
      "session_id TEXT PRIMARY KEY NOT NULL,"
      "created_at_ms INTEGER NOT NULL,"
      "storage_format_version INTEGER NOT NULL CHECK(storage_format_version=1)"
      ") STRICT;"
      "CREATE TABLE events("
      "session_id TEXT NOT NULL REFERENCES sessions(session_id),"
      "sequence INTEGER NOT NULL CHECK(sequence>0),"
      "event_id TEXT NOT NULL,"
      "run_id TEXT NOT NULL,"
      "schema_version INTEGER NOT NULL CHECK(schema_version>0),"
      "timestamp_ms INTEGER NOT NULL,"
      "caused_by_event_id TEXT,"
      "parent_run_id TEXT,"
      "invocation_id TEXT,"
      "payload_type TEXT NOT NULL,"
      "payload_json TEXT NOT NULL CHECK(json_valid(payload_json)),"
      "PRIMARY KEY(session_id,sequence),"
      "UNIQUE(session_id,event_id)"
      ") STRICT;"
      "CREATE INDEX events_session_timestamp "
      "ON events(session_id,timestamp_ms);"
      "PRAGMA user_version=1;");
  if (!schema) {
    rollback(database);
    return schema;
  }
  return commit(database);
}

[[nodiscard]] auto session_info_from_row(sqlite3_stmt* statement)
    -> std::expected<storage::SessionInfo, SessionStoreError> {
  auto session_id = column_id<domain::SessionId>(statement, 0);
  if (!session_id) return std::unexpected(std::move(session_id.error()));
  const auto created = sqlite3_column_int64(statement, 1);
  const auto activity = sqlite3_column_int64(statement, 2);
  const auto sequence = sqlite3_column_int64(statement, 3);
  if (sequence < 0) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "session sequence is invalid"));
  }
  return storage::SessionInfo{std::move(*session_id), timestamp_from_count(created),
                              timestamp_from_count(activity),
                              static_cast<std::uint64_t>(sequence)};
}

constexpr std::string_view session_info_select{
    "SELECT s.session_id,s.created_at_ms,"
    "COALESCE((SELECT e.timestamp_ms FROM events e WHERE e.session_id=s.session_id "
    "ORDER BY e.sequence DESC LIMIT 1),s.created_at_ms),"
    "COALESCE((SELECT e.sequence FROM events e WHERE e.session_id=s.session_id "
    "ORDER BY e.sequence DESC LIMIT 1),0) FROM sessions s"};

}  // namespace

struct SqliteSessionStore::Impl {
  std::filesystem::path path;
  storage::SessionStoreLimits limits;
  sqlite3* database{};
  std::mutex mutex;

  ~Impl() {
    if (database != nullptr) static_cast<void>(sqlite3_close(database));
  }
};

auto resolve_session_store_path(const SessionStorePathEnvironment& environment)
    -> std::expected<std::filesystem::path, storage::SessionStoreError> {
  if (environment.xdg_state_home && environment.xdg_state_home->is_absolute()) {
    return (*environment.xdg_state_home / "aiforge" / "sessions.sqlite3")
        .lexically_normal();
  }
  if (!environment.home || !environment.home->is_absolute()) {
    return std::unexpected(store_error(
        SessionStoreErrorCode::invalid_argument,
        "an absolute HOME is required when XDG_STATE_HOME is unavailable"));
  }
  return (*environment.home / ".local" / "state" / "aiforge" /
          "sessions.sqlite3")
      .lexically_normal();
}

auto process_session_store_path()
    -> std::expected<std::filesystem::path, storage::SessionStoreError> {
  SessionStorePathEnvironment environment;
  if (const auto* value = std::getenv("XDG_STATE_HOME")) {
    environment.xdg_state_home = std::filesystem::path{value};
  }
  if (const auto* value = std::getenv("HOME")) {
    environment.home = std::filesystem::path{value};
  }
  return resolve_session_store_path(environment);
}

SqliteSessionStore::SqliteSessionStore(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}

SqliteSessionStore::~SqliteSessionStore() = default;

auto SqliteSessionStore::open(std::filesystem::path path,
                              const storage::SessionStoreLimits limits)
    -> std::expected<std::unique_ptr<SqliteSessionStore>,
                     storage::SessionStoreError> {
  try {
    if (!valid_limits(limits) || !path.is_absolute() || path.filename().empty() ||
        path.lexically_normal() != path) {
      return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                         "session-store path or limits are invalid"));
    }
    auto directory = check_directory(path.parent_path(), true);
    if (!directory) return std::unexpected(std::move(directory.error()));
    auto existing = check_regular_secure_file(path);
    if (!existing) return std::unexpected(std::move(existing.error()));
    for (const auto suffix : {std::string_view{"-journal"},
                              std::string_view{"-wal"},
                              std::string_view{"-shm"}}) {
      auto sidecar = std::filesystem::path{path.string() + std::string{suffix}};
      auto checked = check_regular_secure_file(sidecar);
      if (!checked) return std::unexpected(std::move(checked.error()));
    }

    const bool created = !*existing;
    if (created) {
      auto created_file = precreate_database(path);
      if (!created_file) return std::unexpected(std::move(created_file.error()));
    }

    sqlite3* database{};
    const auto open_result = sqlite3_open_v2(
        path.c_str(), &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
            SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (open_result != SQLITE_OK) {
      if (database != nullptr) static_cast<void>(sqlite3_close(database));
      if (created) static_cast<void>(::unlink(path.c_str()));
      return std::unexpected(sqlite_error(open_result));
    }
    auto impl = std::make_unique<Impl>();
    impl->path = std::move(path);
    impl->limits = limits;
    impl->database = database;
    static_cast<void>(sqlite3_extended_result_codes(database, 1));
    const auto busy = sqlite3_busy_timeout(
        database, static_cast<int>(limits.busy_timeout.count()));
    if (busy != SQLITE_OK) return std::unexpected(sqlite_error(busy));

    for (const auto pragma : {std::string_view{"PRAGMA foreign_keys=ON"},
                              std::string_view{"PRAGMA synchronous=FULL"}}) {
      auto configured = execute(database, pragma);
      if (!configured) return std::unexpected(std::move(configured.error()));
    }
    auto journal = prepare(database, "PRAGMA journal_mode=DELETE");
    if (!journal) return std::unexpected(std::move(journal.error()));
    if (sqlite3_step(journal->get()) != SQLITE_ROW) {
      return std::unexpected(store_error(SessionStoreErrorCode::io_failure,
                                         "could not select rollback-journal mode"));
    }
    auto journal_name = column_text(journal->get(), 0);
    if (!journal_name || *journal_name != "delete") {
      return std::unexpected(store_error(SessionStoreErrorCode::io_failure,
                                         "rollback-journal mode is unavailable"));
    }
    journal->reset();
    auto migrated = migrate(database);
    if (!migrated) return std::unexpected(std::move(migrated.error()));
    if (created) {
      auto synced = sync_directory(impl->path.parent_path());
      if (!synced) return std::unexpected(std::move(synced.error()));
    }
    return std::unique_ptr<SqliteSessionStore>{
        new SqliteSessionStore{std::move(impl)}};
  } catch (...) {
    return std::unexpected(store_error(SessionStoreErrorCode::internal_failure,
                                       "session store could not be initialized"));
  }
}

auto SqliteSessionStore::path() const noexcept -> const std::filesystem::path& {
  return m_impl->path;
}

auto SqliteSessionStore::create_session(storage::SessionCreate session,
                                        const std::stop_token stop_token)
    -> std::expected<void, storage::SessionStoreError> {
  try {
    if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
    auto created_at = timestamp_count(session.created_at);
    if (!created_at) return std::unexpected(std::move(created_at.error()));
    std::lock_guard lock(m_impl->mutex);
    auto begun = begin_immediate(m_impl->database);
    if (!begun) return begun;
    Transaction transaction{m_impl->database};
    auto statement = prepare(
        m_impl->database,
        "INSERT INTO sessions(session_id,created_at_ms,storage_format_version) "
        "VALUES(?1,?2,1)");
    if (!statement) {
      return std::unexpected(std::move(statement.error()));
    }
    auto bound = bind_text(statement->get(), 1, session.session_id.value());
    if (bound) {
      const auto result = sqlite3_bind_int64(statement->get(), 2, *created_at);
      if (result != SQLITE_OK) bound = std::unexpected(sqlite_error(result));
    }
    if (!bound) {
      return bound;
    }
    auto stepped = step_done(statement->get());
    if (!stepped) {
      if (stepped.error().code == SessionStoreErrorCode::conflict) {
        return std::unexpected(store_error(SessionStoreErrorCode::already_exists,
                                           "session already exists"));
      }
      return stepped;
    }
    if (stop_token.stop_requested()) {
      return std::unexpected(cancelled_error());
    }
    auto committed = commit(m_impl->database);
    if (committed) transaction.complete();
    return committed;
  } catch (...) {
    return std::unexpected(store_error(SessionStoreErrorCode::internal_failure,
                                       "session creation failed internally"));
  }
}

auto SqliteSessionStore::open_session(
    const domain::SessionId& session_id, const std::stop_token stop_token)
    -> std::expected<storage::SessionInfo, storage::SessionStoreError> {
  try {
    if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
    std::lock_guard lock(m_impl->mutex);
    auto statement = prepare(m_impl->database,
                             std::string{session_info_select} +
                                 " WHERE s.session_id=?1");
    if (!statement) return std::unexpected(std::move(statement.error()));
    auto bound = bind_text(statement->get(), 1, session_id.value());
    if (!bound) return std::unexpected(std::move(bound.error()));
    const auto result = sqlite3_step(statement->get());
    if (result == SQLITE_DONE) {
      return std::unexpected(store_error(SessionStoreErrorCode::not_found,
                                         "session was not found"));
    }
    if (result != SQLITE_ROW) return std::unexpected(sqlite_error(result));
    return session_info_from_row(statement->get());
  } catch (...) {
    return std::unexpected(store_error(SessionStoreErrorCode::internal_failure,
                                       "session lookup failed internally"));
  }
}

auto SqliteSessionStore::list_sessions(
    const std::size_t limit, const std::stop_token stop_token)
    -> std::expected<std::vector<storage::SessionInfo>,
                     storage::SessionStoreError> {
  try {
    if (limit == 0 || limit > 1000) {
      return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                         "session-list limit must be between 1 and 1000"));
    }
    if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
    std::lock_guard lock(m_impl->mutex);
    auto statement = prepare(
        m_impl->database,
        std::string{session_info_select} +
            " ORDER BY 3 DESC,s.session_id ASC LIMIT ?1");
    if (!statement) return std::unexpected(std::move(statement.error()));
    const auto bound = sqlite3_bind_int64(
        statement->get(), 1, static_cast<sqlite3_int64>(limit));
    if (bound != SQLITE_OK) return std::unexpected(sqlite_error(bound));
    std::vector<storage::SessionInfo> sessions;
    while (true) {
      if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
      const auto result = sqlite3_step(statement->get());
      if (result == SQLITE_DONE) break;
      if (result != SQLITE_ROW) return std::unexpected(sqlite_error(result));
      auto info = session_info_from_row(statement->get());
      if (!info) return std::unexpected(std::move(info.error()));
      sessions.push_back(std::move(*info));
    }
    return sessions;
  } catch (...) {
    return std::unexpected(store_error(SessionStoreErrorCode::internal_failure,
                                       "session listing failed internally"));
  }
}

auto SqliteSessionStore::append_events(
    const domain::SessionId& session_id,
    const std::span<const domain::RunEvent> events,
    const std::stop_token stop_token)
    -> std::expected<void, storage::SessionStoreError> {
  try {
    if (events.empty() || events.size() > m_impl->limits.maximum_batch_events) {
      return std::unexpected(store_error(
          SessionStoreErrorCode::invalid_argument,
          "event batch must be nonempty and within the configured limit"));
    }
    if (stop_token.stop_requested()) return std::unexpected(cancelled_error());

    std::vector<EncodedPayload> encoded;
    encoded.reserve(events.size());
    std::set<domain::EventId> event_ids;
    std::uint64_t previous_sequence{};
    for (const auto& event : events) {
      if (event.metadata.sequence == 0 || event.metadata.schema_version == 0 ||
          event.metadata.sequence >
              static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
          (!encoded.empty() && event.metadata.sequence <= previous_sequence) ||
          !event_ids.insert(event.metadata.event_id).second) {
        return std::unexpected(store_error(SessionStoreErrorCode::invalid_argument,
                                           "event batch envelope is invalid"));
      }
      auto payload = encode_payload(event);
      if (!payload) return std::unexpected(std::move(payload.error()));
      if (payload->document.size() > m_impl->limits.maximum_payload_bytes) {
        return std::unexpected(store_error(SessionStoreErrorCode::resource_exhausted,
                                           "event payload exceeds the configured limit"));
      }
      auto timestamp = timestamp_count(event.metadata.timestamp);
      if (!timestamp) return std::unexpected(std::move(timestamp.error()));
      encoded.push_back(std::move(*payload));
      previous_sequence = event.metadata.sequence;
    }

    std::lock_guard lock(m_impl->mutex);
    auto begun = begin_immediate(m_impl->database);
    if (!begun) return begun;
    Transaction transaction{m_impl->database};
    const auto fail = [&](SessionStoreError error) {
      return std::expected<void, SessionStoreError>{std::unexpected(std::move(error))};
    };

    auto current = prepare(
        m_impl->database,
        "SELECT COALESCE((SELECT MAX(sequence) FROM events WHERE session_id=?1),0) "
        "FROM sessions WHERE session_id=?1");
    if (!current) return fail(std::move(current.error()));
    auto current_bound = bind_text(current->get(), 1, session_id.value());
    if (!current_bound) return fail(std::move(current_bound.error()));
    const auto current_step = sqlite3_step(current->get());
    if (current_step != SQLITE_ROW) {
      if (current_step == SQLITE_DONE) {
        return fail(store_error(SessionStoreErrorCode::not_found,
                                "session was not found"));
      }
      return fail(sqlite_error(current_step));
    }
    const auto stored_sequence = sqlite3_column_int64(current->get(), 0);
    if (stored_sequence < 0) {
      return fail(store_error(SessionStoreErrorCode::corrupt,
                              "stored session sequence is invalid"));
    }
    if (events.front().metadata.sequence <=
        static_cast<std::uint64_t>(stored_sequence)) {
      return fail(store_error(SessionStoreErrorCode::conflict,
                              "event sequence does not advance the session"));
    }

    auto insert = prepare(
        m_impl->database,
        "INSERT INTO events(session_id,sequence,event_id,run_id,schema_version,"
        "timestamp_ms,caused_by_event_id,parent_run_id,invocation_id,payload_type,"
        "payload_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
    if (!insert) return fail(std::move(insert.error()));
    for (std::size_t index = 0; index < events.size(); ++index) {
      if (stop_token.stop_requested()) return fail(cancelled_error());
      const auto& event = events[index];
      const auto& payload = encoded[index];
      static_cast<void>(sqlite3_reset(insert->get()));
      static_cast<void>(sqlite3_clear_bindings(insert->get()));
      auto bound = bind_text(insert->get(), 1, session_id.value());
      const auto bind_integer = [&](const int position, const sqlite3_int64 value)
          -> std::expected<void, SessionStoreError> {
        const auto result = sqlite3_bind_int64(insert->get(), position, value);
        if (result != SQLITE_OK) return std::unexpected(sqlite_error(result));
        return {};
      };
      if (bound) {
        bound = bind_integer(2, static_cast<sqlite3_int64>(event.metadata.sequence));
      }
      if (bound) bound = bind_text(insert->get(), 3, event.metadata.event_id.value());
      if (bound) bound = bind_text(insert->get(), 4, event.metadata.run_id.value());
      if (bound) bound = bind_integer(5, event.metadata.schema_version);
      if (bound) {
        auto timestamp = timestamp_count(event.metadata.timestamp);
        if (!timestamp) return fail(std::move(timestamp.error()));
        bound = bind_integer(6, *timestamp);
      }
      if (bound) {
        bound = bind_optional_id(insert->get(), 7,
                                 event.metadata.caused_by_event_id);
      }
      if (bound) {
        bound = bind_optional_id(insert->get(), 8, event.metadata.parent_run_id);
      }
      if (bound) {
        bound = bind_optional_id(insert->get(), 9, event.metadata.invocation_id);
      }
      if (bound) bound = bind_text(insert->get(), 10, payload.type);
      if (bound) bound = bind_text(insert->get(), 11, payload.document);
      if (!bound) return fail(std::move(bound.error()));
      auto inserted = step_done(insert->get());
      if (!inserted) return fail(std::move(inserted.error()));
    }
    if (stop_token.stop_requested()) return fail(cancelled_error());
    auto committed = commit(m_impl->database);
    if (committed) transaction.complete();
    return committed;
  } catch (...) {
    return std::unexpected(store_error(SessionStoreErrorCode::internal_failure,
                                       "event append failed internally"));
  }
}

auto SqliteSessionStore::replay_events(
    const domain::SessionId& session_id, const std::stop_token stop_token)
    -> std::expected<std::vector<domain::RunEvent>,
                     storage::SessionStoreError> {
  try {
    if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
    std::lock_guard lock(m_impl->mutex);
    auto begun = execute(m_impl->database, "BEGIN");
    if (!begun) return std::unexpected(std::move(begun.error()));
    Transaction transaction{m_impl->database};
    const auto fail = [&](SessionStoreError error) {
      return std::expected<std::vector<domain::RunEvent>, SessionStoreError>{
          std::unexpected(std::move(error))};
    };

    auto exists = prepare(m_impl->database,
                          "SELECT 1 FROM sessions WHERE session_id=?1");
    if (!exists) return fail(std::move(exists.error()));
    auto exists_bound = bind_text(exists->get(), 1, session_id.value());
    if (!exists_bound) return fail(std::move(exists_bound.error()));
    const auto exists_step = sqlite3_step(exists->get());
    if (exists_step == SQLITE_DONE) {
      return fail(store_error(SessionStoreErrorCode::not_found,
                              "session was not found"));
    }
    if (exists_step != SQLITE_ROW) return fail(sqlite_error(exists_step));

    auto statement = prepare(
        m_impl->database,
        "SELECT sequence,event_id,run_id,schema_version,timestamp_ms,"
        "caused_by_event_id,parent_run_id,invocation_id,payload_type,payload_json "
        "FROM events WHERE session_id=?1 ORDER BY sequence ASC");
    if (!statement) return fail(std::move(statement.error()));
    auto bound = bind_text(statement->get(), 1, session_id.value());
    if (!bound) return fail(std::move(bound.error()));

    std::vector<domain::RunEvent> events;
    std::set<domain::EventId> event_ids;
    std::uint64_t previous_sequence{};
    std::size_t replay_bytes{};
    while (true) {
      if (stop_token.stop_requested()) return fail(cancelled_error());
      const auto result = sqlite3_step(statement->get());
      if (result == SQLITE_DONE) break;
      if (result != SQLITE_ROW) return fail(sqlite_error(result));
      if (events.size() >= m_impl->limits.maximum_replay_events) {
        return fail(store_error(SessionStoreErrorCode::resource_exhausted,
                                "session replay exceeds the event limit"));
      }
      const auto sequence_value = sqlite3_column_int64(statement->get(), 0);
      const auto schema_value = sqlite3_column_int64(statement->get(), 3);
      if (sequence_value <= 0 || schema_value <= 0 ||
          schema_value > std::numeric_limits<std::uint32_t>::max()) {
        return fail(store_error(SessionStoreErrorCode::corrupt,
                                "persisted event envelope is invalid"));
      }
      const auto sequence = static_cast<std::uint64_t>(sequence_value);
      if (sequence <= previous_sequence) {
        return fail(store_error(SessionStoreErrorCode::corrupt,
                                "persisted event sequence does not increase"));
      }
      auto event_id = column_id<domain::EventId>(statement->get(), 1);
      auto run_id = column_id<domain::RunId>(statement->get(), 2);
      auto caused = column_optional_id<domain::EventId>(statement->get(), 5);
      auto parent = column_optional_id<domain::RunId>(statement->get(), 6);
      auto invocation = column_optional_id<domain::InvocationId>(statement->get(), 7);
      auto type = column_text(statement->get(), 8);
      auto document = column_text(statement->get(), 9);
      if (!event_id) return fail(std::move(event_id.error()));
      if (!run_id) return fail(std::move(run_id.error()));
      if (!caused) return fail(std::move(caused.error()));
      if (!parent) return fail(std::move(parent.error()));
      if (!invocation) return fail(std::move(invocation.error()));
      if (!type) return fail(std::move(type.error()));
      if (!document) return fail(std::move(document.error()));
      if (!event_ids.insert(*event_id).second || type->empty() ||
          type->size() > 256 || has_control_character(*type)) {
        return fail(store_error(SessionStoreErrorCode::corrupt,
                                "persisted event identity or type is invalid"));
      }
      if (document->size() > m_impl->limits.maximum_payload_bytes ||
          document->size() > m_impl->limits.maximum_replay_bytes - replay_bytes) {
        return fail(store_error(SessionStoreErrorCode::resource_exhausted,
                                "session replay exceeds the byte limit"));
      }
      replay_bytes += document->size();
      auto parsed = parse_json_document(*document);
      if (!parsed) return fail(std::move(parsed.error()));

      domain::RunEventPayload payload = domain::UnknownEvent{"uninitialized"};
      const auto schema_version = static_cast<std::uint32_t>(schema_value);
      if (schema_version != 1 || !known_payload_type(*type)) {
        payload = domain::UnknownEvent{
            *type, domain::StructuredDataBlock{"application/json", *document}};
      } else {
        try {
          auto decoded = parse_payload(*type, *parsed);
          if (payload_type(decoded) != *type ||
              payload_json(decoded).dump() != *document) {
            return fail(store_error(
                SessionStoreErrorCode::corrupt,
                "known event payload does not match its declared schema"));
          }
          payload = std::move(decoded);
        } catch (const CodecFailure& error) {
          return fail(store_error(SessionStoreErrorCode::corrupt,
                                  std::string{"known event payload "} + *type +
                                      " is invalid: " + error.what()));
        } catch (const Json::exception&) {
          return fail(store_error(SessionStoreErrorCode::corrupt,
                                  "known event payload has invalid fields"));
        }
      }
      events.push_back(domain::RunEvent{
          domain::EventMetadata{
              std::move(*event_id), std::move(*run_id), sequence,
              schema_version,
              timestamp_from_count(sqlite3_column_int64(statement->get(), 4)),
              std::move(*caused), std::move(*parent), std::move(*invocation)},
          std::move(payload)});
      previous_sequence = sequence;
    }
    auto committed = execute(m_impl->database, "COMMIT");
    if (!committed) return fail(std::move(committed.error()));
    transaction.complete();
    return events;
  } catch (...) {
    return std::unexpected(store_error(SessionStoreErrorCode::internal_failure,
                                       "session replay failed internally"));
  }
}

}  // namespace aiforge::adapters
