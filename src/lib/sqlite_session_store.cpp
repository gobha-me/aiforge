#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/domain/plan_projection.hpp>
#include <aiforge/repository/review_receipt.hpp>
#include <aiforge/repository/verification_evidence.hpp>

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
#include <unistd.h>
#include <unordered_set>
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

[[nodiscard]] auto policy_source_name(
    const domain::PolicyDecisionSource value) -> std::string_view {
  switch (value) {
    case domain::PolicyDecisionSource::fallback: return "fallback";
    case domain::PolicyDecisionSource::permission_profile:
      return "permission_profile";
    case domain::PolicyDecisionSource::session_grant: return "session_grant";
    case domain::PolicyDecisionSource::saved_grant: return "saved_grant";
    case domain::PolicyDecisionSource::user_approval: return "user_approval";
  }
  throw CodecFailure{"invalid policy decision source"};
}

[[nodiscard]] auto parse_policy_source(const Json& value)
    -> domain::PolicyDecisionSource {
  const auto name = value.get<std::string>();
  return enum_value<domain::PolicyDecisionSource>(
      name, {{"fallback", domain::PolicyDecisionSource::fallback},
             {"permission_profile",
              domain::PolicyDecisionSource::permission_profile},
             {"session_grant", domain::PolicyDecisionSource::session_grant},
             {"saved_grant", domain::PolicyDecisionSource::saved_grant},
             {"user_approval",
              domain::PolicyDecisionSource::user_approval}});
}

[[nodiscard]] auto spend_ceiling_source_name(
    const domain::SessionSpendCeilingSource value) -> std::string_view {
  switch (value) {
    case domain::SessionSpendCeilingSource::command_line:
      return "command_line";
  }
  throw CodecFailure{"invalid session spend ceiling source"};
}

[[nodiscard]] auto parse_spend_ceiling_source(const Json& value)
    -> domain::SessionSpendCeilingSource {
  const auto name = value.get<std::string>();
  return enum_value<domain::SessionSpendCeilingSource>(
      name,
      {{"command_line", domain::SessionSpendCeilingSource::command_line}});
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

[[nodiscard]] auto approval_lifetime_name(
    const domain::ApprovalGrantLifetime value) -> std::string_view {
  switch (value) {
    case domain::ApprovalGrantLifetime::invocation: return "invocation";
    case domain::ApprovalGrantLifetime::session: return "session";
    case domain::ApprovalGrantLifetime::saved: return "saved";
  }
  throw CodecFailure{"invalid approval grant lifetime"};
}

[[nodiscard]] auto parse_approval_lifetime(const Json& value)
    -> domain::ApprovalGrantLifetime {
  const auto name = value.get<std::string>();
  return enum_value<domain::ApprovalGrantLifetime>(
      name, {{"invocation", domain::ApprovalGrantLifetime::invocation},
             {"session", domain::ApprovalGrantLifetime::session},
             {"saved", domain::ApprovalGrantLifetime::saved}});
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

[[nodiscard]] auto reported_cost_json(const domain::ReportedCost& cost)
    -> Json {
  auto amounts = Json::array();
  for (const auto& amount : cost.amounts()) {
    amounts.push_back({{"unit", std::string{amount.unit()}},
                       {"amount", amount.amount().to_string()}});
  }
  return {{"amounts", std::move(amounts)}};
}

[[nodiscard]] auto parse_reported_cost(const Json& value)
    -> domain::ReportedCost {
  const auto& values = value.at("amounts");
  if (!values.is_array()) throw CodecFailure{"reported cost is invalid"};
  std::vector<domain::MonetaryAmount> amounts;
  amounts.reserve(values.size());
  for (const auto& item : values) {
    auto decimal = domain::DecimalAmount::from(
        item.at("amount").get<std::string>());
    if (!decimal) throw CodecFailure{"reported cost amount is invalid"};
    auto amount = domain::MonetaryAmount::create(
        item.at("unit").get<std::string>(), *decimal);
    if (!amount) throw CodecFailure{"reported cost unit is invalid"};
    amounts.push_back(std::move(*amount));
  }
  auto cost = domain::ReportedCost::create(std::move(amounts));
  if (!cost) throw CodecFailure{"reported cost is invalid"};
  return std::move(*cost);
}

[[nodiscard]] auto session_spend_ceiling_json(
    const domain::SessionSpendCeiling& ceiling) -> Json {
  return {{"unit", "USD"}, {"amount", ceiling.amount().to_string()}};
}

[[nodiscard]] auto parse_session_spend_ceiling(const Json& value)
    -> domain::SessionSpendCeiling {
  if (value.at("unit").get<std::string>() != "USD") {
    throw CodecFailure{"session spend ceiling unit is invalid"};
  }
  auto ceiling =
      domain::SessionSpendCeiling::from(value.at("amount").get<std::string>());
  if (!ceiling) {
    throw CodecFailure{"session spend ceiling amount is invalid"};
  }
  return std::move(*ceiling);
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
                       {"description", optional_string_json(option.description)},
                       {"recommended", option.recommended}});
  }
  Json other = nullptr;
  if (question.other) {
    other = {{"label", question.other->label},
             {"placeholder",
              optional_string_json(question.other->placeholder)},
             {"maximum_bytes", question.other->maximum_bytes}};
  }
  return {{"question_id", id_text(question.question_id)},
          {"prompt", question.prompt},
          {"selection", selection_name(question.selection)},
          {"options", std::move(options)},
          {"required", question.required},
          {"minimum_selections", question.minimum_selections},
          {"maximum_selections",
           question.maximum_selections ? Json(*question.maximum_selections)
                                       : Json(nullptr)},
          {"other", std::move(other)}};
}

[[nodiscard]] auto parse_question(const Json& value)
    -> domain::QuestionDefinition {
  std::vector<domain::QuestionOption> options;
  for (const auto& option : value.at("options")) {
    options.push_back({option.at("option_id").get<std::string>(),
                       option.at("label").get<std::string>(),
                       parse_optional_string(option.at("description")),
                       option.value("recommended", false)});
  }
  if (!value.contains("required")) {
    const bool free_form = value.at("free_form_allowed").get<bool>();
    const bool optional = value.at("answer_optional").get<bool>();
    const auto selection = parse_selection(value.at("selection"));
    return {parse_id<domain::QuestionId>(value.at("question_id")),
            value.at("prompt").get<std::string>(), selection,
            std::move(options), !optional, optional ? 0U : 1U,
            selection == domain::QuestionSelection::one
                ? std::optional<std::size_t>{1}
                : std::optional<std::size_t>{},
            free_form
                ? std::optional<domain::QuestionOtherInput>{
                      domain::QuestionOtherInput{"Other", std::nullopt, 4096}}
                : std::nullopt};
  }
  std::optional<std::size_t> maximum;
  if (!value.at("maximum_selections").is_null()) {
    maximum = value.at("maximum_selections").get<std::size_t>();
  }
  std::optional<domain::QuestionOtherInput> other;
  if (value.contains("other") && !value.at("other").is_null()) {
    const auto& raw = value.at("other");
    other = domain::QuestionOtherInput{
        raw.at("label").get<std::string>(),
        parse_optional_string(raw.at("placeholder")),
        raw.value("maximum_bytes", std::size_t{4096})};
  }
  return {parse_id<domain::QuestionId>(value.at("question_id")),
          value.at("prompt").get<std::string>(),
          parse_selection(value.at("selection")), std::move(options),
          value.at("required").get<bool>(),
          value.at("minimum_selections").get<std::size_t>(), maximum,
          std::move(other)};
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

[[nodiscard]] auto digest_json(const domain::ContentDigest& digest) -> Json {
  return {{"algorithm", digest.algorithm},
          {"value", digest.value},
          {"byte_size", digest.byte_size}};
}

[[nodiscard]] auto parse_digest(const Json& value) -> domain::ContentDigest {
  return {value.at("algorithm").get<std::string>(),
          value.at("value").get<std::string>(),
          value.at("byte_size").get<std::uint64_t>()};
}

[[nodiscard]] auto
optional_decimal_json(const std::optional<domain::DecimalAmount> &value)
    -> Json {
  return value ? Json(value->to_string()) : Json(nullptr);
}

[[nodiscard]] auto parse_optional_decimal(const Json &value)
    -> std::optional<domain::DecimalAmount> {
  if (value.is_null())
    return std::nullopt;
  auto parsed = domain::DecimalAmount::from(value.get<std::string>());
  if (!parsed)
    throw CodecFailure{"pricing amount is invalid"};
  return *parsed;
}

[[nodiscard]] auto price_rate_json(const domain::PriceRate &rate) -> Json {
  return {{"usd", optional_decimal_json(rate.usd)},
          {"diem", optional_decimal_json(rate.diem)}};
}

[[nodiscard]] auto parse_price_rate(const Json &value) -> domain::PriceRate {
  return {parse_optional_decimal(value.at("usd")),
          parse_optional_decimal(value.at("diem"))};
}

[[nodiscard]] auto
optional_price_rate_json(const std::optional<domain::PriceRate> &rate) -> Json {
  return rate ? price_rate_json(*rate) : Json(nullptr);
}

[[nodiscard]] auto parse_optional_price_rate(const Json &value)
    -> std::optional<domain::PriceRate> {
  if (value.is_null())
    return std::nullopt;
  return parse_price_rate(value);
}

[[nodiscard]] auto text_price_tier_json(const domain::TextPriceTier &tier)
    -> Json {
  return {{"input", optional_price_rate_json(tier.input)},
          {"output", optional_price_rate_json(tier.output)},
          {"cache_input", optional_price_rate_json(tier.cache_input)},
          {"cache_write", optional_price_rate_json(tier.cache_write)}};
}

[[nodiscard]] auto parse_text_price_tier(const Json &value)
    -> domain::TextPriceTier {
  return {parse_optional_price_rate(value.at("input")),
          parse_optional_price_rate(value.at("output")),
          parse_optional_price_rate(value.at("cache_input")),
          parse_optional_price_rate(value.at("cache_write"))};
}

[[nodiscard]] auto
pricing_origin_name(const domain::PricingCatalogOrigin origin)
    -> std::string_view {
  switch (origin) {
  case domain::PricingCatalogOrigin::live:
    return "live";
  case domain::PricingCatalogOrigin::fresh_cache:
    return "fresh_cache";
  case domain::PricingCatalogOrigin::stale_cache:
    return "stale_cache";
  }
  throw CodecFailure{"pricing catalog origin is invalid"};
}

[[nodiscard]] auto parse_pricing_origin(const Json &value)
    -> domain::PricingCatalogOrigin {
  const auto text = value.get<std::string>();
  if (text == "live")
    return domain::PricingCatalogOrigin::live;
  if (text == "fresh_cache")
    return domain::PricingCatalogOrigin::fresh_cache;
  if (text == "stale_cache")
    return domain::PricingCatalogOrigin::stale_cache;
  throw CodecFailure{"pricing catalog origin is invalid"};
}

[[nodiscard]] auto
pricing_observation_json(const domain::PricingObservation &observation)
    -> Json {
  Json extended = nullptr;
  if (observation.pricing.extended) {
    extended = text_price_tier_json(*observation.pricing.extended);
  }
  return {
      {"model_id", id_text(observation.model_id)},
      {"source_id", observation.source_id},
      {"source_revision", optional_string_json(observation.source_revision)},
      {"fetched_at_ms", observation.fetched_at.time_since_epoch().count()},
      {"origin", pricing_origin_name(observation.origin)},
      {"basis", "per_million_tokens"},
      {"pricing",
       {{"base", text_price_tier_json(observation.pricing.base)},
        {"extended_threshold_tokens",
         observation.pricing.extended_threshold_tokens
             ? Json(*observation.pricing.extended_threshold_tokens)
             : Json(nullptr)},
        {"extended", std::move(extended)}}},
      {"rate_card_digest", digest_json(observation.rate_card_digest)}};
}

[[nodiscard]] auto parse_pricing_observation(const Json &value)
    -> domain::PricingObservation {
  const auto &pricing = value.at("pricing");
  domain::TextPricing parsed{parse_text_price_tier(pricing.at("base"))};
  if (!pricing.at("extended_threshold_tokens").is_null()) {
    parsed.extended_threshold_tokens =
        pricing.at("extended_threshold_tokens").get<std::uint64_t>();
  }
  if (!pricing.at("extended").is_null()) {
    parsed.extended = parse_text_price_tier(pricing.at("extended"));
  }
  if (value.at("basis").get<std::string>() != "per_million_tokens") {
    throw CodecFailure{"pricing rate basis is invalid"};
  }
  domain::PricingObservation observation{
      parse_id<domain::ModelId>(value.at("model_id")),
      value.at("source_id").get<std::string>(),
      parse_optional_string(value.at("source_revision")),
      std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{
              value.at("fetched_at_ms").get<std::int64_t>()}},
      parse_pricing_origin(value.at("origin")),
      domain::PricingRateBasis::per_million_tokens,
      std::move(parsed),
      parse_digest(value.at("rate_card_digest"))};
  if (!domain::validate_pricing_observation(observation)) {
    throw CodecFailure{"pricing observation is invalid"};
  }
  return observation;
}

