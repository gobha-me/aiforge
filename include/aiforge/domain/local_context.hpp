#pragma once

#include <optional>
#include <vector>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/local_source.hpp>

namespace aiforge::domain {

inline constexpr std::size_t local_context_maximum_admission_bytes =
    std::size_t{512} * 1024;
enum class LocalContextDecision {
  admitted,
  omitted_budget,
  omitted_class_budget
};

struct LocalContextEvidence {
  EvidenceId evidence_id;
  ContextEntryId entry_id;
  MessageId message_id;
  ContextSourceId source_id;
  LocalSourceIdentity source;
  std::uint64_t order{};
  // Version 1 conservatively charges one token per exact UTF8 byte.
  std::uint64_t estimated_tokens{};
  LocalContextDecision decision{LocalContextDecision::admitted};
  auto operator==(const LocalContextEvidence&) const -> bool = default;
};

// References only; no source text, absolute paths, live generations or grants.
struct LocalContextAdmission {
  std::uint32_t version{1};
  SessionId session_id;
  std::uint64_t selection_revision{};
  ContextCapacity capacity;
  std::vector<LocalContextEvidence> evidence;
  std::optional<ContentDigest> admission_digest;
  auto operator==(const LocalContextAdmission&) const -> bool = default;
};
enum class LocalContextErrorCode {
  invalid_request,
  invalid_admission,
  unavailable,
  stale_source,
  stale_lease,
  resource_exhausted,
  cancelled,
  timed_out,
  internal_failure
};
struct LocalContextError {
  LocalContextErrorCode code;
  std::string message;
  auto operator==(const LocalContextError&) const -> bool = default;
};

[[nodiscard]] auto seal_local_context_admission(LocalContextAdmission& value)
    -> std::expected<void, LocalContextError>;
// Exact neutral location: local-file:v1:<root.binding>/<relative_path>.
[[nodiscard]] auto local_context_source_location(
    const LocalSourceIdentity& source)
    -> std::expected<std::string, LocalContextError>;
[[nodiscard]] auto validate_local_context_admission(
    const LocalContextAdmission& value)
    -> std::expected<void, LocalContextError>;
[[nodiscard]] auto local_context_admission_matches_context(
    const LocalContextAdmission& admission, const ConstructedContext& context)
    -> bool;
[[nodiscard]] auto local_context_admission_matches_context(
    const std::optional<LocalContextAdmission>& admission,
    const ConstructedContext& context) -> bool;
// Local sources have no rolling snapshot identity. Every admitted fact is
// fixed.
[[nodiscard]] auto local_context_admission_successor(
    const LocalContextAdmission& previous, const LocalContextAdmission& next)
    -> bool;

} // namespace aiforge::domain
