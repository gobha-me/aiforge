#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/backend/backend.hpp>
#include <aiforge/runtime/application_launch_context.hpp>

namespace aiforge::runtime {

using ToolSpendDisposition =
    std::variant<domain::ToolSpendReleased, domain::ToolSpendFinalized,
                 domain::ToolSpendReconciliationRequired>;

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
  // Paid executors return a terminal disposition only when they have durable
  // evidence bound to this invocation. A release requires proof that provider
  // transport did not start; provider-reported finalization carries the
  // correlated provider evidence digest. The runtime validates the neutral
  // reservation contract but does not interpret provider-specific evidence.
  std::optional<ToolSpendDisposition> spend_disposition{};
  auto operator==(const ToolExecutionError&) const -> bool = default;
};

struct ValidatedToolArguments {
  domain::StructuredDataBlock value;
  std::vector<domain::CapabilityScope> required_scopes{};
  // Empty retains the declaration's complete effect set. A validator may
  // return a nonempty subset when arguments narrow a tool's effects.
  std::vector<domain::Effect> required_effects{};
  // Required exactly when the effective effects include spend. The runtime
  // records this maximum after policy approval and before ToolStarted.
  std::optional<domain::ToolSpendQuote> spend_quote{};
  auto operator==(const ValidatedToolArguments&) const -> bool = default;
};

struct ToolExecutionLimits {
  std::size_t output_bytes{1024U * 1024U};
  std::size_t progress_events{1024};
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
  auto operator==(const ToolExecutionLimits&) const -> bool = default;
};

// Stable implementation identity used to prove that a durable pending run is
// resumed against the same executor contract that validated its arguments.
struct ToolExecutorContract {
  std::string identity;
  std::string version;
  auto operator==(const ToolExecutorContract&) const -> bool = default;
};

// Runtime-only presentation and selection metadata. Categories never enter a
// provider declaration and do not imply effects, scopes, or approval.
enum class ToolCategory {
  interaction,
  memory,
  repository,
  process,
  media,
  other,
};

[[nodiscard]] auto tool_category_name(ToolCategory category) noexcept
    -> std::string_view;
[[nodiscard]] auto tool_category_from_name(std::string_view name) noexcept
    -> std::optional<ToolCategory>;
[[nodiscard]] auto all_tool_categories() noexcept
    -> std::span<const ToolCategory>;

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
  // Paid executors must report the terminal spend outcome using evidence bound
  // to this invocation. Provider success without usable cost evidence leaves
  // the reservation in reconciliation rather than assuming a zero cost.
  std::optional<ToolSpendDisposition> spend_disposition{};
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
  std::optional<ToolExecutorContract> executor_contract;
  ToolCategory category{ToolCategory::other};
};

// Canonical durable identity for one exact declaration, limits, and executor
// contract. Registrations without a versioned executor contract have no
// recoverable identity.
[[nodiscard]] auto tool_registration_digest(const RegisteredTool& tool)
    -> std::optional<std::string>;

enum class ToolUnavailableReason {
  not_configured,
  durable_session_required,
  unrestricted_network_required,
  restriction_unavailable,
  unsupported_platform,
  runtime_dependency_or_path_unavailable,
  shell_unimplemented,
};

struct ToolRestrictionUnavailability {
  RestrictionLevel selected_restriction{RestrictionLevel::high};
  RestrictionUnavailableReason reason{
      RestrictionUnavailableReason::mechanism_absent};
  auto operator==(const ToolRestrictionUnavailability&) const -> bool = default;
};

struct ToolUnavailability {
  ToolUnavailableReason reason{ToolUnavailableReason::not_configured};
  std::optional<ToolRestrictionUnavailability> restriction;
  ToolUnavailability() = default;
  ToolUnavailability(
      ToolUnavailableReason unavailable_reason,
      std::optional<ToolRestrictionUnavailability> restriction_detail = {})
      : reason(unavailable_reason), restriction(restriction_detail) {}
  auto operator==(const ToolUnavailability&) const -> bool = default;
};