[[nodiscard]] auto persona_reference_json(
    const domain::PersonaReference& reference) -> Json {
  return {{"persona_id", id_text(reference.persona_id)},
          {"name", reference.name},
          {"source_location", reference.source_location},
          {"content_digest", digest_json(reference.content_digest)}};
}

[[nodiscard]] auto parse_persona_reference(const Json& value)
    -> domain::PersonaReference {
  domain::PersonaReference reference{
      parse_id<domain::PersonaId>(value.at("persona_id")),
      value.at("name").get<std::string>(),
      value.at("source_location").get<std::string>(),
      parse_digest(value.at("content_digest"))};
  if (!domain::validate_persona_reference(reference)) {
    throw CodecFailure{"persona reference is invalid"};
  }
  return reference;
}

[[nodiscard]] auto optional_persona_reference_json(
    const std::optional<domain::PersonaReference>& reference) -> Json {
  return reference ? persona_reference_json(*reference) : Json(nullptr);
}

[[nodiscard]] auto parse_optional_persona_reference(const Json& value)
    -> std::optional<domain::PersonaReference> {
  if (value.is_null()) return std::nullopt;
  return parse_persona_reference(value);
}

[[nodiscard]] auto persona_action_name(
    const domain::PersonaSelectionAction value) -> std::string_view {
  switch (value) {
    case domain::PersonaSelectionAction::selected: return "selected";
    case domain::PersonaSelectionAction::disabled: return "disabled";
    case domain::PersonaSelectionAction::unknown: break;
  }
  throw CodecFailure{"invalid persona selection action"};
}

[[nodiscard]] auto parse_persona_action(const Json& value)
    -> domain::PersonaSelectionAction {
  return enum_value<domain::PersonaSelectionAction>(
      value.get<std::string>(),
      {{"selected", domain::PersonaSelectionAction::selected},
       {"disabled", domain::PersonaSelectionAction::disabled}});
}

[[nodiscard]] auto persona_source_name(
    const domain::PersonaSelectionSource value) -> std::string_view {
  switch (value) {
    case domain::PersonaSelectionSource::command_line: return "command_line";
    case domain::PersonaSelectionSource::interactive: return "interactive";
    case domain::PersonaSelectionSource::resumed: return "resumed";
    case domain::PersonaSelectionSource::retained: return "retained";
    case domain::PersonaSelectionSource::unknown: break;
  }
  throw CodecFailure{"invalid persona selection source"};
}

[[nodiscard]] auto parse_persona_source(const Json& value)
    -> domain::PersonaSelectionSource {
  return enum_value<domain::PersonaSelectionSource>(
      value.get<std::string>(),
      {{"command_line", domain::PersonaSelectionSource::command_line},
       {"interactive", domain::PersonaSelectionSource::interactive},
       {"resumed", domain::PersonaSelectionSource::resumed},
       {"retained", domain::PersonaSelectionSource::retained}});
}

[[nodiscard]] auto persona_selection_json(
    const domain::PersonaSelection& selection) -> Json {
  return {{"action", persona_action_name(selection.action)},
          {"source", persona_source_name(selection.source)},
          {"persona", optional_persona_reference_json(selection.persona)},
          {"previous_persona",
           optional_persona_reference_json(selection.previous_persona)}};
}

[[nodiscard]] auto parse_persona_selection(const Json& value)
    -> domain::PersonaSelection {
  domain::PersonaSelection selection{
      parse_persona_action(value.at("action")),
      parse_persona_source(value.at("source")),
      parse_optional_persona_reference(value.at("persona")),
      parse_optional_persona_reference(value.at("previous_persona"))};
  if (!domain::validate_persona_selection(selection)) {
    throw CodecFailure{"persona selection is invalid"};
  }
  return selection;
}

[[nodiscard]] auto snapshot_json(
    const domain::RepositorySnapshotIdentity& snapshot) -> Json {
  return {{"repository_id", id_text(snapshot.repository_id)},
          {"fingerprint", digest_json(snapshot.fingerprint)}};
}

[[nodiscard]] auto parse_snapshot(const Json& value)
    -> domain::RepositorySnapshotIdentity {
  return {parse_id<domain::RepositoryId>(value.at("repository_id")),
          parse_digest(value.at("fingerprint"))};
}

[[nodiscard]] auto optional_snapshot_json(
    const std::optional<domain::RepositorySnapshotIdentity>& snapshot) -> Json {
  return snapshot ? snapshot_json(*snapshot) : Json(nullptr);
}

[[nodiscard]] auto parse_optional_snapshot(const Json& value)
    -> std::optional<domain::RepositorySnapshotIdentity> {
  if (value.is_null()) return std::nullopt;
  return parse_snapshot(value);
}

[[nodiscard]] auto plan_decision_name(const domain::PlanDecision value)
    -> std::string_view {
  switch (value) {
    case domain::PlanDecision::approved: return "approved";
    case domain::PlanDecision::revision_requested: return "revision_requested";
    case domain::PlanDecision::rejected: return "rejected";
  }
  throw CodecFailure{"invalid plan decision"};
}

[[nodiscard]] auto parse_plan_decision(const Json& value)
    -> domain::PlanDecision {
  using Decision = domain::PlanDecision;
  return enum_value<Decision>(
      value.get<std::string>(),
      {{"approved", Decision::approved},
       {"revision_requested", Decision::revision_requested},
       {"rejected", Decision::rejected}});
}

[[nodiscard]] auto plan_decision_source_name(
    const domain::PlanDecisionSource value) -> std::string_view {
  switch (value) {
    case domain::PlanDecisionSource::user: return "user";
    case domain::PlanDecisionSource::policy: return "policy";
  }
  throw CodecFailure{"invalid plan decision source"};
}

[[nodiscard]] auto parse_plan_decision_source(const Json& value)
    -> domain::PlanDecisionSource {
  using Source = domain::PlanDecisionSource;
  return enum_value<Source>(value.get<std::string>(),
                            {{"user", Source::user},
                             {"policy", Source::policy}});
}

[[nodiscard]] auto plan_invalidation_trigger_name(
    const domain::PlanInvalidationTrigger value) -> std::string_view {
  switch (value) {
    case domain::PlanInvalidationTrigger::source_snapshot_changed:
      return "source_snapshot_changed";
    case domain::PlanInvalidationTrigger::evidence_changed:
      return "evidence_changed";
  }
  throw CodecFailure{"invalid plan invalidation trigger"};
}

[[nodiscard]] auto parse_plan_invalidation_trigger(const Json& value)
    -> domain::PlanInvalidationTrigger {
  using Trigger = domain::PlanInvalidationTrigger;
  return enum_value<Trigger>(
      value.get<std::string>(),
      {{"source_snapshot_changed", Trigger::source_snapshot_changed},
       {"evidence_changed", Trigger::evidence_changed}});
}

[[nodiscard]] auto plan_evidence_binding_json(
    const domain::PlanEvidenceBinding& binding) -> Json {
  return {{"evidence_id", id_text(binding.evidence_id)},
          {"digest", digest_json(binding.digest)}};
}

[[nodiscard]] auto parse_plan_evidence_binding(const Json& value)
    -> domain::PlanEvidenceBinding {
  return {parse_id<domain::EvidenceId>(value.at("evidence_id")),
          parse_digest(value.at("digest"))};
}

[[nodiscard]] auto plan_resource_intent_json(
    const domain::PlanResourceIntent& intent) -> Json {
  return {{"effect", effect_name(intent.effect)},
          {"kind", intent.kind},
          {"value", intent.value}};
}

[[nodiscard]] auto parse_plan_resource_intent(const Json& value)
    -> domain::PlanResourceIntent {
  return {parse_effect(value.at("effect")),
          value.at("kind").get<std::string>(),
          value.at("value").get<std::string>()};
}

[[nodiscard]] auto
session_task_outcome_name(const domain::SessionTaskOutcome outcome)
    -> std::string_view {
  using Outcome = domain::SessionTaskOutcome;
  switch (outcome) {
  case Outcome::completed:
    return "completed";
  case Outcome::failed:
    return "failed";
  case Outcome::cancelled:
    return "cancelled";
  case Outcome::timed_out:
    return "timed_out";
  case Outcome::budget_exhausted:
    return "budget_exhausted";
  case Outcome::unavailable:
    return "unavailable";
  }
  throw CodecFailure{"invalid session task outcome"};
}

