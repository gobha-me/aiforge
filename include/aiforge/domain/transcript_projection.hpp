#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/domain/run_projection.hpp>

namespace aiforge::domain {

enum class TranscriptMessageState {
  streaming,
  complete,
  cancelled,
  failed,
};

struct TranscriptMessage {
  MessageId message_id;
  Role role{Role::user};
  std::vector<ContentBlock> content;
  TranscriptMessageState state{TranscriptMessageState::complete};
  std::optional<InferenceId> inference_id;
  Usage usage;
  std::optional<DomainError> error;
  std::vector<ArtifactMetadata> artifacts;
  auto operator==(const TranscriptMessage&) const -> bool = default;
};

enum class TranscriptToolState {
  proposed,
  allowed,
  awaiting_approval,
  running,
  complete,
  denied,
  cancelled,
  failed,
};

struct TranscriptToolSummary {
  InvocationId invocation_id;
  std::string tool_name;
  TranscriptToolState state{TranscriptToolState::proposed};
  std::vector<ContentBlock> progress;
  std::vector<ContentBlock> result;
  std::optional<DomainError> error;
  auto operator==(const TranscriptToolSummary&) const -> bool = default;
};

enum class TranscriptQuestionState {
  awaiting_answer,
  answered,
  cancelled,
};

struct TranscriptQuestionSummary {
  QuestionDefinition question;
  TranscriptQuestionState state{TranscriptQuestionState::awaiting_answer};
  std::optional<QuestionAnswer> answer;
  std::optional<std::string> cancellation_reason;
  std::optional<InvocationId> invocation_id;
  auto operator==(const TranscriptQuestionSummary&) const -> bool = default;
};

struct TranscriptArtifactReference {
  ArtifactMetadata artifact;
  std::optional<MessageId> message_id;
  auto operator==(const TranscriptArtifactReference&) const -> bool = default;
};

struct TranscriptVerificationSummary {
  VerificationEvidence evidence;
  auto operator==(const TranscriptVerificationSummary&) const -> bool = default;
};

enum class TranscriptNoticeKind {
  failed,
  cancelled,
};

struct TranscriptNotice {
  TranscriptNoticeKind kind{TranscriptNoticeKind::failed};
  std::string message;
  auto operator==(const TranscriptNotice&) const -> bool = default;
};

using TranscriptItem =
    std::variant<TranscriptMessage, TranscriptToolSummary,
                 TranscriptQuestionSummary, TranscriptArtifactReference,
                 TranscriptVerificationSummary, TranscriptNotice>;

enum class TranscriptProjectionErrorCode {
  invalid_envelope,
  duplicate_event,
  wrong_run,
  non_monotonic_sequence,
  invalid_transition,
  unknown_message,
  unknown_inference,
  unknown_invocation,
  unknown_question,
  unknown_artifact,
  invalid_verification,
  usage_overflow,
  cost_overflow,
  internal_failure,
};

struct TranscriptProjectionError {
  TranscriptProjectionErrorCode code;
  std::string message;
  auto operator==(const TranscriptProjectionError&) const -> bool = default;
};

class TranscriptProjection final {
 public:
  [[nodiscard]] auto apply(const RunEvent& event)
      -> std::expected<void, TranscriptProjectionError>;

  [[nodiscard]] static auto rebuild(std::span<const RunEvent> events)
      -> std::expected<TranscriptProjection, TranscriptProjectionError>;

  [[nodiscard]] auto run_id() const noexcept -> const std::optional<RunId>& {
    return m_run.run_id();
  }
  [[nodiscard]] auto status() const noexcept -> RunStatus {
    return m_run.status();
  }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t {
    return m_run.last_sequence();
  }
  [[nodiscard]] auto usage() const noexcept -> const Usage& {
    return m_run.usage();
  }
  [[nodiscard]] auto items() const noexcept
      -> const std::vector<TranscriptItem>& {
    return m_items;
  }

 private:
  [[nodiscard]] auto apply_in_place(const RunEvent& event)
      -> std::expected<void, TranscriptProjectionError>;
  [[nodiscard]] auto message(const MessageId& id) -> TranscriptMessage*;
  [[nodiscard]] auto inference_message(const InferenceId& id)
      -> TranscriptMessage*;
  [[nodiscard]] auto tool(const InvocationId& id) -> TranscriptToolSummary*;
  [[nodiscard]] auto question(const QuestionId& id,
                              const std::optional<InvocationId>& invocation_id)
      -> TranscriptQuestionSummary*;

  RunProjection m_run;
  std::vector<TranscriptItem> m_items;
  std::set<EventId> m_event_ids;
  std::map<InferenceId, Usage> m_inference_usage;
  std::map<ArtifactId, ArtifactMetadata> m_artifacts;
  std::set<VerificationEvidenceId> m_verification_ids;
  std::set<InvocationId> m_verification_invocations;
};

// Composes the per-run transcript reducer over an ordered session event
// stream. Runs retain their first-observed order while each reducer accepts
// session-sequence gaps caused by events belonging to other runs.
class SessionTranscriptProjection final {
 public:
  [[nodiscard]] auto apply(const RunEvent& event)
      -> std::expected<void, TranscriptProjectionError>;

  [[nodiscard]] static auto rebuild(std::span<const RunEvent> events)
      -> std::expected<SessionTranscriptProjection, TranscriptProjectionError>;

  [[nodiscard]] auto runs() const noexcept
      -> std::span<const TranscriptProjection> {
    return m_runs;
  }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t {
    return m_last_sequence;
  }

 private:
  [[nodiscard]] auto apply_in_place(const RunEvent& event)
      -> std::expected<void, TranscriptProjectionError>;

  std::vector<TranscriptProjection> m_runs;
  std::map<RunId, std::size_t> m_run_indices;
  std::set<EventId> m_event_ids;
  std::uint64_t m_last_sequence{};
};

} // namespace aiforge::domain
