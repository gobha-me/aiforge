#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/runtime/local_context_controller.hpp>
#include <aiforge/runtime/repository_context_controller.hpp>
#include <aiforge/runtime/session_context.hpp>

namespace aiforge::runtime {

struct SelectedSessionEvidence {
  PreparedSessionContext session;
  domain::ConstructedContext context;
  std::optional<domain::RepositoryContextAdmission> repository_admission;
  std::optional<domain::LocalContextAdmission> local_admission;
  std::vector<ContextSelectionDecisionRecord> decisions;
  ContextClassUsage usage;
};

enum class SessionEvidenceErrorCode {
  invalid_base,
  invalid_evidence,
  resource_exhausted,
  token_overflow,
  selection_failed,
  admission_failed,
  cancelled,
  internal_failure,
};
struct SessionEvidenceError {
  SessionEvidenceErrorCode code;
  std::string message;
  auto operator==(const SessionEvidenceError&) const -> bool = default;
};

// Performs one optional selection after mandatory session preparation. No I/O,
// grants, memory selection or provider work occurs. Repository instructions
// must already be present in base.input. Base orders and source admissions are
// preserved; optional evidence receives checked orders after all base content.
// The returned owning input contains only admitted optional evidence and its
// conversation admission accounts for the exact final context. Base memory and
// conversation seals are required. Sealed optional recovery preparations are
// rejected: recovery restores saved choices without selection.
[[nodiscard]] auto select_session_evidence(
    const PreparedSessionContext& base,
    const std::optional<PreparedRepositoryContext>& repository = std::nullopt,
    const std::optional<PreparedLocalContext>& local = std::nullopt,
    std::stop_token stop = {})
    -> std::expected<SelectedSessionEvidence, SessionEvidenceError>;

} // namespace aiforge::runtime