[[nodiscard]] auto parse_session_task_outcome(const Json& value)
    -> domain::SessionTaskOutcome {
  using Outcome = domain::SessionTaskOutcome;
  return enum_value<Outcome>(value.get<std::string>(),
                             {{"completed", Outcome::completed},
                              {"failed", Outcome::failed},
                              {"cancelled", Outcome::cancelled},
                              {"timed_out", Outcome::timed_out},
                              {"budget_exhausted", Outcome::budget_exhausted},
                              {"unavailable", Outcome::unavailable}});
}

template <typename IdType>
[[nodiscard]] auto id_list_json(const std::vector<IdType>& values) -> Json {
  auto result = Json::array();
  for (const auto& value : values)
    result.push_back(id_text(value));
  return result;
}

template <typename IdType>
[[nodiscard]] auto parse_id_list(const Json& values) -> std::vector<IdType> {
  if (!values.is_array())
    throw CodecFailure{"identity list is invalid"};
  std::vector<IdType> result;
  result.reserve(values.size());
  for (const auto& value : values)
    result.push_back(parse_id<IdType>(value));
  return result;
}

[[nodiscard]] auto child_run_budget_json(const domain::ChildRunBudget& budget)
    -> Json {
  return {{"maximum_inferences", budget.maximum_inferences},
          {"maximum_tool_invocations", budget.maximum_tool_invocations},
          {"maximum_input_tokens", budget.maximum_input_tokens},
          {"maximum_output_tokens", budget.maximum_output_tokens},
          {"timeout_ms", budget.timeout.count()}};
}

[[nodiscard]] auto parse_child_run_budget(const Json& value)
    -> domain::ChildRunBudget {
  return {
      value.at("maximum_inferences").get<std::uint32_t>(),
      value.at("maximum_tool_invocations").get<std::uint32_t>(),
      value.at("maximum_input_tokens").get<std::uint64_t>(),
      value.at("maximum_output_tokens").get<std::uint64_t>(),
      std::chrono::milliseconds{value.at("timeout_ms").get<std::int64_t>()}};
}

[[nodiscard]] auto
child_run_context_json(const domain::ChildRunContextBinding& context) -> Json {
  return {{"parcel_id", id_text(context.parcel_id)},
          {"target_snapshot", snapshot_json(context.target_snapshot)},
          {"evidence_ids", id_list_json(context.evidence_ids)},
          {"represented_bytes", context.represented_bytes},
          {"estimated_tokens", context.estimated_tokens}};
}

[[nodiscard]] auto parse_child_run_context(const Json& value)
    -> domain::ChildRunContextBinding {
  return {parse_id<domain::ContextParcelId>(value.at("parcel_id")),
          parse_snapshot(value.at("target_snapshot")),
          parse_id_list<domain::EvidenceId>(value.at("evidence_ids")),
          value.at("represented_bytes").get<std::uint64_t>(),
          value.at("estimated_tokens").get<std::uint64_t>()};
}

[[nodiscard]] auto
child_run_descriptor_json(const domain::ChildRunDescriptor& descriptor,
                          const bool include_attempt)
    -> Json {
  if (!domain::validate_child_run_descriptor(descriptor)) {
    throw CodecFailure{"child-run descriptor is invalid"};
  }
  Json result{{"parent_run_id", id_text(descriptor.parent_run_id)},
              {"plan_id", id_text(descriptor.plan_id)},
              {"revision_id", id_text(descriptor.revision_id)},
              {"task_id", id_text(descriptor.task_id)},
              {"context", child_run_context_json(descriptor.context)},
              {"budget", child_run_budget_json(descriptor.budget)},
              {"effects", effects_json(descriptor.effects)},
              {"capability_scopes", scopes_json(descriptor.capability_scopes)}};
  if (include_attempt) result["attempt"] = descriptor.attempt;
  return result;
}

[[nodiscard]] auto parse_child_run_descriptor(const Json& value)
    -> domain::ChildRunDescriptor {
  domain::ChildRunDescriptor result{
      parse_id<domain::RunId>(value.at("parent_run_id")),
      parse_id<domain::PlanId>(value.at("plan_id")),
      parse_id<domain::PlanRevisionId>(value.at("revision_id")),
      parse_id<domain::PlanTaskId>(value.at("task_id")),
      parse_child_run_context(value.at("context")),
      parse_child_run_budget(value.at("budget")),
      parse_effects(value.at("effects")),
      parse_scopes(value.at("capability_scopes")),
      value.contains("attempt") ? value.at("attempt").get<std::uint32_t>()
                                : 1U};
  if (!domain::validate_child_run_descriptor(result)) {
    throw CodecFailure{"child-run descriptor is invalid"};
  }
  return result;
}

[[nodiscard]] auto
session_task_result_json(const domain::SessionTaskResult& result) -> Json {
  const domain::ChildRunBudget shape_budget{
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), std::chrono::milliseconds{1}};
  if (!domain::validate_session_task_result(result, shape_budget)) {
    throw CodecFailure{"session task result is invalid"};
  }
  return {{"plan_id", id_text(result.plan_id)},
          {"revision_id", id_text(result.revision_id)},
          {"task_id", id_text(result.task_id)},
          {"child_run_id", id_text(result.child_run_id)},
          {"outcome", session_task_outcome_name(result.outcome)},
          {"consumption",
           {{"inference_count", result.consumption.inference_count},
            {"tool_invocation_count", result.consumption.tool_invocation_count},
            {"usage", usage_json(result.consumption.usage)}}},
          {"evidence_ids", id_list_json(result.evidence_ids)},
          {"artifact_ids", id_list_json(result.artifact_ids)},
          {"error",
           result.error ? domain_error_json(*result.error) : Json(nullptr)}};
}

[[nodiscard]] auto parse_session_task_result(const Json& value)
    -> domain::SessionTaskResult {
  const auto& consumption = value.at("consumption");
  std::optional<domain::DomainError> error;
  if (!value.at("error").is_null()) {
    error = parse_domain_error(value.at("error"));
  }
  domain::SessionTaskResult result{
      parse_id<domain::PlanId>(value.at("plan_id")),
      parse_id<domain::PlanRevisionId>(value.at("revision_id")),
      parse_id<domain::PlanTaskId>(value.at("task_id")),
      parse_id<domain::RunId>(value.at("child_run_id")),
      parse_session_task_outcome(value.at("outcome")),
      {consumption.at("inference_count").get<std::uint32_t>(),
       consumption.at("tool_invocation_count").get<std::uint32_t>(),
       parse_usage(consumption.at("usage"))},
      parse_id_list<domain::EvidenceId>(value.at("evidence_ids")),
      parse_id_list<domain::ArtifactId>(value.at("artifact_ids")),
      std::move(error)};
  const domain::ChildRunBudget shape_budget{
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), std::chrono::milliseconds{1}};
  if (!domain::validate_session_task_result(result, shape_budget)) {
    throw CodecFailure{"session task result is invalid"};
  }
  return result;
}

[[nodiscard]] auto plan_task_json(const domain::PlanTask& task) -> Json {
  auto dependencies = Json::array();
  for (const auto& dependency : task.dependency_task_ids) {
    dependencies.push_back(id_text(dependency));
  }
  auto intents = Json::array();
  for (const auto& intent : task.resource_intents) {
    intents.push_back(plan_resource_intent_json(intent));
  }
  return {{"task_id", id_text(task.task_id)},
          {"parent_task_id", optional_id_json(task.parent_task_id)},
          {"dependency_task_ids", std::move(dependencies)},
          {"title", task.title},
          {"acceptance_criteria", task.acceptance_criteria},
          {"intended_effects", effects_json(task.intended_effects)},
          {"resource_intents", std::move(intents)}};
}

[[nodiscard]] auto parse_plan_task(const Json& value) -> domain::PlanTask {
  const auto& raw_dependencies = value.at("dependency_task_ids");
  const auto& raw_criteria = value.at("acceptance_criteria");
  const auto& raw_intents = value.at("resource_intents");
  if (!raw_dependencies.is_array() || !raw_criteria.is_array() ||
      !raw_intents.is_array()) {
    throw CodecFailure{"plan task list is invalid"};
  }
  domain::PlanTask task{
      parse_id<domain::PlanTaskId>(value.at("task_id")),
      parse_optional_id<domain::PlanTaskId>(value.at("parent_task_id")),
      {},
      value.at("title").get<std::string>(),
      raw_criteria.get<std::vector<std::string>>(),
      parse_effects(value.at("intended_effects")),
      {}};
  task.dependency_task_ids.reserve(raw_dependencies.size());
  for (const auto& dependency : raw_dependencies) {
    task.dependency_task_ids.push_back(
        parse_id<domain::PlanTaskId>(dependency));
  }
  task.resource_intents.reserve(raw_intents.size());
  for (const auto& intent : raw_intents) {
    task.resource_intents.push_back(parse_plan_resource_intent(intent));
  }
  return task;
}

[[nodiscard]] auto plan_revision_json(const domain::PlanRevision& revision,
                                      const bool include_evidence)
    -> Json {
  if (!domain::validate_plan_revision(revision)) {
    throw CodecFailure{"plan revision is invalid"};
  }
  auto tasks = Json::array();
  for (const auto& task : revision.tasks) {
    tasks.push_back(plan_task_json(task));
  }
  Json result{{"plan_id", id_text(revision.plan_id)},
              {"revision_id", id_text(revision.revision_id)},
              {"supersedes_revision_id",
               optional_id_json(revision.supersedes_revision_id)},
              {"goal", revision.goal},
              {"source_snapshot",
               optional_snapshot_json(revision.source_snapshot)},
              {"tasks", std::move(tasks)}};
  if (include_evidence) {
    auto evidence = Json::array();
    for (const auto& binding : revision.evidence) {
      evidence.push_back(plan_evidence_binding_json(binding));
    }
    result["evidence"] = std::move(evidence);
  }
  return result;
}

[[nodiscard]] auto parse_plan_revision(const Json& value)
    -> domain::PlanRevision {
  const auto& raw_tasks = value.at("tasks");
  if (!raw_tasks.is_array()) throw CodecFailure{"plan task list is invalid"};
  domain::PlanRevision revision{
      parse_id<domain::PlanId>(value.at("plan_id")),
      parse_id<domain::PlanRevisionId>(value.at("revision_id")),
      parse_optional_id<domain::PlanRevisionId>(
          value.at("supersedes_revision_id")),
      value.at("goal").get<std::string>(),
      parse_optional_snapshot(value.at("source_snapshot")),
      {},
      {}};
  revision.tasks.reserve(raw_tasks.size());
  for (const auto& task : raw_tasks) {
    revision.tasks.push_back(parse_plan_task(task));
  }
  if (value.contains("evidence")) {
    const auto& raw_evidence = value.at("evidence");
    if (!raw_evidence.is_array()) {
      throw CodecFailure{"plan evidence list is invalid"};
    }
    revision.evidence.reserve(raw_evidence.size());
    for (const auto& binding : raw_evidence) {
      revision.evidence.push_back(parse_plan_evidence_binding(binding));
    }
  }
  if (!domain::validate_plan_revision(revision)) {
    throw CodecFailure{"plan revision is invalid"};
  }
  return revision;
}

