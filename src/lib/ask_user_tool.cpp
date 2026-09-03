#include <aiforge/runtime/ask_user_tool.hpp>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace aiforge::runtime {
namespace {

using Json = nlohmann::json;

[[nodiscard]] auto execution_error(std::string message)
    -> std::unexpected<ToolExecutionError> {
  return std::unexpected(ToolExecutionError{
      ToolExecutionErrorCode::invalid_arguments, std::move(message), false});
}

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto has_only_keys(
    const Json& object, const std::initializer_list<std::string_view> allowed)
    -> bool {
  if (!object.is_object()) return false;
  return std::ranges::all_of(object.items(), [&](const auto& item) {
    return std::ranges::find(allowed, std::string_view{item.key()}) !=
           allowed.end();
  });
}

[[nodiscard]] auto bounded_text(const Json& object, const char* key,
                                const std::size_t maximum,
                                const bool optional = false)
    -> std::expected<std::optional<std::string>, ToolExecutionError> {
  if (!object.contains(key)) {
    if (optional) return std::nullopt;
    return execution_error("ask_user text field is missing");
  }
  if (!object.at(key).is_string()) {
    return execution_error("ask_user text field has the wrong type");
  }
  auto value = object.at(key).get<std::string>();
  if ((!optional && value.empty()) || value.size() > maximum ||
      has_control_character(value)) {
    return execution_error("ask_user text field is invalid or oversized");
  }
  return std::optional<std::string>{std::move(value)};
}

[[nodiscard]] auto parse_questions(const domain::StructuredDataBlock& arguments,
                                   const AskUserLimits& limits)
    -> std::expected<std::vector<domain::QuestionDefinition>,
                     ToolExecutionError> {
  if (arguments.media_type != "application/json" || arguments.data.empty()) {
    return execution_error("ask_user arguments must be JSON");
  }
  try {
    const auto root = Json::parse(arguments.data);
    if (!root.is_object() || !root.contains("questions") ||
        !root.at("questions").is_array() || root.size() != 1 ||
        root.at("questions").empty() ||
        root.at("questions").size() > limits.questions) {
      return execution_error("ask_user question count is invalid");
    }

    std::set<domain::QuestionId> question_ids;
    std::vector<domain::QuestionDefinition> result;
    result.reserve(root.at("questions").size());
    for (const auto& raw_question : root.at("questions")) {
      if (!has_only_keys(raw_question,
                         {"id", "prompt", "kind", "required",
                          "minimum_selections", "maximum_selections", "options",
                          "other"})) {
        return execution_error("ask_user question must be an object");
      }
      auto raw_id =
          bounded_text(raw_question, "id", domain::QuestionId::max_size);
      auto prompt = bounded_text(raw_question, "prompt", limits.prompt_bytes);
      auto kind = bounded_text(raw_question, "kind", 8);
      if (!raw_id || !prompt || !kind) {
        return execution_error("ask_user question fields are invalid");
      }
      auto question_id = domain::QuestionId::from(std::move(**raw_id));
      if (!question_id || !question_ids.insert(*question_id).second) {
        return execution_error("ask_user question IDs must be unique");
      }
      const auto selection =
          **kind == "one"    ? std::optional{domain::QuestionSelection::one}
          : **kind == "many" ? std::optional{domain::QuestionSelection::many}
                             : std::nullopt;
      if (!selection || !raw_question.contains("required") ||
          !raw_question.at("required").is_boolean() ||
          !raw_question.contains("minimum_selections") ||
          !raw_question.at("minimum_selections").is_number_unsigned() ||
          !raw_question.contains("maximum_selections") ||
          !raw_question.at("maximum_selections").is_number_unsigned() ||
          !raw_question.contains("options") ||
          !raw_question.at("options").is_array()) {
        return execution_error("ask_user question constraints are invalid");
      }

      const bool required = raw_question.at("required").get<bool>();
      const auto minimum =
          raw_question.at("minimum_selections").get<std::size_t>();
      const auto maximum =
          raw_question.at("maximum_selections").get<std::size_t>();
      const auto& raw_options = raw_question.at("options");
      if (raw_options.empty() ||
          raw_options.size() > limits.options_per_question ||
          maximum < minimum || maximum == 0 || (required && minimum == 0) ||
          (!required && minimum != 0) ||
          (*selection == domain::QuestionSelection::one && maximum != 1)) {
        return execution_error("ask_user selection bounds are impossible");
      }

      std::set<std::string> option_ids;
      std::vector<domain::QuestionOption> options;
      options.reserve(raw_options.size());
      std::size_t recommended{};
      for (const auto& raw_option : raw_options) {
        if (!has_only_keys(raw_option,
                           {"id", "label", "description", "recommended"})) {
          return execution_error("ask_user option must be an object");
        }
        auto option_id =
            bounded_text(raw_option, "id", domain::QuestionId::max_size);
        auto label = bounded_text(raw_option, "label", limits.label_bytes);
        auto description = bounded_text(raw_option, "description",
                                        limits.description_bytes, true);
        if (!option_id || !label || !description ||
            !option_ids.insert(**option_id).second) {
          return execution_error("ask_user options are invalid or duplicated");
        }
        bool is_recommended{};
        if (raw_option.contains("recommended")) {
          if (!raw_option.at("recommended").is_boolean()) {
            return execution_error("ask_user recommendation is invalid");
          }
          is_recommended = raw_option.at("recommended").get<bool>();
        }
        if (is_recommended) ++recommended;
        options.push_back({std::move(**option_id), std::move(**label),
                           std::move(*description), is_recommended});
      }

      std::optional<domain::QuestionOtherInput> other;
      if (raw_question.contains("other")) {
        const auto& raw_other = raw_question.at("other");
        if (!has_only_keys(raw_other, {"label", "placeholder"})) {
          return execution_error("ask_user Other definition is invalid");
        }
        auto label = bounded_text(raw_other, "label", limits.label_bytes);
        auto placeholder = bounded_text(raw_other, "placeholder",
                                        limits.description_bytes, true);
        if (!label || !placeholder) {
          return execution_error("ask_user Other text is invalid");
        }
        other = domain::QuestionOtherInput{std::move(**label),
                                           std::move(*placeholder),
                                           limits.other_answer_bytes};
      }
      const auto available = options.size() + (other ? 1U : 0U);
      if (maximum > available || recommended > maximum) {
        return execution_error("ask_user selection bounds exceed its options");
      }
      result.push_back({std::move(*question_id), std::move(**prompt),
                        *selection, std::move(options), required, minimum,
                        maximum, std::move(other)});
    }
    return result;
  } catch (...) {
    return execution_error("ask_user arguments are malformed");
  }
}

class AskUserStream final : public ToolExecutionStream {
 public:
  explicit AskUserStream(std::vector<domain::QuestionDefinition> questions)
      : m_questions(std::move(questions)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<ToolExecutionEvent>,
                       ToolExecutionError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(ToolExecutionError{
          ToolExecutionErrorCode::cancelled, "ask_user was cancelled", false});
    }
    if (m_emitted) return std::optional<ToolExecutionEvent>{};
    m_emitted = true;
    return std::optional<ToolExecutionEvent>{
        ToolInputRequested{std::move(m_questions)}};
  }

