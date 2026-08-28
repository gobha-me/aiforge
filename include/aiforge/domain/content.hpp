#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/domain/ids.hpp>

namespace aiforge::domain {

struct TextBlock {
  std::string text;
  auto operator==(const TextBlock&) const -> bool = default;
};

struct StructuredDataBlock {
  std::string media_type;
  std::string data;
  auto operator==(const StructuredDataBlock&) const -> bool = default;
};

struct CitationBlock {
  std::string uri;
  std::optional<std::string> title;
  auto operator==(const CitationBlock&) const -> bool = default;
};

struct ArtifactReferenceBlock {
  ArtifactId artifact_id;
  std::optional<std::string> label;
  auto operator==(const ArtifactReferenceBlock&) const -> bool = default;
};

struct UnknownContentBlock {
  std::string type_name;
  auto operator==(const UnknownContentBlock&) const -> bool = default;
};

using ContentBlock = std::variant<TextBlock, StructuredDataBlock, CitationBlock,
                                  ArtifactReferenceBlock, UnknownContentBlock>;

enum class Role {
  system,
  user,
  assistant,
  tool,
  evidence,
};

struct Message {
  MessageId message_id;
  Role role;
  std::vector<ContentBlock> content;
  std::optional<InvocationId> invocation_id;
  auto operator==(const Message&) const -> bool = default;
};

struct Usage {
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
  std::uint64_t cached_input_tokens{};
  std::uint64_t reasoning_tokens{};
  auto operator==(const Usage&) const -> bool = default;
};

enum class FinishReason {
  stop,
  length,
  tool_call,
  content_filter,
  other,
};

enum class ErrorCode {
  invalid_event,
  invalid_state,
  backend,
  policy,
  cancelled,
  unavailable,
};

struct DomainError {
  ErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const DomainError&) const -> bool = default;
};

enum class Effect {
  read,
  write,
  remove,
  execute,
  network,
  communicate,
  spend,
  change_infrastructure,
  change_privileges,
};

struct CapabilityScope {
  Effect effect;
  std::string kind;
  std::string value;
  auto operator==(const CapabilityScope&) const -> bool = default;
};

enum class QuestionSelection {
  one,
  many,
};

struct QuestionOption {
  std::string option_id;
  std::string label;
  std::optional<std::string> description;
  bool recommended{};
  auto operator==(const QuestionOption&) const -> bool = default;
};

struct QuestionOtherInput {
  std::string label{"Other"};
  std::optional<std::string> placeholder;
  std::size_t maximum_bytes{4096};
  auto operator==(const QuestionOtherInput&) const -> bool = default;
};

struct QuestionDefinition {
  QuestionId question_id;
  std::string prompt;
  QuestionSelection selection{QuestionSelection::one};
  std::vector<QuestionOption> options;
  bool required{true};
  std::size_t minimum_selections{1};
  std::optional<std::size_t> maximum_selections{1};
  std::optional<QuestionOtherInput> other;
  auto operator==(const QuestionDefinition&) const -> bool = default;
};

struct QuestionAnswer {
  QuestionId question_id;
  std::vector<std::string> selected_option_ids;
  std::optional<std::string> free_form;
  auto operator==(const QuestionAnswer&) const -> bool = default;
};

} // namespace aiforge::domain