[[nodiscard]] auto plan_revision_decision_json(
    const domain::PlanRevisionDecision& decision) -> Json {
  if (!domain::validate_plan_decision(decision)) {
    throw CodecFailure{"plan revision decision is invalid"};
  }
  return {{"plan_id", id_text(decision.plan_id)},
          {"revision_id", id_text(decision.revision_id)},
          {"decision", plan_decision_name(decision.decision)},
          {"source", plan_decision_source_name(decision.source)},
          {"reason", optional_string_json(decision.reason)}};
}

[[nodiscard]] auto parse_plan_revision_decision(const Json& value)
    -> domain::PlanRevisionDecision {
  domain::PlanRevisionDecision decision{
      parse_id<domain::PlanId>(value.at("plan_id")),
      parse_id<domain::PlanRevisionId>(value.at("revision_id")),
      parse_plan_decision(value.at("decision")),
      parse_plan_decision_source(value.at("source")),
      parse_optional_string(value.at("reason"))};
  if (!domain::validate_plan_decision(decision)) {
    throw CodecFailure{"plan revision decision is invalid"};
  }
  return decision;
}

[[nodiscard]] auto plan_revision_invalidation_json(
    const domain::PlanRevisionInvalidation& invalidation) -> Json {
  if (!domain::validate_plan_invalidation(invalidation)) {
    throw CodecFailure{"plan revision invalidation is invalid"};
  }
  auto triggers = Json::array();
  for (const auto trigger : invalidation.triggers) {
    triggers.push_back(plan_invalidation_trigger_name(trigger));
  }
  return {{"plan_id", id_text(invalidation.plan_id)},
          {"revision_id", id_text(invalidation.revision_id)},
          {"triggers", std::move(triggers)}};
}

[[nodiscard]] auto parse_plan_revision_invalidation(const Json& value)
    -> domain::PlanRevisionInvalidation {
  const auto& raw_triggers = value.at("triggers");
  if (!raw_triggers.is_array()) {
    throw CodecFailure{"plan invalidation trigger list is invalid"};
  }
  domain::PlanRevisionInvalidation invalidation{
      parse_id<domain::PlanId>(value.at("plan_id")),
      parse_id<domain::PlanRevisionId>(value.at("revision_id")), {}};
  invalidation.triggers.reserve(raw_triggers.size());
  for (const auto& trigger : raw_triggers) {
    invalidation.triggers.push_back(parse_plan_invalidation_trigger(trigger));
  }
  if (!domain::validate_plan_invalidation(invalidation)) {
    throw CodecFailure{"plan revision invalidation is invalid"};
  }
  return invalidation;
}

[[nodiscard]] auto optional_digest_json(
    const std::optional<domain::ContentDigest>& digest) -> Json {
  return digest ? digest_json(*digest) : Json(nullptr);
}

[[nodiscard]] auto parse_optional_digest(const Json& value)
    -> std::optional<domain::ContentDigest> {
  if (value.is_null()) return std::nullopt;
  return parse_digest(value);
}

[[nodiscard]] auto review_evidence_kind_name(
    const domain::ReviewEvidenceKind value) -> std::string_view {
  switch (value) {
    case domain::ReviewEvidenceKind::verification: return "verification";
    case domain::ReviewEvidenceKind::scenario: return "scenario";
  }
  throw CodecFailure{"invalid review evidence kind"};
}

[[nodiscard]] auto parse_review_evidence_kind(const Json& value)
    -> domain::ReviewEvidenceKind {
  return enum_value<domain::ReviewEvidenceKind>(
      value.get<std::string>(),
      {{"verification", domain::ReviewEvidenceKind::verification},
       {"scenario", domain::ReviewEvidenceKind::scenario}});
}

[[nodiscard]] auto review_verdict_name(const domain::ReviewVerdict value)
    -> std::string_view {
  switch (value) {
    case domain::ReviewVerdict::approved: return "approved";
    case domain::ReviewVerdict::changes_requested: return "changes_requested";
    case domain::ReviewVerdict::rejected: return "rejected";
  }
  throw CodecFailure{"invalid review verdict"};
}

[[nodiscard]] auto parse_review_verdict(const Json& value)
    -> domain::ReviewVerdict {
  return enum_value<domain::ReviewVerdict>(
      value.get<std::string>(),
      {{"approved", domain::ReviewVerdict::approved},
       {"changes_requested", domain::ReviewVerdict::changes_requested},
       {"rejected", domain::ReviewVerdict::rejected}});
}

[[nodiscard]] auto review_actor_json(const domain::ReviewActor& actor) -> Json {
  if (!repository::validate_review_actor(actor)) {
    throw CodecFailure{"review actor is invalid"};
  }
  return {{"actor_id", actor.actor_id}, {"display_name", actor.display_name}};
}

[[nodiscard]] auto parse_review_actor(const Json& value)
    -> domain::ReviewActor {
  domain::ReviewActor result{value.at("actor_id").get<std::string>(),
                             value.at("display_name").get<std::string>()};
  if (!repository::validate_review_actor(result)) {
    throw CodecFailure{"review actor is invalid"};
  }
  return result;
}

[[nodiscard]] auto review_candidate_json(
    const domain::ReviewCandidate& candidate) -> Json {
  if (!repository::validate_review_candidate(candidate)) {
    throw CodecFailure{"review candidate is invalid"};
  }
  return {{"snapshot", snapshot_json(candidate.snapshot)},
          {"revision", candidate.revision}};
}

[[nodiscard]] auto parse_review_candidate(const Json& value)
    -> domain::ReviewCandidate {
  domain::ReviewCandidate result{parse_snapshot(value.at("snapshot")),
                                 value.at("revision").get<std::string>()};
  if (!repository::validate_review_candidate(result)) {
    throw CodecFailure{"review candidate is invalid"};
  }
  return result;
}

[[nodiscard]] auto review_artifact_digest_json(
    const domain::ReviewArtifactDigest& artifact) -> Json {
  return {{"artifact_id", id_text(artifact.artifact_id)},
          {"digest", digest_json(artifact.digest)}};
}

[[nodiscard]] auto parse_review_artifact_digest(const Json& value)
    -> domain::ReviewArtifactDigest {
  return {parse_id<domain::ArtifactId>(value.at("artifact_id")),
          parse_digest(value.at("digest"))};
}

[[nodiscard]] auto review_evidence_json(
    const domain::ReviewEvidenceBinding& binding) -> Json {
  if (!repository::validate_review_evidence_binding(binding)) {
    throw CodecFailure{"review evidence binding is invalid"};
  }
  Json artifacts = Json::array();
  for (const auto& artifact : binding.artifacts) {
    artifacts.push_back(review_artifact_digest_json(artifact));
  }
  return {{"requirement_id", id_text(binding.requirement_id)},
          {"kind", review_evidence_kind_name(binding.kind)},
          {"producer_name", binding.producer_name},
          {"producer_version", binding.producer_version},
          {"verification_evidence_id",
           optional_id_json(binding.verification_evidence_id)},
          {"scenario_id", optional_string_json(binding.scenario_id)},
          {"scenario_corpus_version",
           optional_string_json(binding.scenario_corpus_version)},
          {"scenario_application_revision",
           optional_string_json(binding.scenario_application_revision)},
          {"scenario_fake_script_digest",
           optional_digest_json(binding.scenario_fake_script_digest)},
          {"scenario_terminal_capabilities_digest",
           optional_digest_json(binding.scenario_terminal_capabilities_digest)},
          {"result_digest", digest_json(binding.result_digest)},
          {"artifacts", std::move(artifacts)}};
}

[[nodiscard]] auto parse_review_evidence(const Json& value)
    -> domain::ReviewEvidenceBinding {
  std::vector<domain::ReviewArtifactDigest> artifacts;
  for (const auto& artifact : value.at("artifacts")) {
    artifacts.push_back(parse_review_artifact_digest(artifact));
  }
  domain::ReviewEvidenceBinding result{
      parse_id<domain::ReviewRequirementId>(value.at("requirement_id")),
      parse_review_evidence_kind(value.at("kind")),
      value.at("producer_name").get<std::string>(),
      value.at("producer_version").get<std::string>(),
      parse_optional_id<domain::VerificationEvidenceId>(
          value.at("verification_evidence_id")),
      parse_optional_string(value.at("scenario_id")),
      parse_optional_string(value.at("scenario_corpus_version")),
      parse_optional_string(value.at("scenario_application_revision")),
      parse_optional_digest(value.at("scenario_fake_script_digest")),
      parse_optional_digest(
          value.at("scenario_terminal_capabilities_digest")),
      parse_digest(value.at("result_digest")), std::move(artifacts)};
  if (!repository::validate_review_evidence_binding(result)) {
    throw CodecFailure{"review evidence binding is invalid"};
  }
  return result;
}

[[nodiscard]] auto review_draft_json(
    const domain::ReviewReceiptDraft& draft) -> Json {
  if (!repository::validate_review_receipt_draft(draft)) {
    throw CodecFailure{"review receipt draft is invalid"};
  }
  Json evidence = Json::array();
  for (const auto& binding : draft.evidence) {
    evidence.push_back(review_evidence_json(binding));
  }
  return {{"receipt_id", id_text(draft.receipt_id)},
          {"candidate", review_candidate_json(draft.candidate)},
          {"evidence", std::move(evidence)}};
}

[[nodiscard]] auto parse_review_draft(const Json& value)
    -> domain::ReviewReceiptDraft {
  std::vector<domain::ReviewEvidenceBinding> evidence;
  for (const auto& binding : value.at("evidence")) {
    evidence.push_back(parse_review_evidence(binding));
  }
  domain::ReviewReceiptDraft result{
      parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
      parse_review_candidate(value.at("candidate")), std::move(evidence)};
  if (!repository::validate_review_receipt_draft(result)) {
    throw CodecFailure{"review receipt draft is invalid"};
  }
  return result;
}

[[nodiscard]] auto review_finding_json(const domain::ReviewFinding& finding)
    -> Json {
  if (!repository::validate_review_finding(finding)) {
    throw CodecFailure{"review finding is invalid"};
  }
  Json artifacts = Json::array();
  for (const auto& artifact : finding.artifacts) {
    artifacts.push_back(id_text(artifact));
  }
  return {{"finding_id", id_text(finding.finding_id)},
          {"summary", finding.summary},
          {"verification_evidence_id",
           optional_id_json(finding.verification_evidence_id)},
          {"artifacts", std::move(artifacts)}};
}