 private:
  std::vector<domain::QuestionDefinition> m_questions;
  bool m_emitted{};
};

class AskUserExecutor final : public ToolExecutor {
 public:
  explicit AskUserExecutor(AskUserLimits limits) : m_limits(limits) {}

  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<ValidatedToolArguments, ToolExecutionError> override {
    auto questions = parse_questions(arguments, m_limits);
    if (!questions) return std::unexpected(std::move(questions.error()));
    return ValidatedToolArguments{arguments, {}};
  }

  auto start(ToolInvocation invocation, std::stop_token)
      -> std::expected<std::unique_ptr<ToolExecutionStream>,
                       ToolExecutionError> override {
    auto questions = parse_questions(invocation.arguments.value, m_limits);
    if (!questions) return std::unexpected(std::move(questions.error()));
    return std::make_unique<AskUserStream>(std::move(*questions));
  }

 private:
  AskUserLimits m_limits;
};

[[nodiscard]] auto valid_limits(const AskUserLimits& limits) -> bool {
  constexpr AskUserLimits maximums;
  return limits.questions != 0 && limits.options_per_question != 0 &&
         limits.prompt_bytes != 0 && limits.description_bytes != 0 &&
         limits.label_bytes != 0 && limits.other_answer_bytes != 0 &&
         limits.questions <= maximums.questions &&
         limits.options_per_question <= maximums.options_per_question &&
         limits.prompt_bytes <= maximums.prompt_bytes &&
         limits.description_bytes <= maximums.description_bytes &&
         limits.label_bytes <= maximums.label_bytes &&
         limits.other_answer_bytes <= maximums.other_answer_bytes;
}

} // namespace

