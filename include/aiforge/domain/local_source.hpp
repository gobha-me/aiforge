#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <aiforge/domain/digest.hpp>

namespace aiforge::domain {

// Version 1 is an opaque SHA256 physical host/mount/root binding. It is not
// a portable pathname, an OS descriptor, or permission to reopen the root.
struct LocalRootIdentity {
  std::uint32_t version{1};
  std::string binding;
  auto operator==(const LocalRootIdentity&) const -> bool = default;
};

struct LocalSourceIdentity {
  LocalRootIdentity root;
  // Raw root-relative identity, never a display label. No normalization on
  // read.
  std::string relative_path;
  // Exact whole-file SHA256. File inode is deliberately not durable identity.
  ContentDigest content_digest;
  auto operator==(const LocalSourceIdentity&) const -> bool = default;
};

struct LocalSourceLimits {
  std::size_t maximum_roots{16};
  std::size_t maximum_selected_files{64};
  std::uint64_t maximum_file_bytes{std::uint64_t{256} * 1024};
  std::uint64_t maximum_total_bytes{std::uint64_t{2} * 1024 * 1024};
  std::size_t maximum_path_bytes{4096};
  std::size_t maximum_depth{64};
  std::size_t maximum_list_entries{256};
  std::size_t maximum_scanned_entries{4096};
  std::size_t maximum_listing_bytes{std::size_t{256} * 1024};
  std::uint64_t maximum_preview_bytes{std::uint64_t{64} * 1024};
  std::chrono::milliseconds timeout{30000};
  auto operator==(const LocalSourceLimits&) const -> bool = default;
};

enum class LocalSourceErrorCode {
  invalid_request,
  invalid_result,
  unsupported_version,
  unavailable,
  permission_denied,
  unsupported_entry,
  invalid_text,
  source_mismatch,
  stale_lease,
  concurrent_change,
  resource_exhausted,
  cancelled,
  timed_out,
  io_failure,
  internal_failure,
};

struct LocalSourceError {
  LocalSourceErrorCode code;
  // Safe application message, never raw paths, OS errors or file content.
  std::string message;
  auto operator==(const LocalSourceError&) const -> bool = default;
};

[[nodiscard]] auto validate_local_source_limits(const LocalSourceLimits& limits)
    -> std::expected<void, LocalSourceError>;
[[nodiscard]] auto validate_local_root_identity(const LocalRootIdentity& root)
    -> std::expected<void, LocalSourceError>;
[[nodiscard]] auto validate_local_relative_path(
    std::string_view path, bool allow_root = false,
    const LocalSourceLimits& limits = {})
    -> std::expected<void, LocalSourceError>;
[[nodiscard]] auto validate_local_source_identity(
    const LocalSourceIdentity& source, const LocalSourceLimits& limits = {})
    -> std::expected<void, LocalSourceError>;
// Bounds and uniqueness across a future tray; performs no reads or grants.
[[nodiscard]] auto validate_local_source_selection(
    std::span<const LocalSourceIdentity> sources,
    const LocalSourceLimits& limits = {})
    -> std::expected<void, LocalSourceError>;

} // namespace aiforge::domain
