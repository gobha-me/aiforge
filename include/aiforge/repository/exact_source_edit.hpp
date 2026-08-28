#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/repository/snapshot_source.hpp>

namespace aiforge::repository {

struct ExactSourceEditLimits {
  std::size_t maximum_path_bytes{4096};
  std::uint64_t maximum_source_bytes{64U * 1024U * 1024U};
  std::uint64_t maximum_replacement_bytes{64U * 1024U * 1024U};
  std::chrono::milliseconds timeout{30000};
  auto operator==(const ExactSourceEditLimits&) const -> bool = default;
};

struct ExactSourceReadRequest {
  domain::RepositorySnapshot baseline;
  std::string relative_path;
  ExactSourceEditLimits limits;
  auto operator==(const ExactSourceReadRequest&) const -> bool = default;
};

struct ExactSourceReadResult {
  // The digest covers all content. Exact excerpts are derived later without
  // weakening the precondition used by an edit.
  domain::RepositorySourceIdentity source;
  std::string content;
  auto operator==(const ExactSourceReadResult&) const -> bool = default;
};

struct ExactSourceEditRequest {
  domain::RepositorySnapshot baseline;
  domain::RepositorySourceIdentity expected_source;
  // Zero-based, half-open. begin == end is an insertion.
  domain::SourceByteRange range;
  std::string replacement;
  ExactSourceEditLimits limits;
  auto operator==(const ExactSourceEditRequest&) const -> bool = default;
};

struct ExactSourceEditReceipt {
  domain::RepositorySourceIdentity previous_source;
  domain::RepositorySourceIdentity resulting_source;
  domain::SourceByteRange replaced_range;
  domain::SourceByteRange resulting_range;
  domain::RepositorySnapshotIdentity before_snapshot;
  domain::RepositorySnapshotIdentity after_snapshot;
  auto operator==(const ExactSourceEditReceipt&) const -> bool = default;
};

enum class ExactSourceEditErrorCode {
  invalid_request,
  not_found,
  outside_repository,
  permission_denied,
  unsupported_entry,
  stale_snapshot,
  source_mismatch,
  concurrent_change,
  resource_exhausted,
  io_failure,
  durability_failure,
  timed_out,
  cancelled,
  internal_failure,
};

struct ExactSourceEditError {
  ExactSourceEditErrorCode code{ExactSourceEditErrorCode::internal_failure};
  std::string message;
  std::optional<domain::RepositorySnapshotIdentity> observed_snapshot;
  std::optional<domain::RepositorySourceIdentity> observed_source;
  bool retryable{};
  // True means the caller must inspect the repository before retrying because
  // the effect may have committed even though durability/postcondition checks
  // did not complete.
  bool may_have_applied{};
  auto operator==(const ExactSourceEditError&) const -> bool = default;
};

class ExactSourceEditor {
 public:
  virtual ~ExactSourceEditor() = default;

  [[nodiscard]] virtual auto read(ExactSourceReadRequest request,
                                  std::stop_token stop_token = {})
      -> std::expected<ExactSourceReadResult, ExactSourceEditError> = 0;

  [[nodiscard]] virtual auto apply(ExactSourceEditRequest request,
                                   std::stop_token stop_token = {})
      -> std::expected<ExactSourceEditReceipt, ExactSourceEditError> = 0;
};

[[nodiscard]] auto validate_exact_source_read_request(
    const ExactSourceReadRequest& request)
    -> std::expected<void, ExactSourceEditError>;

[[nodiscard]] auto validate_exact_source_edit_request(
    const ExactSourceEditRequest& request)
    -> std::expected<void, ExactSourceEditError>;

[[nodiscard]] auto validate_exact_source_edit_receipt(
    const ExactSourceEditRequest& request,
    const ExactSourceEditReceipt& receipt)
    -> std::expected<void, ExactSourceEditError>;

} // namespace aiforge::repository