auto ask_user_declaration(const AskUserLimits& limits)
    -> backend::ToolDeclaration {
  Json option_properties = Json::object();
  option_properties["id"] = {{"type", "string"}, {"maxLength", 128}};
  option_properties["label"] = {{"type", "string"},
                                {"maxLength", limits.label_bytes}};
  option_properties["description"] = {{"type", "string"},
                                      {"maxLength", limits.description_bytes}};
  option_properties["recommended"] = {{"type", "boolean"}};

  Json option = {{"type", "object"},
                 {"additionalProperties", false},
                 {"required", Json::array({"id", "label"})},
                 {"properties", std::move(option_properties)}};
  Json options = {{"type", "array"},
                  {"minItems", 1},
                  {"maxItems", limits.options_per_question},
                  {"items", std::move(option)}};

  Json other_properties = Json::object();
  other_properties["label"] = {{"type", "string"},
                               {"maxLength", limits.label_bytes}};
  other_properties["placeholder"] = {{"type", "string"},
                                     {"maxLength", limits.description_bytes}};
  Json other = {{"type", "object"},
                {"additionalProperties", false},
                {"required", Json::array({"label"})},
                {"properties", std::move(other_properties)}};

  Json question_properties = Json::object();
  question_properties["id"] = {{"type", "string"}, {"maxLength", 128}};
  question_properties["prompt"] = {{"type", "string"},
                                   {"maxLength", limits.prompt_bytes}};
  question_properties["kind"] = {{"enum", Json::array({"one", "many"})}};
  question_properties["required"] = {{"type", "boolean"}};
  question_properties["minimum_selections"] = {{"type", "integer"},
                                               {"minimum", 0}};
  question_properties["maximum_selections"] = {{"type", "integer"},
                                               {"minimum", 1}};
  question_properties["options"] = std::move(options);
  question_properties["other"] = std::move(other);

  Json question = {{"type", "object"},
                   {"additionalProperties", false},
                   {"required", Json::array({"id", "prompt", "kind", "required",
                                             "minimum_selections",
                                             "maximum_selections", "options"})},
                   {"properties", std::move(question_properties)}};
  Json questions = {{"type", "array"},
                    {"minItems", 1},
                    {"maxItems", limits.questions},
                    {"items", std::move(question)}};
  Json schema = {{"type", "object"},
                 {"additionalProperties", false},
                 {"required", Json::array({"questions"})},
                 {"properties", {{"questions", std::move(questions)}}}};
  return {"ask_user",
          "Ask the user a small set of structured questions. This gathers "
          "information and grants no authority.",
          {"application/schema+json", schema.dump()},
          {},
          {}};
}

auto register_ask_user_tool(ToolRegistry& registry,
                            const bool interactive_input_available,
                            AskUserLimits limits)
    -> std::expected<void, ToolRegistryError> {
  if (!interactive_input_available) {
    return std::unexpected(ToolRegistryError{
        ToolRegistryErrorCode::interactive_input_unavailable,
        "ask_user requires an interactive question input protocol"});
  }
  if (!valid_limits(limits)) {
    return std::unexpected(
        ToolRegistryError{ToolRegistryErrorCode::invalid_declaration,
                          "ask_user limits must be positive"});
  }
  return registry.register_tool(
      ask_user_declaration(limits), std::make_shared<AskUserExecutor>(limits),
      ToolExecutionLimits{std::size_t{64} * 1024U, 1, std::chrono::seconds{5}},
      ToolExecutorContract{"aiforge.runtime.ask_user", "1"},
      ToolCategory::interaction);
}

} // namespace aiforge::runtime