[[nodiscard]] auto parse_review_finding(const Json& value)
    -> domain::ReviewFinding {
  std::vector<domain::ArtifactId> artifacts;
  for (const auto& artifact : value.at("artifacts")) {
    artifacts.push_back(parse_id<domain::ArtifactId>(artifact));
  }
  domain::ReviewFinding result{
      parse_id<domain::ReviewFindingId>(value.at("finding_id")),
      value.at("summary").get<std::string>(),
      parse_optional_id<domain::VerificationEvidenceId>(
          value.at("verification_evidence_id")),
      std::move(artifacts)};
  if (!repository::validate_review_finding(result)) {
    throw CodecFailure{"review finding is invalid"};
  }
  return result;
}

[[nodiscard]] auto review_override_json(
    const domain::ReviewOverride& value) -> Json {
  if (!repository::validate_review_override(value)) {
    throw CodecFailure{"review override is invalid"};
  }
  return {{"override_id", id_text(value.override_id)},
          {"receipt_id", id_text(value.receipt_id)},
          {"candidate", review_candidate_json(value.candidate)},
          {"actor", review_actor_json(value.actor)},
          {"reason", value.reason}};
}

[[nodiscard]] auto parse_review_override(const Json& value)
    -> domain::ReviewOverride {
  domain::ReviewOverride result{
      parse_id<domain::ReviewOverrideId>(value.at("override_id")),
      parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
      parse_review_candidate(value.at("candidate")),
      parse_review_actor(value.at("actor")),
      value.at("reason").get<std::string>()};
  if (!repository::validate_review_override(result)) {
    throw CodecFailure{"review override is invalid"};
  }
  return result;
}

[[nodiscard]] auto source_json(const domain::RepositorySourceIdentity& source)
    -> Json {
  Json range = nullptr;
  if (source.range) {
    range = {{"begin", source.range->begin}, {"end", source.range->end}};
  }
  return {{"snapshot", snapshot_json(source.snapshot)},
          {"relative_path", source.relative_path},
          {"content_digest", digest_json(source.content_digest)},
          {"range", std::move(range)}};
}

[[nodiscard]] auto parse_source(const Json& value)
    -> domain::RepositorySourceIdentity {
  std::optional<domain::SourceByteRange> range;
  if (!value.at("range").is_null()) {
    range = domain::SourceByteRange{
        value.at("range").at("begin").get<std::uint64_t>(),
        value.at("range").at("end").get<std::uint64_t>()};
  }
  return {parse_snapshot(value.at("snapshot")),
          value.at("relative_path").get<std::string>(),
          parse_digest(value.at("content_digest")), range};
}

[[nodiscard]] auto verification_kind_name(const domain::VerificationKind value)
    -> std::string_view {
  switch (value) {
    case domain::VerificationKind::build: return "build";
    case domain::VerificationKind::test: return "test";
    case domain::VerificationKind::static_analysis: return "static_analysis";
    case domain::VerificationKind::diagnostic: return "diagnostic";
    case domain::VerificationKind::diff: return "diff";
    case domain::VerificationKind::runtime: return "runtime";
    case domain::VerificationKind::unknown: return "unknown";
  }
  throw CodecFailure{"invalid verification kind"};
}

[[nodiscard]] auto parse_verification_kind(const Json& value)
    -> domain::VerificationKind {
  return enum_value<domain::VerificationKind>(
      value.get<std::string>(),
      {{"build", domain::VerificationKind::build},
       {"test", domain::VerificationKind::test},
       {"static_analysis", domain::VerificationKind::static_analysis},
       {"diagnostic", domain::VerificationKind::diagnostic},
       {"diff", domain::VerificationKind::diff},
       {"runtime", domain::VerificationKind::runtime},
       {"unknown", domain::VerificationKind::unknown}});
}

[[nodiscard]] auto verification_outcome_name(
    const domain::VerificationOutcome value) -> std::string_view {
  switch (value) {
    case domain::VerificationOutcome::passed: return "passed";
    case domain::VerificationOutcome::failed: return "failed";
    case domain::VerificationOutcome::partial: return "partial";
    case domain::VerificationOutcome::cancelled: return "cancelled";
    case domain::VerificationOutcome::timed_out: return "timed_out";
    case domain::VerificationOutcome::unavailable: return "unavailable";
    case domain::VerificationOutcome::unknown: return "unknown";
  }
  throw CodecFailure{"invalid verification outcome"};
}

[[nodiscard]] auto parse_verification_outcome(const Json& value)
    -> domain::VerificationOutcome {
  return enum_value<domain::VerificationOutcome>(
      value.get<std::string>(),
      {{"passed", domain::VerificationOutcome::passed},
       {"failed", domain::VerificationOutcome::failed},
       {"partial", domain::VerificationOutcome::partial},
       {"cancelled", domain::VerificationOutcome::cancelled},
       {"timed_out", domain::VerificationOutcome::timed_out},
       {"unavailable", domain::VerificationOutcome::unavailable},
       {"unknown", domain::VerificationOutcome::unknown}});
}

[[nodiscard]] auto verification_stream_name(
    const domain::VerificationOutputStream value) -> std::string_view {
  switch (value) {
    case domain::VerificationOutputStream::standard_output: return "stdout";
    case domain::VerificationOutputStream::standard_error: return "stderr";
  }
  throw CodecFailure{"invalid verification output stream"};
}

[[nodiscard]] auto parse_verification_stream(const Json& value)
    -> domain::VerificationOutputStream {
  return enum_value<domain::VerificationOutputStream>(
      value.get<std::string>(),
      {{"stdout", domain::VerificationOutputStream::standard_output},
       {"stderr", domain::VerificationOutputStream::standard_error}});
}

[[nodiscard]] auto verification_severity_name(
    const domain::VerificationDiagnosticSeverity value) -> std::string_view {
  switch (value) {
    case domain::VerificationDiagnosticSeverity::note: return "note";
    case domain::VerificationDiagnosticSeverity::warning: return "warning";
    case domain::VerificationDiagnosticSeverity::error: return "error";
    case domain::VerificationDiagnosticSeverity::fatal: return "fatal";
    case domain::VerificationDiagnosticSeverity::unknown: return "unknown";
  }
  throw CodecFailure{"invalid verification diagnostic severity"};
}

[[nodiscard]] auto parse_verification_severity(const Json& value)
    -> domain::VerificationDiagnosticSeverity {
  return enum_value<domain::VerificationDiagnosticSeverity>(
      value.get<std::string>(),
      {{"note", domain::VerificationDiagnosticSeverity::note},
       {"warning", domain::VerificationDiagnosticSeverity::warning},
       {"error", domain::VerificationDiagnosticSeverity::error},
       {"fatal", domain::VerificationDiagnosticSeverity::fatal},
       {"unknown", domain::VerificationDiagnosticSeverity::unknown}});
}

[[nodiscard]] auto verification_json(
    const domain::VerificationEvidence& evidence) -> Json {
  if (!repository::validate_verification_evidence(evidence)) {
    throw CodecFailure{"verification evidence is invalid"};
  }
  Json output = Json::array();
  for (const auto& excerpt : evidence.output) {
    output.push_back(
        {{"stream", verification_stream_name(excerpt.stream)},
         {"text", excerpt.text},
         {"represented_bytes", excerpt.represented_bytes},
         {"truncated", excerpt.truncated},
         {"complete_artifact_id",
          optional_id_json(excerpt.complete_artifact_id)}});
  }
  Json diagnostics = Json::array();
  for (const auto& diagnostic : evidence.diagnostics) {
    diagnostics.push_back(
        {{"severity", verification_severity_name(diagnostic.severity)},
         {"code", diagnostic.code},
         {"message", diagnostic.message},
         {"source", diagnostic.source ? source_json(*diagnostic.source)
                                       : Json(nullptr)}});
  }
  Json artifacts = Json::array();
  for (const auto& artifact : evidence.artifacts) {
    artifacts.push_back(id_text(artifact));
  }
  return {{"evidence_id", id_text(evidence.evidence_id)},
          {"kind", verification_kind_name(evidence.kind)},
          {"extension_name", optional_string_json(evidence.extension_name)},
          {"outcome", verification_outcome_name(evidence.outcome)},
          {"source_snapshot", snapshot_json(evidence.source_snapshot)},
          {"baseline_snapshot",
           optional_snapshot_json(evidence.baseline_snapshot)},
          {"build_configuration",
           optional_digest_json(evidence.build_configuration)},
          {"producer",
           {{"name", evidence.producer.name},
            {"version", evidence.producer.version},
            {"tool_name", evidence.producer.tool_name},
            {"invocation_id", id_text(evidence.producer.invocation_id)}}},
          {"observed_at_ms", evidence.observed_at.time_since_epoch().count()},
          {"summary", evidence.summary},
          {"output", std::move(output)},
          {"diagnostics", std::move(diagnostics)},
          {"artifacts", std::move(artifacts)}};
}

[[nodiscard]] auto parse_verification(const Json& value)
    -> domain::VerificationEvidence {
  std::vector<domain::VerificationOutputExcerpt> output;
  for (const auto& excerpt : value.at("output")) {
    output.push_back({
        parse_verification_stream(excerpt.at("stream")),
        excerpt.at("text").get<std::string>(),
        excerpt.at("represented_bytes").get<std::uint64_t>(),
        excerpt.at("truncated").get<bool>(),
        parse_optional_id<domain::ArtifactId>(
            excerpt.at("complete_artifact_id"))});
  }
  std::vector<domain::VerificationDiagnostic> diagnostics;
  for (const auto& diagnostic : value.at("diagnostics")) {
    std::optional<domain::RepositorySourceIdentity> source;
    if (!diagnostic.at("source").is_null()) {
      source = parse_source(diagnostic.at("source"));
    }
    diagnostics.push_back({
        parse_verification_severity(diagnostic.at("severity")),
        diagnostic.at("code").get<std::string>(),
        diagnostic.at("message").get<std::string>(), std::move(source)});
  }
  std::vector<domain::ArtifactId> artifacts;
  for (const auto& artifact : value.at("artifacts")) {
    artifacts.push_back(parse_id<domain::ArtifactId>(artifact));
  }
  const auto observed_count = value.at("observed_at_ms").get<std::int64_t>();
  domain::VerificationEvidence result{
      parse_id<domain::VerificationEvidenceId>(value.at("evidence_id")),
      parse_verification_kind(value.at("kind")),
      parse_optional_string(value.at("extension_name")),
      parse_verification_outcome(value.at("outcome")),
      parse_snapshot(value.at("source_snapshot")),
      parse_optional_snapshot(value.at("baseline_snapshot")),
      parse_optional_digest(value.at("build_configuration")),
      {value.at("producer").at("name").get<std::string>(),
       value.at("producer").at("version").get<std::string>(),
       value.at("producer").at("tool_name").get<std::string>(),
       parse_id<domain::InvocationId>(
           value.at("producer").at("invocation_id"))},
      std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{observed_count}},
      value.at("summary").get<std::string>(), std::move(output),
      std::move(diagnostics), std::move(artifacts)};
  if (!repository::validate_verification_evidence(result)) {
    throw CodecFailure{"verification evidence is invalid"};
  }
  return result;
}

