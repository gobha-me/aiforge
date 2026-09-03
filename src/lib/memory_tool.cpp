#include <aiforge/runtime/memory_tool.hpp>

#include <chrono>
#include <memory>
#include <ranges>
#include <set>
#include <string_view>

#include <nlohmann/json.hpp>

namespace aiforge::runtime {
namespace {

using Json = nlohmann::json;

[[nodiscard]] auto error(std::string message)
    -> std::unexpected<ToolExecutionError> {
  return std::unexpected(ToolExecutionError{
      ToolExecutionErrorCode::invalid_arguments, std::move(message), false});
}

class DuplicateJsonKey final : public std::exception {};

[[nodiscard]] auto parse_json(const std::string& text) -> Json {
  std::vector<std::set<std::string>> keys;
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
  return Json::parse(text, callback, true, false);
}

[[nodiscard]] auto scope_from(const std::string_view value)
    -> std::optional<domain::MemoryScope> {
  if (value == "global") return domain::MemoryScope::global;
  if (value == "project") return domain::MemoryScope::project;
  return std::nullopt;
}

[[nodiscard]] auto kind_from(const std::string_view value)
    -> std::optional<domain::MemoryKind> {
  if (value == "user_preference") return domain::MemoryKind::user_preference;
  if (value == "project_convention")
    return domain::MemoryKind::project_convention;
  if (value == "workflow") return domain::MemoryKind::workflow;
  if (value == "reusable_fact") return domain::MemoryKind::reusable_fact;
  return std::nullopt;
}

[[nodiscard]] auto valid_text(const std::string& value, const std::size_t limit)
    -> bool {
  return value.size() <= limit && domain::memory_text_is_safe(value) &&
         !domain::memory_text_looks_secret(value);
}

[[nodiscard]] auto parse_draft(const domain::StructuredDataBlock& arguments,
                               const MemoryToolConfiguration& configuration)
    -> std::expected<MemoryProposalDraft, ToolExecutionError> {
  if (arguments.media_type != "application/json" || arguments.data.empty() ||
      arguments.data.size() > std::size_t{64} * 1024U) {
    return error("propose_memory arguments must be bounded JSON");
  }
  try {
    const auto root = parse_json(arguments.data);
    static const std::set<std::string> allowed{"scope",
                                               "kind",
                                               "content",
                                               "rationale",
                                               "evidence_excerpt",
                                               "replacement_record_id",
                                               "overlap_record_ids"};
    if (!root.is_object() || root.size() < 5 ||
        std::ranges::any_of(root.items(), [](const auto& item) {
          return !allowed.contains(item.key());
        })) {
      return error("propose_memory contains unknown or missing fields");
    }
    for (const auto* field :
         {"scope", "kind", "content", "rationale", "evidence_excerpt"}) {
      if (!root.contains(field) || !root.at(field).is_string()) {
        return error("propose_memory required field has the wrong type");
      }
    }
    auto scope = scope_from(root.at("scope").get<std::string>());
    auto kind = kind_from(root.at("kind").get<std::string>());
    if (!scope || !kind ||
        (*scope == domain::MemoryScope::global &&
         !configuration.global_enabled) ||
        (*scope == domain::MemoryScope::project &&
         !configuration.project_enabled) ||
        (*scope == domain::MemoryScope::global &&
         *kind == domain::MemoryKind::project_convention)) {
      return error("propose_memory scope or kind is unavailable");
    }
    MemoryProposalDraft result{*scope,
                               *kind,
                               root.at("content").get<std::string>(),
                               root.at("rationale").get<std::string>(),
                               root.at("evidence_excerpt").get<std::string>(),
                               std::nullopt,
                               {}};
    if (!valid_text(result.content,
                    configuration.limits.maximum_content_bytes) ||
        !valid_text(result.rationale,
                    configuration.limits.maximum_rationale_bytes) ||
        !valid_text(result.evidence_excerpt,
                    configuration.limits.maximum_excerpt_bytes)) {
      return error("propose_memory text is unsafe or oversized");
    }
    if (root.contains("replacement_record_id")) {
      if (!root.at("replacement_record_id").is_string()) {
        return error("propose_memory replacement identity is invalid");
      }
      auto id = domain::MemoryRecordId::from(
          root.at("replacement_record_id").get<std::string>());
      if (!id) return error("propose_memory replacement identity is invalid");
      result.replacement_record_id = std::move(*id);
    }
    if (root.contains("overlap_record_ids")) {
      const auto& raw = root.at("overlap_record_ids");
      if (!raw.is_array() ||
          raw.size() > configuration.limits.maximum_relationships) {
        return error("propose_memory overlap list is invalid");
      }
      std::set<domain::MemoryRecordId> unique;
      for (const auto& value : raw) {
        if (!value.is_string()) {
          return error("propose_memory overlap identity is invalid");
        }
        auto id = domain::MemoryRecordId::from(value.get<std::string>());
        if (!id || !unique.insert(*id).second) {
          return error("propose_memory overlap identities are invalid");
        }
        result.overlap_record_ids.push_back(std::move(*id));
      }
    }
    if (result.replacement_record_id &&
        !std::ranges::contains(result.overlap_record_ids,
                               *result.replacement_record_id)) {
      return error("propose_memory replacement must be listed as an overlap");
    }
    return result;
  } catch (...) {
    return error("propose_memory arguments are malformed");
  }
}

class MemoryToolStream final : public ToolExecutionStream {
 public:
  explicit MemoryToolStream(domain::StructuredDataBlock result)
      : m_result(std::move(result)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<ToolExecutionEvent>,
                       ToolExecutionError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(
          ToolExecutionError{ToolExecutionErrorCode::cancelled,
                             "memory proposal was cancelled", false});
    }
    if (m_emitted) return std::optional<ToolExecutionEvent>{};
    m_emitted = true;
    return std::optional<ToolExecutionEvent>{ToolResult{{m_result}, {}}};
  }

