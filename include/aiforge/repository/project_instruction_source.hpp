#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/domain/project_instructions.hpp>
#include <aiforge/domain/repository.hpp>

namespace aiforge::repository {

struct ProjectInstructionLimits {
  std::size_t maximum_documents{128};
  std::size_t maximum_path_bytes{4096};
  std::uint64_t maximum_document_bytes{1024U * 1024U};
  std::uint64_t maximum_total_bytes{4U * 1024U * 1024U};
  std::chrono::milliseconds timeout{30000};
  auto operator==(const ProjectInstructionLimits&) const -> bool = default;
};

struct ProjectInstructionRequest {
  domain::RepositorySnapshot baseline;
  // Empty denotes the root. Nonempty values are normalized generic relative
  // directory paths; callers map a target file to its parent directory.
  std::string target_subtree;
  ProjectInstructionLimits limits;
  auto operator==(const ProjectInstructionRequest&) const -> bool = default;
};

enum class ProjectInstructionErrorCode {
  invalid_request,
  not_found,
  outside_repository,
  permission_denied,
  unsupported_entry,
  malformed_text,
  stale_snapshot,
  unstable,
  resource_exhausted,
  timed_out,
  cancelled,
  io_failure,
  internal_failure,
};

struct ProjectInstructionError {
  ProjectInstructionErrorCode code{
      ProjectInstructionErrorCode::internal_failure};
  std::string message;
  std::optional<std::string> relative_path;
  bool retryable{};
  auto operator==(const ProjectInstructionError&) const -> bool = default;
};

class ProjectInstructionSource {
 public:
  virtual ~ProjectInstructionSource() = default;

  [[nodiscard]] virtual auto discover(ProjectInstructionRequest request,
                                      std::stop_token stop_token = {})
      -> std::expected<domain::ProjectInstructionDiscovery,
                       ProjectInstructionError> = 0;
};

} // namespace aiforge::repository