[[nodiscard]] auto provenance_source_name(const domain::ProvenanceSource value)
    -> std::string_view {
  switch (value) {
    case domain::ProvenanceSource::command_line: return "command_line";
    case domain::ProvenanceSource::environment: return "environment";
    case domain::ProvenanceSource::file: return "file";
    case domain::ProvenanceSource::compiled_default: return "compiled_default";
  }
  throw CodecFailure{"invalid provenance source"};
}

[[nodiscard]] auto parse_provenance_source(const Json& value)
    -> domain::ProvenanceSource {
  const auto name = value.get<std::string>();
  return enum_value<domain::ProvenanceSource>(
      name, {{"command_line", domain::ProvenanceSource::command_line},
             {"environment", domain::ProvenanceSource::environment},
             {"file", domain::ProvenanceSource::file},
             {"compiled_default", domain::ProvenanceSource::compiled_default}});
}

[[nodiscard]] auto provenance_disposition_name(
    const domain::ProvenanceDisposition value) -> std::string_view {
  switch (value) {
    case domain::ProvenanceDisposition::selected: return "selected";
    case domain::ProvenanceDisposition::shadowed: return "shadowed";
    case domain::ProvenanceDisposition::rejected: return "rejected";
  }
  throw CodecFailure{"invalid provenance disposition"};
}

[[nodiscard]] auto parse_provenance_disposition(const Json& value)
    -> domain::ProvenanceDisposition {
  const auto name = value.get<std::string>();
  return enum_value<domain::ProvenanceDisposition>(
      name, {{"selected", domain::ProvenanceDisposition::selected},
             {"shadowed", domain::ProvenanceDisposition::shadowed},
             {"rejected", domain::ProvenanceDisposition::rejected}});
}

[[nodiscard]] auto provenance_diagnostic_name(
    const domain::ProvenanceDiagnosticCode value) -> std::string_view {
  switch (value) {
    case domain::ProvenanceDiagnosticCode::invalid_registry:
      return "invalid_registry";
    case domain::ProvenanceDiagnosticCode::duplicate_key: return "duplicate_key";
    case domain::ProvenanceDiagnosticCode::duplicate_environment_binding:
      return "duplicate_environment_binding";
    case domain::ProvenanceDiagnosticCode::unknown_key: return "unknown_key";
    case domain::ProvenanceDiagnosticCode::invalid_value: return "invalid_value";
    case domain::ProvenanceDiagnosticCode::value_too_large:
      return "value_too_large";
    case domain::ProvenanceDiagnosticCode::too_many_values:
      return "too_many_values";
    case domain::ProvenanceDiagnosticCode::sensitive_value:
      return "sensitive_value";
    case domain::ProvenanceDiagnosticCode::duplicate_source_value:
      return "duplicate_source_value";
    case domain::ProvenanceDiagnosticCode::source_warning:
      return "source_warning";
  }
  throw CodecFailure{"invalid provenance diagnostic code"};
}

[[nodiscard]] auto parse_provenance_diagnostic(const Json& value)
    -> domain::ProvenanceDiagnosticCode {
  using Code = domain::ProvenanceDiagnosticCode;
  const auto name = value.get<std::string>();
  return enum_value<Code>(
      name, {{"invalid_registry", Code::invalid_registry},
             {"duplicate_key", Code::duplicate_key},
             {"duplicate_environment_binding",
              Code::duplicate_environment_binding},
             {"unknown_key", Code::unknown_key},
             {"invalid_value", Code::invalid_value},
             {"value_too_large", Code::value_too_large},
             {"too_many_values", Code::too_many_values},
             {"sensitive_value", Code::sensitive_value},
             {"duplicate_source_value", Code::duplicate_source_value},
             {"source_warning", Code::source_warning}});
}

[[nodiscard]] auto credential_kind_name(const domain::CredentialSourceKind value)
    -> std::string_view {
  switch (value) {
    case domain::CredentialSourceKind::environment: return "environment";
    case domain::CredentialSourceKind::configuration_file:
      return "configuration_file";
    case domain::CredentialSourceKind::unrecognized: return "unrecognized";
  }
  throw CodecFailure{"invalid credential source kind"};
}

[[nodiscard]] auto parse_credential_kind(const Json& value)
    -> domain::CredentialSourceKind {
  using Kind = domain::CredentialSourceKind;
  const auto name = value.get<std::string>();
  return enum_value<Kind>(name,
                          {{"environment", Kind::environment},
                           {"configuration_file", Kind::configuration_file},
                           {"unrecognized", Kind::unrecognized}});
}

[[nodiscard]] auto configuration_entry_json(
    const domain::ConfigurationProvenanceEntry& entry) -> Json {
  auto decisions = Json::array();
  for (const auto& decision : entry.decisions) {
    decisions.push_back(
        {{"source", provenance_source_name(decision.source)},
         {"disposition", provenance_disposition_name(decision.disposition)},
         {"diagnostic_code",
          decision.diagnostic_code
              ? Json(provenance_diagnostic_name(*decision.diagnostic_code))
              : Json(nullptr)}});
  }
  return {{"key", entry.key},
          {"value", entry.value ? Json(*entry.value) : Json(nullptr)},
          {"value_present", entry.value_present},
          {"source", entry.source ? Json(provenance_source_name(*entry.source))
                                  : Json(nullptr)},
          {"sensitive", entry.sensitive},
          {"decisions", std::move(decisions)}};
}

[[nodiscard]] auto parse_configuration_entry(const Json& value)
    -> domain::ConfigurationProvenanceEntry {
  const auto& decisions = value.at("decisions");
  if (!decisions.is_array()) {
    throw CodecFailure{"configuration decision list is invalid"};
  }
  domain::ConfigurationProvenanceEntry entry{};
  entry.key = value.at("key").get<std::string>();
  entry.value = parse_optional_string(value.at("value"));
  entry.value_present = value.at("value_present").get<bool>();
  if (!value.at("source").is_null()) {
    entry.source = parse_provenance_source(value.at("source"));
  }
  entry.sensitive = value.at("sensitive").get<bool>();
  entry.decisions.reserve(decisions.size());
  for (const auto& decision : decisions) {
    domain::ProvenanceDecision parsed{};
    parsed.source = parse_provenance_source(decision.at("source"));
    parsed.disposition =
        parse_provenance_disposition(decision.at("disposition"));
    if (!decision.at("diagnostic_code").is_null()) {
      parsed.diagnostic_code =
          parse_provenance_diagnostic(decision.at("diagnostic_code"));
    }
    entry.decisions.push_back(parsed);
  }
  return entry;
}

[[nodiscard]] auto run_provenance_json(const domain::RunProvenance& provenance)
    -> Json {
  if (!domain::validate_run_provenance(provenance)) {
    throw CodecFailure{"run provenance is invalid"};
  }
  auto configuration = Json::array();
  for (const auto& entry : provenance.configuration) {
    configuration.push_back(configuration_entry_json(entry));
  }
  auto components = Json::array();
  for (const auto& component : provenance.components) {
    components.push_back({{"component", component.component},
                          {"version", component.version}});
  }
  auto tools = Json::array();
  for (const auto& tool : provenance.tools) {
    tools.push_back({{"tool_name", tool.tool_name},
                     {"declared_effects", effects_json(tool.declared_effects)},
                     {"capability_scopes", scopes_json(tool.capability_scopes)}});
  }
  return {{"aiforge_version", provenance.aiforge_version},
          {"backend_id", provenance.backend_id},
          {"backend_version", provenance.backend_version
                                  ? Json(*provenance.backend_version)
                                  : Json(nullptr)},
          {"model_id", id_text(provenance.model_id)},
          {"credential_source",
           provenance.credential_source
               ? Json{{"kind",
                       credential_kind_name(provenance.credential_source->kind)},
                      {"identity", provenance.credential_source->identity}}
               : Json(nullptr)},
          {"configuration", std::move(configuration)},
          {"components", std::move(components)},
          {"tools", std::move(tools)}};
}

[[nodiscard]] auto parse_run_provenance(const Json& value)
    -> domain::RunProvenance {
  const auto& configuration = value.at("configuration");
  const auto& components = value.at("components");
  const auto& tools = value.at("tools");
  if (!configuration.is_array() || !components.is_array() ||
      !tools.is_array()) {
    throw CodecFailure{"run provenance list is invalid"};
  }
  domain::RunProvenance provenance{
      value.at("aiforge_version").get<std::string>(),
      value.at("backend_id").get<std::string>(),
      std::nullopt,
      parse_id<domain::ModelId>(value.at("model_id")),
      std::nullopt,
      {},
      {},
      {}};
  provenance.backend_version = parse_optional_string(value.at("backend_version"));
  if (!value.at("credential_source").is_null()) {
    const auto& source = value.at("credential_source");
    provenance.credential_source =
        domain::CredentialSourceReference{parse_credential_kind(source.at("kind")),
                                          source.at("identity").get<std::string>()};
  }
  provenance.configuration.reserve(configuration.size());
  for (const auto& entry : configuration) {
    provenance.configuration.push_back(parse_configuration_entry(entry));
  }
  provenance.components.reserve(components.size());
  for (const auto& component : components) {
    provenance.components.push_back(
        {component.at("component").get<std::string>(),
         component.at("version").get<std::string>()});
  }
  provenance.tools.reserve(tools.size());
  for (const auto& tool : tools) {
    provenance.tools.push_back({tool.at("tool_name").get<std::string>(),
                                parse_effects(tool.at("declared_effects")),
                                parse_scopes(tool.at("capability_scopes"))});
  }
  if (!domain::validate_run_provenance(provenance)) {
    throw CodecFailure{"run provenance is invalid"};
  }
  return provenance;
}

