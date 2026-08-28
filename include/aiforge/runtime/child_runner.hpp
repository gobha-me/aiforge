#pragma once

#include <aiforge/domain/child_run.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace aiforge::runtime {

enum class ChildRunErrorCode {
  unavailable,
  cancelled,
  timed_out,
  budget_exhausted,
  protocol_failure,
  internal_failure,
};

struct ChildRunError {
  ChildRunErrorCode code{ChildRunErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const ChildRunError&) const -> bool = default;
};

struct ChildRunResult {
  domain::SessionTaskOutcome outcome{domain::SessionTaskOutcome::failed};
  domain::ChildRunConsumption consumption;
  std::vector<domain::EvidenceId> evidence_ids;
  std::vector<domain::ArtifactId> artifact_ids;
  std::optional<domain::DomainError> error;
  std::optional<domain::ReviewChildResult> review;
  auto operator==(const ChildRunResult&) const -> bool = default;
};

struct ChildRunInvocation {
  domain::RunId child_run_id;
  domain::ChildRunDescriptor descriptor;
  domain::PlanTask task;
  domain::ContextParcel context;
  auto operator==(const ChildRunInvocation&) const -> bool = default;
};

class ChildRunResultStream {
 public:
  virtual ~ChildRunResultStream() = default;

  [[nodiscard]] virtual auto next(std::stop_token stop_token)
      -> std::expected<std::optional<ChildRunResult>, ChildRunError> = 0;
};

class ChildRunner {
 public:
  virtual ~ChildRunner() = default;

  [[nodiscard]] virtual auto maximum_concurrency() const noexcept
      -> std::size_t {
    return 1;
  }

  [[nodiscard]] virtual auto start(ChildRunInvocation invocation,
                                   std::stop_token stop_token)
      -> std::expected<std::unique_ptr<ChildRunResultStream>,
                       ChildRunError> = 0;
};

}  // namespace aiforge::runtime
