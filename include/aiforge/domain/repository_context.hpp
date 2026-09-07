#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/project_instructions.hpp>

namespace aiforge::domain {

inline constexpr std::size_t repository_context_maximum_instructions = 128;
inline constexpr std::size_t repository_context_maximum_evidence = 64;
inline constexpr std::size_t repository_context_maximum_path_bytes = 4096;
inline constexpr std::uint64_t repository_context_maximum_instruction_bytes =
    std::uint64_t{1024} * 1024;
inline constexpr std::uint64_t
    repository_context_maximum_instruction_total_bytes =
        std::uint64_t{4} * 1024 * 1024;
inline constexpr std::uint64_t repository_context_maximum_evidence_bytes =
    std::uint64_t{256} * 1024;
inline constexpr std::uint64_t repository_context_maximum_evidence_total_bytes =
    std::uint64_t{2} * 1024 * 1024;

enum class RepositoryContextDecision {
  admitted,
  omitted_budget,
  omitted_class_budget,
};

struct RepositoryContextInstruction {
  ProjectInstructionId instruction_id;
  RepositorySourceIdentity source;
  std::string applicable_subtree;
  std::uint32_t specificity{};
  std::uint64_t order{};
  std::uint64_t estimated_tokens{};
  ContentDigest text_digest;
  auto operator==(const RepositoryContextInstruction&) const -> bool = default;
};

struct RepositoryContextEvidence {
  EvidenceId evidence_id;
  ContextEntryId entry_id;
  MessageId message_id;
  ContextSourceId source_id;
  RepositorySourceIdentity source;
  std::uint64_t order{};
  std::uint64_t estimated_tokens{};
  RepositoryContextDecision decision{RepositoryContextDecision::admitted};
  ContentDigest text_digest;
  auto operator==(const RepositoryContextEvidence&) const -> bool = default;
};

// References only. The root binding is an opaque, non-secret physical lease
// identity; neither absolute paths nor source text belong in this admission.
struct RepositoryContextAdmission {
  std::uint32_t version{1};
  std::string root_binding;
  RepositorySnapshotIdentity source_snapshot;
  std::string target_subtree;
  std::uint64_t selection_revision{};
  ContextCapacity capacity;
  std::vector<RepositoryContextInstruction> instructions;
  std::vector<RepositoryContextEvidence> evidence;
  std::optional<ContentDigest> admission_digest;
  auto operator==(const RepositoryContextAdmission&) const -> bool = default;
};

enum class RepositoryContextErrorCode {
  invalid_request,
  invalid_admission,
  unavailable,
  stale_source,
  resource_exhausted,
  cancelled,
  timed_out,
  internal_failure,
};

struct RepositoryContextError {
  RepositoryContextErrorCode code{RepositoryContextErrorCode::internal_failure};
  std::string message;
  auto operator==(const RepositoryContextError&) const -> bool = default;
};

[[nodiscard]] auto seal_repository_context_admission(
    RepositoryContextAdmission& admission)
    -> std::expected<void, RepositoryContextError>;
[[nodiscard]] auto validate_repository_context_admission(
    const RepositoryContextAdmission& admission)
    -> std::expected<void, RepositoryContextError>;
[[nodiscard]] auto repository_context_admission_matches_context(
    const RepositoryContextAdmission& admission,
    const ConstructedContext& context) -> bool;
[[nodiscard]] auto repository_context_admission_matches_context(
    const std::optional<RepositoryContextAdmission>& admission,
    const ConstructedContext& context) -> bool;
// Only snapshot fingerprints may advance. Membership, source bytes, ordering,
// identities, capacity and admitted/omitted decisions remain pinned.
[[nodiscard]] auto repository_context_admission_successor(
    const RepositoryContextAdmission& previous,
    const RepositoryContextAdmission& next) -> bool;

} // namespace aiforge::domain