[[nodiscard]] auto payload_type(const domain::RunEventPayload& payload)
    -> std::string {
  return std::visit(
      Overloaded{
          [](const domain::RunStarted&) { return std::string{"run.started"}; },
          [](const domain::RunProvenanceRecorded&) { return std::string{"run.provenance_recorded"}; },
          [](const domain::PersonaSelectionRecorded&) { return std::string{"persona.selection_recorded"}; },
          [](const domain::SessionSpendCeilingSet&) { return std::string{"session.spend_ceiling_set"}; },
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
          [](const domain::InferencePricingObserved&) {
            return std::string{"inference.pricing_observed"};
          },
          [](const domain::ReasoningMetadataAdded&) { return std::string{"inference.reasoning_metadata_added"}; },
          [](const domain::UsageRecorded&) { return std::string{"inference.usage_recorded"}; },
          [](const domain::InferenceCostRecorded&) {
            return std::string{"inference.cost_recorded"};
          },
          [](const domain::InferenceFinished&) { return std::string{"inference.finished"}; },
          [](const domain::InferenceFailed&) { return std::string{"inference.failed"}; },
          [](const domain::InferenceCancelled&) { return std::string{"inference.cancelled"}; },
          [](const domain::ToolProposed&) { return std::string{"tool.proposed"}; },
          [](const domain::ToolPolicyDecided&) { return std::string{"tool.policy_decided"}; },
          [](const domain::ToolApprovalRequested&) { return std::string{"tool.approval_requested"}; },
          [](const domain::ToolApprovalDecided&) { return std::string{"tool.approval_decided"}; },
          [](const domain::ToolPolicyFailed&) { return std::string{"tool.policy_failed"}; },
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
          [](const domain::VerificationEvidenceRecorded&) {
            return std::string{"verification.evidence_recorded"};
          },
          [](const domain::ReviewReceiptDrafted&) {
            return std::string{"review.receipt_drafted"};
          },
          [](const domain::ReviewRequested&) {
            return std::string{"review.requested"};
          },
          [](const domain::ReviewFindingOpened&) {
            return std::string{"review.finding_opened"};
          },
          [](const domain::ReviewFindingResolved&) {
            return std::string{"review.finding_resolved"};
          },
          [](const domain::ReviewVerdictRecorded&) {
            return std::string{"review.verdict_recorded"};
          },
          [](const domain::ReviewVerdictRevoked&) {
            return std::string{"review.verdict_revoked"};
          },
          [](const domain::ReviewOverrideRecorded&) {
            return std::string{"review.override_recorded"};
          },
          [](const domain::ReviewOverrideRevoked&) {
            return std::string{"review.override_revoked"};
          },
          [](const domain::PlanRevisionProposed&) {
            return std::string{"plan.revision_proposed"};
          },
          [](const domain::PlanRevisionDecisionRecorded&) {
            return std::string{"plan.revision_decision_recorded"};
          },
          [](const domain::PlanRevisionInvalidated&) {
            return std::string{"plan.revision_invalidated"};
          },
          [](const domain::SessionTasksMaterialized&) {
            return std::string{"session.tasks_materialized"};
          },
          [](const domain::ChildRunCreated&) { return std::string{"run.child_created"}; },
          [](const domain::SessionTaskResultRecorded&) {
            return std::string{"session.task_result_recorded"};
          },
          [](const domain::InterRunMessageSent&) { return std::string{"run.inter_message_sent"}; },
          [](const domain::UnknownEvent& value) { return value.type_name; }},
      payload);
}

[[nodiscard]] auto known_payload_type(const std::string_view type) -> bool {
  // A payload added to the variant must also gain a name here and encode and
  // parse paths below. Bump this only alongside those edits.
  static_assert(std::variant_size_v<domain::RunEventPayload> == 56,
                "a new run event payload needs every codec path updated");
  static const std::set<std::string_view> types{
      "run.started", "run.provenance_recorded", "persona.selection_recorded",
      "session.spend_ceiling_set",
      "run.awaiting_input", "run.resumed",
      "run.completion_requested", "run.completed", "run.failed",
      "run.cancel_requested", "run.cancelled", "content.user_added",
      "content.assistant_started", "content.assistant_delta_added",
      "content.assistant_finished", "inference.started",
      "inference.pricing_observed",
      "inference.reasoning_metadata_added", "inference.usage_recorded",
      "inference.cost_recorded",
      "inference.finished", "inference.failed", "inference.cancelled",
      "tool.proposed", "tool.policy_decided", "tool.approval_requested",
      "tool.approval_decided", "tool.policy_failed", "tool.started", "tool.progressed",
      "tool.result_recorded", "tool.errored", "question.requested",
      "question.answered", "question.cancelled", "artifact.created",
      "artifact.referenced", "artifact.displayed",
      "artifact.removed_from_view", "verification.evidence_recorded",
      "review.receipt_drafted", "review.requested",
      "review.finding_opened", "review.finding_resolved",
      "review.verdict_recorded", "review.verdict_revoked",
      "review.override_recorded", "review.override_revoked",
      "plan.revision_proposed", "plan.revision_decision_recorded",
      "plan.revision_invalidated", "session.tasks_materialized",
      "run.child_created",
      "session.task_result_recorded",
      "run.inter_message_sent"};
  return types.contains(type);
}

[[nodiscard]] auto known_payload_schema(const std::string_view type,
                                        const std::uint32_t schema_version)
    -> bool {
  return (schema_version == 1 && known_payload_type(type)) ||
         (schema_version == 2 &&
          (type == "plan.revision_proposed" || type == "run.child_created")) ||
         (schema_version == 3 && type == "run.child_created");
}

