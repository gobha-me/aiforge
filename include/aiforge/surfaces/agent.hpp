#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/domain/events.hpp>
#include <aiforge/surfaces/chat_session.hpp>

namespace aiforge::surfaces {

enum class AgentOperation { submit, replay };
enum class AgentErrorCode {
  invalid_request,
  unavailable,
  resource_exhausted,
  cancelled,
  output_failed,
  run_failed,
  internal_failure,
  cleanup_incomplete,
};
struct AgentError {
  AgentErrorCode code;
  std::string message;
  auto operator==(const AgentError&) const -> bool = default;
};
struct AgentRequest {
  AgentOperation operation{AgentOperation::submit};
  std::optional<domain::SessionId> session_id;
  std::optional<domain::ModelId> model;
  std::optional<domain::ToolProfileId> profile;
  std::vector<std::string> tools;
  std::string prompt;
};
inline constexpr std::size_t agent_maximum_input_bytes =
    std::size_t{1024} * 1024U;
inline constexpr std::size_t agent_maximum_record_bytes =
    std::size_t{2} * 1024U * 1024U;

class AgentRecordSink {
 public:
  virtual ~AgentRecordSink() = default;
  // Includes one final LF. A failure makes the sink unusable for this run.
  [[nodiscard]] virtual auto write_record(std::string_view record)
      -> std::expected<void, AgentError> = 0;
};

enum class AgentStatus {
  completed,
  failed,
  cancelled,
  interaction_required,
  recovery_required,
};
struct AgentRunLimits {
  std::chrono::milliseconds run_timeout{std::chrono::minutes{5}};
  std::chrono::milliseconds cleanup_timeout{std::chrono::seconds{2}};
};

struct AgentOutcome {
  AgentStatus status;
  bool durable_terminal{};
  std::optional<domain::SessionId> session_id;
  std::optional<domain::RunId> run_id;
  std::string reason;
  auto operator==(const AgentOutcome&) const -> bool = default;
};

[[nodiscard]] auto parse_agent_request(std::string_view input)
    -> std::expected<AgentRequest, AgentError>;
[[nodiscard]] auto agent_accepted_record(
    const domain::SessionId& session, const domain::RunId& run,
    const std::vector<backend::ToolDeclaration>& tools)
    -> std::expected<std::string, AgentError>;
[[nodiscard]] auto agent_event_record(const domain::SessionId& session,
                                      const domain::RunEvent& event)
    -> std::expected<std::string, AgentError>;
[[nodiscard]] auto agent_error_record(const AgentError& error)
    -> std::expected<std::string, AgentError>;
[[nodiscard]] auto agent_terminal_record(AgentOutcome outcome)
    -> std::expected<std::string, AgentError>;
[[nodiscard]] auto run_agent_session(ChatSession& session,
                                     const AgentRequest& request,
                                     AgentRecordSink& sink,
                                     std::stop_token stop_token = {},
                                     AgentRunLimits limits = {})
    -> std::expected<AgentOutcome, AgentError>;
[[nodiscard]] auto replay_agent_session(
    const domain::SessionId& session,
    const std::vector<domain::RunEvent>& events, AgentRecordSink& sink,
    std::stop_token stop_token = {}) -> std::expected<AgentOutcome, AgentError>;

} // namespace aiforge::surfaces
