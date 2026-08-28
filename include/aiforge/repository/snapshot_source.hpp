#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>

#include <aiforge/domain/repository.hpp>

namespace aiforge::repository {

struct RepositorySnapshotLimits {
  std::size_t maximum_entries{100000};
  std::size_t maximum_path_bytes{4096};
  std::uint64_t maximum_file_bytes{64U * 1024U * 1024U};
  std::uint64_t maximum_total_bytes{1024ULL * 1024ULL * 1024ULL};
  std::size_t maximum_command_output_bytes{16U * 1024U * 1024U};
  std::chrono::milliseconds command_timeout{10000};
  std::chrono::milliseconds observation_timeout{30000};
  auto operator==(const RepositorySnapshotLimits&) const -> bool = default;
};

struct RepositorySnapshotRequest {
  std::string root;
  RepositorySnapshotLimits limits;
  auto operator==(const RepositorySnapshotRequest&) const -> bool = default;
};

enum class RepositorySnapshotErrorCode {
  invalid_request,
  not_found,
  not_directory,
  permission_denied,
  unsupported_entry,
  unstable,
  resource_exhausted,
  vcs_failure,
  io_failure,
  timed_out,
  cancelled,
  internal_failure,
};

struct RepositorySnapshotError {
  RepositorySnapshotErrorCode code{
      RepositorySnapshotErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const RepositorySnapshotError&) const -> bool = default;
};

class RepositorySnapshotSource {
 public:
  virtual ~RepositorySnapshotSource() = default;

  [[nodiscard]] virtual auto observe(RepositorySnapshotRequest request,
                                     std::stop_token stop_token = {})
      -> std::expected<domain::RepositorySnapshot, RepositorySnapshotError> = 0;
};

[[nodiscard]] auto validate_repository_snapshot(
    const domain::RepositorySnapshot& snapshot,
    const RepositorySnapshotLimits& limits = {})
    -> std::expected<void, RepositorySnapshotError>;

} // namespace aiforge::repository