[[nodiscard]] auto tool_unavailable_reason_text(
    ToolUnavailableReason reason) noexcept -> std::string_view;
inline constexpr std::size_t maximum_tool_unavailability_text_bytes{96};
// Produces a closed, single-line operator summary. Its size is bounded because
// every component is a closed enum; no adapter or platform message enters it.
[[nodiscard]] auto format_tool_unavailability(
    const ToolUnavailability& unavailability) -> std::string;

struct UnavailableTool {
  std::string name;
  ToolCategory category{ToolCategory::other};
  ToolUnavailability unavailability;
  auto operator==(const UnavailableTool&) const -> bool = default;
};

class ToolRegistrySnapshot final {
 public:
  ToolRegistrySnapshot() = default;

  [[nodiscard]] auto declarations() const noexcept
      -> const std::vector<backend::ToolDeclaration>&;
  [[nodiscard]] auto find(std::string_view name) const noexcept
      -> const RegisteredTool*;
  [[nodiscard]] auto find_unavailable(std::string_view name) const noexcept
      -> const UnavailableTool*;
  [[nodiscard]] auto unavailable_tools() const noexcept
      -> std::span<const UnavailableTool>;
  [[nodiscard]] auto subset(std::span<const std::string> names) const
      -> std::expected<ToolRegistrySnapshot, ToolRegistryError>;
  [[nodiscard]] auto replace(RegisteredTool replacement) const
      -> std::expected<ToolRegistrySnapshot, ToolRegistryError>;
  [[nodiscard]] auto empty() const noexcept -> bool { return m_tools.empty(); }
  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return m_tools.size();
  }

 private:
  friend class ToolRegistry;
  ToolRegistrySnapshot(std::vector<RegisteredTool> tools,
                       std::vector<backend::ToolDeclaration> declarations,
                       std::vector<UnavailableTool> unavailable_tools)
      : m_tools(std::move(tools)), m_declarations(std::move(declarations)),
        m_unavailable_tools(std::move(unavailable_tools)) {}

  std::vector<RegisteredTool> m_tools;
  std::vector<backend::ToolDeclaration> m_declarations;
  std::vector<UnavailableTool> m_unavailable_tools;
};

class ToolRegistry final {
 public:
  [[nodiscard]] auto register_tool(
      backend::ToolDeclaration declaration,
      std::shared_ptr<ToolExecutor> executor, ToolExecutionLimits limits = {},
      std::optional<ToolExecutorContract> executor_contract = std::nullopt,
      ToolCategory category = ToolCategory::other)
      -> std::expected<void, ToolRegistryError>;

  [[nodiscard]] auto declare_unavailable_tool(
      std::string name, ToolUnavailability unavailability,
      ToolCategory category = ToolCategory::other)
      -> std::expected<void, ToolRegistryError>;

  [[nodiscard]] auto snapshot() const
      -> std::expected<ToolRegistrySnapshot, ToolRegistryError>;

 private:
  std::vector<RegisteredTool> m_tools;
  std::vector<UnavailableTool> m_unavailable_tools;
};

[[nodiscard]] auto tool_result_messages(
    std::span<const domain::RunEvent> events)
    -> std::expected<std::vector<domain::Message>, ToolExecutionError>;

// Reconstructs provider-neutral assistant tool-call turns and their terminal
// tool messages in durable event order for a follow-up inference. Artifact
// references resolve against preceding creation events from the producing tool
// invocation and become bounded metadata-only JSON (at most 32 references and
// 32 KiB metadata per result). Repeated references are deduplicated. No
// artifact bytes or labels are read; existing tool excerpts remain unchanged.
// Durable events and tool_result_messages retain typed references. Invalid or
// ambiguous artifact provenance fails closed; unknown media stays opaque
// metadata.
[[nodiscard]] auto tool_continuation_messages(
    std::span<const domain::RunEvent> events)
    -> std::expected<std::vector<domain::Message>, ToolExecutionError>;

} // namespace aiforge::runtime
