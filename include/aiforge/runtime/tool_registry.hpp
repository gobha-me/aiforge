#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <aiforge/backend/backend.hpp>

namespace aiforge::runtime {

enum class ToolRegistryErrorCode {
  invalid_declaration,
  duplicate_name,
  missing_executor,
  interactive_input_unavailable,
  internal_failure,
};

struct ToolRegistryError {
  ToolRegistryErrorCode code;
  std::string message;
  auto operator==(const ToolRegistryError&) const -> bool = default;
};

enum class ToolExecutionErrorCode {
  invalid_arguments,
  unavailable,
  cancelled,
  timed_out,
  output_limit,
  protocol_failure,
  internal_failure,
};

struct ToolExecutionError {
  ToolExecutionErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const ToolExecutionError&) const -> bool = default;
};

struct ValidatedToolArguments {
  domain::StructuredDataBlock value;
  std::vector<domain::CapabilityScope> required_scopes{};
  // Empty retains the declaration's complete effect set. A validator may
  // return a nonempty subset when arguments narrow a tool's effects.
  std::vector<domain::Effect> required_effects{};
  auto operator==(const ValidatedToolArguments&) const -> bool = default;
};

struct ToolExecutionLimits {
  std::size_t output_bytes{1024U * 1024U};
  std::size_t progress_events{1024};
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
  auto operator==(const ToolExecutionLimits&) const -> bool = default;
};

struct ToolInvocation {
  domain::InvocationId invocation_id;
  std::optional<domain::InvocationId> parent_invocation_id;
  std::string tool_name;
  ValidatedToolArguments arguments;
  std::vector<domain::CapabilityScope> granted_scopes;
  ToolExecutionLimits limits;
  auto operator==(const ToolInvocation&) const -> bool = default;
};

struct ToolProgress {
  std::vector<domain::ContentBlock> content;
  auto operator==(const ToolProgress&) const -> bool = default;
};

struct ToolResult {
  std::vector<domain::ContentBlock> content;
  std::vector<domain::ArtifactMetadata> created_artifacts{};
  auto operator==(const ToolResult&) const -> bool = default;
};

struct ToolInputRequested {
  std::vector<domain::QuestionDefinition> questions;
  auto operator==(const ToolInputRequested&) const -> bool = default;
};

using ToolExecutionEvent =
    std::variant<ToolProgress, ToolInputRequested, ToolResult>;

class ToolExecutionStream {
 public:
  virtual ~ToolExecutionStream() = default;

  [[nodiscard]] virtual auto next(std::stop_token stop_token)
      -> std::expected<std::optional<ToolExecutionEvent>,
                       ToolExecutionError> = 0;
};

class ToolExecutor {
 public:
  virtual ~ToolExecutor() = default;

  [[nodiscard]] virtual auto validate(
      const domain::StructuredDataBlock& arguments) const
      -> std::expected<ValidatedToolArguments, ToolExecutionError> = 0;

  [[nodiscard]] virtual auto start(ToolInvocation invocation,
                                   std::stop_token stop_token)
      -> std::expected<std::unique_ptr<ToolExecutionStream>,
                       ToolExecutionError> = 0;
};

struct RegisteredTool {
  backend::ToolDeclaration declaration;
  ToolExecutionLimits limits;
  std::shared_ptr<ToolExecutor> executor;
};

class ToolRegistrySnapshot final {
 public:
  ToolRegistrySnapshot() = default;

  [[nodiscard]] auto declarations() const noexcept
      -> const std::vector<backend::ToolDeclaration>&;
  [[nodiscard]] auto find(std::string_view name) const noexcept
      -> const RegisteredTool*;
  [[nodiscard]] auto empty() const noexcept -> bool { return m_tools.empty(); }
  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return m_tools.size();
  }

 private:
  friend class ToolRegistry;
  ToolRegistrySnapshot(std::vector<RegisteredTool> tools,
                       std::vector<backend::ToolDeclaration> declarations)
      : m_tools(std::move(tools)), m_declarations(std::move(declarations)) {}

  std::vector<RegisteredTool> m_tools;
  std::vector<backend::ToolDeclaration> m_declarations;
};

class ToolRegistry final {
 public:
  [[nodiscard]] auto register_tool(backend::ToolDeclaration declaration,
                                   std::shared_ptr<ToolExecutor> executor,
                                   ToolExecutionLimits limits = {})
      -> std::expected<void, ToolRegistryError>;

  [[nodiscard]] auto snapshot() const
      -> std::expected<ToolRegistrySnapshot, ToolRegistryError>;

 private:
  std::vector<RegisteredTool> m_tools;
};

[[nodiscard]] auto tool_result_messages(
    std::span<const domain::RunEvent> events)
    -> std::expected<std::vector<domain::Message>, ToolExecutionError>;

} // namespace aiforge::runtime
