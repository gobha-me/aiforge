#include <aiforge/surfaces/agent.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Json = nlohmann::json;

auto invalid() -> std::unexpected<AgentError> {
  return std::unexpected(AgentError{AgentErrorCode::invalid_request,
                                    "invalid version 1 agent request"});
}

auto valid_framing(std::string_view input) -> bool {
  if (input.empty() || input.size() > agent_maximum_input_bytes) return false;
  if (input.back() == '\n') input.remove_suffix(1);
  if (!input.empty() && input.back() == '\r') input.remove_suffix(1);
  if (input.find_first_of("\r\n") != std::string_view::npos) return false;
  bool quoted{};
  bool escape{};
  unsigned depth{};
  for (const char byte : input) {
    if (quoted) {
      if (escape)
        escape = false;
      else if (byte == '\\')
        escape = true;
      else if (byte == '"')
        quoted = false;
    } else if (byte == '"') {
      quoted = true;
    } else if (byte == '{' || byte == '[') {
      if (++depth > 8) return false;
    } else if (byte == '}' || byte == ']') {
      if (depth == 0) return false;
      --depth;
    }
  }
  return !quoted && depth == 0;
}

auto exact_keys(const Json& value, const std::set<std::string>& allowed)
    -> bool {
  return std::ranges::all_of(value.items(), [&](const auto& item) {
    return allowed.contains(item.key());
  });
}

auto safe_identity(const Json& value) -> bool {
  return value.is_string() && !value.get_ref<const std::string&>().empty() &&
         detail::is_safe_utf8_text(value.get_ref<const std::string&>()) &&
         value.get_ref<const std::string&>().find_first_of("\r\n\t") ==
             std::string::npos;
}

template <class Id>
auto optional_id(const Json& root, const char* key, std::optional<Id>& result)
    -> bool {
  if (!root.contains(key)) return true;
  if (!safe_identity(root.at(key))) return false;
  auto parsed = Id::from(root.at(key).get<std::string>());
  if (!parsed) return false;
  result = std::move(*parsed);
  return true;
}
} // namespace

auto parse_agent_request(const std::string_view input)
    -> std::expected<AgentRequest, AgentError> {
  try {
    if (!valid_framing(input)) return invalid();
    std::vector<std::set<std::string>> keys;
    bool duplicate{};
    const auto callback = [&](int, const Json::parse_event_t event,
                              Json& value) {
      if (event == Json::parse_event_t::object_start)
        keys.emplace_back();
      else if (event == Json::parse_event_t::key) {
        if (keys.empty() ||
            !keys.back().insert(value.get<std::string>()).second)
          duplicate = true;
      } else if (event == Json::parse_event_t::object_end && !keys.empty())
        keys.pop_back();
      return true;
    };
    const auto root = Json::parse(input, callback, true, false);
    if (duplicate || !root.is_object() || !root.contains("version") ||
        !root.at("version").is_number_unsigned() || root.at("version") != 1 ||
        !root.contains("operation") || !root.at("operation").is_string())
      return invalid();
    AgentRequest result;
    if (!optional_id(root, "session_id", result.session_id)) return invalid();
    if (root.at("operation") == "replay") {
      if (!exact_keys(root, {"version", "operation", "session_id"}) ||
          !result.session_id)
        return invalid();
      result.operation = AgentOperation::replay;
      return result;
    }
    if (root.at("operation") != "submit" ||
        !exact_keys(root, {"version", "operation", "session_id", "model",
                           "profile", "tools", "prompt"}) ||
        !optional_id(root, "model", result.model) ||
        !optional_id(root, "profile", result.profile) || !result.profile ||
        !root.contains("prompt") || !root.at("prompt").is_string() ||
        !root.contains("tools") || !root.at("tools").is_array())
      return invalid();
    result.prompt = root.at("prompt").get<std::string>();
    if (result.prompt.empty() || !detail::is_safe_utf8_text(result.prompt))
      return invalid();
    const auto& tools = root.at("tools");
    if (tools.empty() || tools.size() > 2) return invalid();
    for (const auto& tool : tools) {
      if (!tool.is_string() ||
          (tool != "read_repository_file" && tool != "run_process"))
        return invalid();
      auto name = tool.get<std::string>();
      if (std::ranges::find(result.tools, name) != result.tools.end())
        return invalid();
      result.tools.push_back(std::move(name));
    }
    return result;
  } catch (...) {
    return invalid();
  }
}
} // namespace aiforge::surfaces
