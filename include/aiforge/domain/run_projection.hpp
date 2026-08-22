#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {

enum class RunStatus {
  not_started,
  running,
  awaiting_input,
  completed,
  failed,
  cancelled,
};

struct ProjectedMessage {
  MessageId message_id;
  Role role;
  std::vector<ContentBlock> content;
  bool complete{};
  std::optional<InferenceId> inference_id;
  auto operator==(const ProjectedMessage&) const -> bool = default;
};

enum class ProjectionErrorCode {
  invalid_envelope,
  wrong_run,
  non_monotonic_sequence,
  invalid_transition,
  unknown_message,
  wrong_inference,
  usage_overflow,
};

struct ProjectionError {
  ProjectionErrorCode code;
  std::string message;
  auto operator==(const ProjectionError&) const -> bool = default;
};

class RunProjection final {
 public:
  [[nodiscard]] auto apply(const RunEvent& event) -> std::expected<void, ProjectionError>;

  [[nodiscard]] auto run_id() const noexcept -> const std::optional<RunId>& { return m_run_id; }
  [[nodiscard]] auto status() const noexcept -> RunStatus { return m_status; }
  [[nodiscard]] auto messages() const noexcept -> const std::vector<ProjectedMessage>& {
    return m_messages;
  }
  [[nodiscard]] auto usage() const noexcept -> const Usage& { return m_usage; }
  [[nodiscard]] auto active_inference_id() const noexcept -> const std::optional<InferenceId>& {
    return m_active_inference_id;
  }
  [[nodiscard]] auto provenance() const noexcept -> const std::optional<RunProvenance>& {
    return m_provenance;
  }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t { return m_last_sequence; }

 private:
  [[nodiscard]] auto find_message(const MessageId& message_id) -> ProjectedMessage*;
  [[nodiscard]] auto require_running() const -> std::expected<void, ProjectionError>;

  std::optional<RunId> m_run_id;
  RunStatus m_status{RunStatus::not_started};
  std::vector<ProjectedMessage> m_messages;
  Usage m_usage;
  std::optional<InferenceId> m_active_inference_id;
  std::optional<RunProvenance> m_provenance;
  std::uint64_t m_last_sequence{};
};

}  // namespace aiforge::domain