 private:
  domain::StructuredDataBlock m_result;
  bool m_emitted{};
};

class MemoryToolExecutor final : public ToolExecutor {
 public:
  explicit MemoryToolExecutor(MemoryToolConfiguration configuration)
      : m_configuration(std::move(configuration)) {}

  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<ValidatedToolArguments, ToolExecutionError> override {
    auto parsed = parse_draft(arguments, m_configuration);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    return ValidatedToolArguments{arguments, {}, {}};
  }

  auto start(ToolInvocation invocation, std::stop_token)
      -> std::expected<std::unique_ptr<ToolExecutionStream>,
                       ToolExecutionError> override {
    auto parsed = parse_draft(invocation.arguments.value, m_configuration);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    Json result{{"status", "proposed"},
                {"scope", parsed->scope == domain::MemoryScope::global
                              ? "global"
                              : "project"}};
    return std::make_unique<MemoryToolStream>(
        domain::StructuredDataBlock{"application/json", result.dump()});
  }

 private:
  MemoryToolConfiguration m_configuration;
};

} // namespace

auto parse_memory_proposal_draft(const domain::StructuredDataBlock& arguments,
                                 const MemoryToolConfiguration& configuration)
    -> std::expected<MemoryProposalDraft, ToolExecutionError> {
  return parse_draft(arguments, configuration);
}

auto memory_tool_declaration(const MemoryToolConfiguration& configuration)
    -> backend::ToolDeclaration {
  Json scopes = Json::array();
  if (configuration.global_enabled) scopes.push_back("global");
  if (configuration.project_enabled) scopes.push_back("project");
  Json schema{
      {"type", "object"},
      {"additionalProperties", false},
      {"required", Json::array({"scope", "kind", "content", "rationale",
                                "evidence_excerpt"})},
      {"properties",
       {{"scope", {{"enum", scopes}}},
        {"kind",
         {{"enum", Json::array({"user_preference", "project_convention",
                                "workflow", "reusable_fact"})}}},
        {"content",
         {{"type", "string"},
          {"maxLength", configuration.limits.maximum_content_bytes}}},
        {"rationale",
         {{"type", "string"},
          {"maxLength", configuration.limits.maximum_rationale_bytes}}},
        {"evidence_excerpt",
         {{"type", "string"},
          {"maxLength", configuration.limits.maximum_excerpt_bytes}}},
        {"replacement_record_id", {{"type", "string"}, {"maxLength", 128}}},
        {"overlap_record_ids",
         {{"type", "array"},
          {"maxItems", configuration.limits.maximum_relationships},
          {"uniqueItems", true},
          {"items", {{"type", "string"}, {"maxLength", 128}}}}}}}};
  return {"propose_memory",
          "Propose a bounded global or project memory for runtime review. "
          "This proposal grants no authority and may be rejected by policy.",
          {"application/schema+json", schema.dump()},
          {},
          {}};
}

auto register_memory_tool(ToolRegistry& registry,
                          MemoryToolConfiguration configuration)
    -> std::expected<void, ToolRegistryError> {
  if (!configuration.global_enabled && !configuration.project_enabled) {
    return std::unexpected(ToolRegistryError{
        ToolRegistryErrorCode::invalid_declaration,
        "propose_memory requires at least one enabled scope"});
  }
  return registry.register_tool(
      memory_tool_declaration(configuration),
      std::make_shared<MemoryToolExecutor>(configuration),
      ToolExecutionLimits{std::size_t{64} * 1024U, 1, std::chrono::seconds{5}},
      ToolExecutorContract{"aiforge.runtime.propose_memory", "1"});
}

} // namespace aiforge::runtime