[[nodiscard]] auto payload_json(const domain::RunEventPayload& payload,
                                const std::uint32_t schema_version = 1)
    -> Json {
  return std::visit(
      Overloaded{
          [](const domain::RunStarted& value) -> Json {
            return {{"surface_id", id_text(value.surface_id)},
                    {"workspace_id", id_text(value.workspace_id)},
                    {"permission_profile_id", id_text(value.permission_profile_id)},
                    {"persona_id", optional_id_json(value.persona_id)}};
          },
          [](const domain::RunProvenanceRecorded& value) -> Json {
            return {{"provenance", run_provenance_json(value.provenance)}};
          },
          [](const domain::PersonaSelectionRecorded& value) -> Json {
            return {{"selection", persona_selection_json(value.selection)}};
          },
          [](const domain::SessionSpendCeilingSet& value) -> Json {
            return {{"ceiling", session_spend_ceiling_json(value.ceiling)},
                    {"source", spend_ceiling_source_name(value.source)}};
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
          [](const domain::InferencePricingObserved &value) -> Json {
            return {
                {"inference_id", id_text(value.inference_id)},
                {"observation", pricing_observation_json(value.observation)}};
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
          [](const domain::InferenceCostRecorded& value) -> Json {
            return {{"inference_id", id_text(value.inference_id)},
                    {"cost", reported_cost_json(value.cost)}};
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
                    {"declared_effects", effects_json(value.declared_effects)},
                    {"parent_invocation_id",
                     optional_id_json(value.parent_invocation_id)},
                    {"arguments_replayable", value.arguments_replayable},
                    {"validated_required_scopes",
                     scopes_json(value.validated_required_scopes)},
                    {"requested_scopes", scopes_json(value.requested_scopes)},
                    {"result_message_id",
                     optional_id_json(value.result_message_id)}};
          },
          [](const domain::ToolPolicyDecided& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"decision", policy_name(value.decision)},
                    {"scopes", scopes_json(value.scopes)},
                    {"reason", optional_string_json(value.reason)},
                    {"source", policy_source_name(value.source)}};
          },
          [](const domain::ToolApprovalRequested& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"requested_scopes", scopes_json(value.requested_scopes)},
                    {"reason", optional_string_json(value.reason)}};
          },
          [](const domain::ToolApprovalDecided& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"decision", approval_name(value.decision)},
                    {"granted_scopes", scopes_json(value.granted_scopes)},
                    {"lifetime", approval_lifetime_name(value.lifetime)}};
          },
          [](const domain::ToolPolicyFailed& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"error", domain_error_json(value.error)}};
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
                    {"content", content_list_json(value.content)},
                    {"result_message_id",
                     optional_id_json(value.result_message_id)}};
          },
          [](const domain::ToolErrored& value) -> Json {
            return {{"invocation_id", id_text(value.invocation_id)},
                    {"error", domain_error_json(value.error)},
                    {"result_message_id",
                     optional_id_json(value.result_message_id)}};
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
          [](const domain::VerificationEvidenceRecorded& value) -> Json {
            return {{"evidence", verification_json(value.evidence)}};
          },
          [](const domain::ReviewReceiptDrafted& value) -> Json {
            return {{"draft", review_draft_json(value.draft)}};
          },
          [](const domain::ReviewRequested& value) -> Json {
            return {{"receipt_id", id_text(value.receipt_id)},
                    {"requested_by", review_actor_json(value.requested_by)}};
          },
          [](const domain::ReviewFindingOpened& value) -> Json {
            return {{"receipt_id", id_text(value.receipt_id)},
                    {"finding", review_finding_json(value.finding)}};
          },
          [](const domain::ReviewFindingResolved& value) -> Json {
            return {{"receipt_id", id_text(value.receipt_id)},
                    {"finding_id", id_text(value.finding_id)},
                    {"resolved_by", review_actor_json(value.resolved_by)},
                    {"reason", optional_string_json(value.reason)}};
          },
          [](const domain::ReviewVerdictRecorded& value) -> Json {
            return {{"receipt_id", id_text(value.receipt_id)},
                    {"verdict", review_verdict_name(value.verdict)},
                    {"reviewer", review_actor_json(value.reviewer)}};
          },
          [](const domain::ReviewVerdictRevoked& value) -> Json {
            return {{"receipt_id", id_text(value.receipt_id)},
                    {"verdict_event_id", id_text(value.verdict_event_id)},
                    {"revoked_by", review_actor_json(value.revoked_by)},
                    {"reason", value.reason}};
          },
          [](const domain::ReviewOverrideRecorded& value) -> Json {
            return {{"override", review_override_json(value.override)}};
          },
          [](const domain::ReviewOverrideRevoked& value) -> Json {
            return {{"receipt_id", id_text(value.receipt_id)},
                    {"override_id", id_text(value.override_id)},
                    {"revoked_by", review_actor_json(value.revoked_by)},
                    {"reason", value.reason}};
          },
          [schema_version](const domain::PlanRevisionProposed& value) -> Json {
            return {{"revision", plan_revision_json(
                                     value.revision, schema_version >= 2)}};
          },
          [](const domain::PlanRevisionDecisionRecorded& value) -> Json {
            return {{"decision",
                     plan_revision_decision_json(value.decision)}};
          },
          [](const domain::PlanRevisionInvalidated& value) -> Json {
            return {{"invalidation", plan_revision_invalidation_json(
                                         value.invalidation)}};
          },
          [](const domain::SessionTasksMaterialized& value) -> Json {
            return {{"plan_id", id_text(value.plan_id)},
                    {"revision_id", id_text(value.revision_id)}};
          },
          [schema_version](const domain::ChildRunCreated& value) -> Json {
            Json result {{"child_run_id", id_text(value.child_run_id)}};
            if (schema_version >= 2) {
              if (!value.descriptor) {
                throw CodecFailure{
                    "schema-v2 child run lacks a dispatch descriptor"};
              }
              if (schema_version < 3 && value.descriptor->attempt != 1) {
                throw CodecFailure{
                    "schema-v2 child run cannot encode a retry attempt"};
              }
              result["descriptor"] =
                  child_run_descriptor_json(*value.descriptor,
                                            schema_version >= 3);
            }
            return result;
          },
          [](const domain::SessionTaskResultRecorded& value) -> Json {
            return {{"result", session_task_result_json(value.result)}};
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

[[nodiscard]] auto parse_payload(const std::string_view type, const Json& value,
                                 const std::uint32_t schema_version)
    -> domain::RunEventPayload {
  if (type == "run.started") {
    return domain::RunStarted{
        parse_id<domain::SurfaceId>(value.at("surface_id")),
        parse_id<domain::WorkspaceId>(value.at("workspace_id")),
        parse_id<domain::PermissionProfileId>(value.at("permission_profile_id")),
        parse_optional_id<domain::PersonaId>(value.at("persona_id"))};
  }
  if (type == "run.provenance_recorded") {
    return domain::RunProvenanceRecorded{
        parse_run_provenance(value.at("provenance"))};
  }
  if (type == "persona.selection_recorded") {
    return domain::PersonaSelectionRecorded{
        parse_persona_selection(value.at("selection"))};
  }
  if (type == "session.spend_ceiling_set") {
    return domain::SessionSpendCeilingSet{
        parse_session_spend_ceiling(value.at("ceiling")),
        parse_spend_ceiling_source(value.at("source"))};
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
  if (type == "inference.pricing_observed") {
    return domain::InferencePricingObserved{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_pricing_observation(value.at("observation"))};
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
  if (type == "inference.cost_recorded") {
    return domain::InferenceCostRecorded{
        parse_id<domain::InferenceId>(value.at("inference_id")),
        parse_reported_cost(value.at("cost"))};
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
        parse_effects(value.at("declared_effects")),
        value.contains("parent_invocation_id")
            ? parse_optional_id<domain::InvocationId>(
                  value.at("parent_invocation_id"))
            : std::nullopt,
        value.value("arguments_replayable", false),
        value.contains("validated_required_scopes")
            ? parse_scopes(value.at("validated_required_scopes"))
            : std::vector<domain::CapabilityScope>{},
        value.contains("requested_scopes")
            ? parse_scopes(value.at("requested_scopes"))
            : std::vector<domain::CapabilityScope>{},
        value.contains("result_message_id")
            ? parse_optional_id<domain::MessageId>(
                  value.at("result_message_id"))
            : std::nullopt};
  }
  if (type == "tool.policy_decided") {
    return domain::ToolPolicyDecided{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_policy(value.at("decision")), parse_scopes(value.at("scopes")),
        parse_optional_string(value.at("reason")),
        value.contains("source")
            ? parse_policy_source(value.at("source"))
            : domain::PolicyDecisionSource::fallback};
  }
  if (type == "tool.approval_requested") {
    return domain::ToolApprovalRequested{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_scopes(value.at("requested_scopes")),
        value.contains("reason")
            ? parse_optional_string(value.at("reason"))
            : std::nullopt};
  }
  if (type == "tool.approval_decided") {
    return domain::ToolApprovalDecided{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_approval(value.at("decision")),
        parse_scopes(value.at("granted_scopes")),
        value.contains("lifetime")
            ? parse_approval_lifetime(value.at("lifetime"))
            : domain::ApprovalGrantLifetime::invocation};
  }
  if (type == "tool.policy_failed") {
    return domain::ToolPolicyFailed{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_domain_error(value.at("error"))};
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
        parse_content_list(value.at("content")),
        value.contains("result_message_id")
            ? parse_optional_id<domain::MessageId>(
                  value.at("result_message_id"))
            : std::nullopt};
  }
  if (type == "tool.errored") {
    return domain::ToolErrored{
        parse_id<domain::InvocationId>(value.at("invocation_id")),
        parse_domain_error(value.at("error")),
        value.contains("result_message_id")
            ? parse_optional_id<domain::MessageId>(
                  value.at("result_message_id"))
            : std::nullopt};
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
  if (type == "verification.evidence_recorded") {
    return domain::VerificationEvidenceRecorded{
        parse_verification(value.at("evidence"))};
  }
  if (type == "review.receipt_drafted") {
    return domain::ReviewReceiptDrafted{
        parse_review_draft(value.at("draft"))};
  }
  if (type == "review.requested") {
    return domain::ReviewRequested{
        parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
        parse_review_actor(value.at("requested_by"))};
  }
  if (type == "review.finding_opened") {
    return domain::ReviewFindingOpened{
        parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
        parse_review_finding(value.at("finding"))};
  }
  if (type == "review.finding_resolved") {
    return domain::ReviewFindingResolved{
        parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
        parse_id<domain::ReviewFindingId>(value.at("finding_id")),
        parse_review_actor(value.at("resolved_by")),
        parse_optional_string(value.at("reason"))};
  }
  if (type == "review.verdict_recorded") {
    return domain::ReviewVerdictRecorded{
        parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
        parse_review_verdict(value.at("verdict")),
        parse_review_actor(value.at("reviewer"))};
  }
  if (type == "review.verdict_revoked") {
    return domain::ReviewVerdictRevoked{
        parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
        parse_id<domain::EventId>(value.at("verdict_event_id")),
        parse_review_actor(value.at("revoked_by")),
        value.at("reason").get<std::string>()};
  }
  if (type == "review.override_recorded") {
    return domain::ReviewOverrideRecorded{
        parse_review_override(value.at("override"))};
  }
  if (type == "review.override_revoked") {
    return domain::ReviewOverrideRevoked{
        parse_id<domain::ReviewReceiptId>(value.at("receipt_id")),
        parse_id<domain::ReviewOverrideId>(value.at("override_id")),
        parse_review_actor(value.at("revoked_by")),
        value.at("reason").get<std::string>()};
  }
  if (type == "plan.revision_proposed") {
    return domain::PlanRevisionProposed{
        parse_plan_revision(value.at("revision"))};
  }
  if (type == "plan.revision_decision_recorded") {
    return domain::PlanRevisionDecisionRecorded{
        parse_plan_revision_decision(value.at("decision"))};
  }
  if (type == "plan.revision_invalidated") {
    return domain::PlanRevisionInvalidated{
        parse_plan_revision_invalidation(value.at("invalidation"))};
  }
  if (type == "session.tasks_materialized") {
    return domain::SessionTasksMaterialized{
        parse_id<domain::PlanId>(value.at("plan_id")),
        parse_id<domain::PlanRevisionId>(value.at("revision_id"))};
  }
  if (type == "run.child_created") {
    const auto has_descriptor = value.contains("descriptor");
    const auto has_attempt =
        has_descriptor && value.at("descriptor").contains("attempt");
    if ((schema_version == 1 && has_descriptor) ||
        (schema_version == 2 && (!has_descriptor || has_attempt)) ||
        (schema_version == 3 && (!has_descriptor || !has_attempt))) {
      throw CodecFailure{"child-run dispatch does not match its event schema"};
    }
    return domain::ChildRunCreated{
        parse_id<domain::RunId>(value.at("child_run_id")),
        has_descriptor
            ? std::optional{parse_child_run_descriptor(value.at("descriptor"))}
            : std::nullopt};
  }
  if (type == "session.task_result_recorded") {
    return domain::SessionTaskResultRecorded{
        parse_session_task_result(value.at("result"))};
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
    if (!known_payload_schema(type, event.metadata.schema_version) &&
        !std::holds_alternative<domain::UnknownEvent>(event.payload)) {
      return std::unexpected(store_error(
          SessionStoreErrorCode::unsupported_version,
          "known event payload uses an unsupported schema version"));
    }
    if (known_payload_schema(type, event.metadata.schema_version) &&
        std::holds_alternative<domain::UnknownEvent>(event.payload)) {
      return std::unexpected(store_error(
          SessionStoreErrorCode::invalid_argument,
          "known event schema cannot carry an opaque payload"));
    }
    if (event.metadata.schema_version == 1) {
      if (const auto* proposed =
              std::get_if<domain::PlanRevisionProposed>(&event.payload);
          proposed != nullptr && !proposed->revision.evidence.empty()) {
        return std::unexpected(store_error(
            SessionStoreErrorCode::invalid_argument,
            "plan evidence requires proposal event schema version 2"));
      }
      if (const auto* child =
              std::get_if<domain::ChildRunCreated>(&event.payload);
          child != nullptr && child->descriptor) {
        return std::unexpected(store_error(
            SessionStoreErrorCode::invalid_argument,
            "child-run dispatch metadata requires event schema version 2 or 3"));
      }
    }
    return EncodedPayload{
        type, payload_json(event.payload, event.metadata.schema_version).dump()};
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
  const auto run_count = sqlite3_column_int64(statement, 4);
  if (sequence < 0 || run_count < 0) {
    return std::unexpected(store_error(SessionStoreErrorCode::corrupt,
                                       "session summary is invalid"));
  }
  return storage::SessionInfo{std::move(*session_id), timestamp_from_count(created),
                              timestamp_from_count(activity),
                              static_cast<std::uint64_t>(sequence),
                              static_cast<std::uint64_t>(run_count)};
}

constexpr std::string_view session_info_select{
    "SELECT s.session_id,s.created_at_ms,"
    "COALESCE((SELECT e.timestamp_ms FROM events e WHERE "
    "e.session_id=s.session_id "
    "ORDER BY e.sequence DESC LIMIT 1),s.created_at_ms),"
    "COALESCE((SELECT e.sequence FROM events e WHERE e.session_id=s.session_id "
    "ORDER BY e.sequence DESC LIMIT 1),0),"
    "(SELECT COUNT(DISTINCT e.run_id) FROM events e "
    "WHERE e.session_id=s.session_id) FROM sessions s"};

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

auto sqlite_library_version() -> std::string {
  const auto* version = sqlite3_libversion();
  return version == nullptr ? std::string{} : std::string{version};
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
        m_impl->database, "SELECT COALESCE((SELECT MAX(sequence) FROM "
                                  "events WHERE session_id=?1),0) "
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
        "timestamp_ms,caused_by_event_id,parent_run_id,invocation_id,payload_"
        "type,"
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
                "caused_by_event_id,parent_run_id,invocation_id,payload_type,"
                "payload_json "
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
      if (!known_payload_schema(*type, schema_version)) {
        payload = domain::UnknownEvent{
            *type, domain::StructuredDataBlock{"application/json", *document}};
      } else {
        try {
          auto decoded = parse_payload(*type, *parsed, schema_version);
          if (payload_type(decoded) != *type ||
              payload_json(decoded, schema_version).dump() != *document) {
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
