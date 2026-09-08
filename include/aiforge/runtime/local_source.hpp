#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/ids.hpp>
#include <aiforge/domain/local_source.hpp>

namespace aiforge::runtime {

struct LocalSourceRequestToken {
  domain::SessionId session_id;
  domain::LocalRootIdentity root;
  // Live generation is a revocation/stale-worker guard, not source identity.
  std::uint64_t lease_generation{};
  std::uint64_t request_id{};
  std::uint64_t selection_revision{};
  auto operator==(const LocalSourceRequestToken&) const -> bool = default;
};

enum class LocalListingState { complete, partial };
enum class LocalEntryKind {
  regular_file,
  directory,
  symbolic_link,
  unsupported
};
enum class LocalPreviewMode { bounded_prefix, exact };
enum class LocalPreviewState { complete, prefix };

struct LocalDirectoryEntry {
  // Raw identity only: controls/non-UTF8 names must be unsupported and escaped
  // by the eventual display adapter. Never write this field to a terminal.
  std::string relative_path;
  LocalEntryKind kind{LocalEntryKind::unsupported};
  auto operator==(const LocalDirectoryEntry&) const -> bool = default;
};

struct LocalListRequest {
  LocalSourceRequestToken token;
  // Empty means root. Initial bounded observations have no resume cursor.
  std::string directory;
  domain::LocalSourceLimits limits;
  // Bounded plain filename substring, applied during the bounded scan.
  std::string filename_filter{};
  auto operator==(const LocalListRequest&) const -> bool = default;
};
struct LocalListResult {
  LocalSourceRequestToken token;
  std::string directory;
  std::vector<LocalDirectoryEntry> entries;
  // Counts all inspected names, including skipped/unsupported entries.
  std::size_t scanned_entries{};
  // No claim of global sort/completeness beyond this bounded observation.
  LocalListingState state{LocalListingState::partial};
  std::size_t skipped_unsupported_entries{};
  std::size_t skipped_filtered_entries{};
  // Inspected valid names that could not fit output bounds. Requires partial.
  std::size_t skipped_capacity_entries{};
  auto operator==(const LocalListResult&) const -> bool = default;
};

struct LocalPreviewRequest {
  LocalSourceRequestToken token;
  std::string relative_path;
  LocalPreviewMode mode{LocalPreviewMode::bounded_prefix};
  domain::LocalSourceLimits limits;
  auto operator==(const LocalPreviewRequest&) const -> bool = default;
};
struct LocalPreviewResult {
  LocalSourceRequestToken token;
  std::string relative_path;
  std::uint64_t observed_file_bytes{};
  std::string text;
  LocalPreviewState state{LocalPreviewState::prefix};
  // Prefix previews cannot claim an exact whole-file source identity.
  std::optional<domain::LocalSourceIdentity> source;
  auto operator==(const LocalPreviewResult&) const -> bool = default;
};

struct LocalRevalidateRequest {
  LocalSourceRequestToken token;
  domain::LocalSourceIdentity expected_source;
  domain::LocalSourceLimits limits;
  auto operator==(const LocalRevalidateRequest&) const -> bool = default;
};
struct LocalReadResult {
  LocalSourceRequestToken token;
  domain::LocalSourceIdentity source;
  std::string text;
  auto operator==(const LocalReadResult&) const -> bool = default;
};

[[nodiscard]] auto validate_local_list_request(const LocalListRequest& request)
    -> std::expected<void, domain::LocalSourceError>;
[[nodiscard]] auto validate_local_list_result(const LocalListRequest& request,
                                              const LocalListResult& result)
    -> std::expected<void, domain::LocalSourceError>;
[[nodiscard]] auto validate_local_preview_request(
    const LocalPreviewRequest& request)
    -> std::expected<void, domain::LocalSourceError>;
[[nodiscard]] auto validate_local_preview_result(
    const LocalPreviewRequest& request, const LocalPreviewResult& result)
    -> std::expected<void, domain::LocalSourceError>;
[[nodiscard]] auto validate_local_revalidate_request(
    const LocalRevalidateRequest& request)
    -> std::expected<void, domain::LocalSourceError>;
[[nodiscard]] auto validate_local_read_result(
    const LocalRevalidateRequest& request, const LocalReadResult& result)
    -> std::expected<void, domain::LocalSourceError>;

// A lease-owning implementation checks current authority as well as these
// value contracts. A caller cannot mint permission by constructing a token.
// No filesystem, instruction discovery, edit, execute or tool-approval API.
// Methods reject revoked leases and source races. Stop/deadline checks bound
// work between OS calls; they do not promise to interrupt a stalled OS call.
class LocalSourceReader {
 public:
  virtual ~LocalSourceReader() = default;
  [[nodiscard]] virtual auto guarantees_pinned_read_only_sources()
      const noexcept -> bool {
    return false;
  }
  [[nodiscard]] virtual auto list(LocalListRequest request,
                                  std::stop_token stop = {})
      -> std::expected<LocalListResult, domain::LocalSourceError> = 0;
  [[nodiscard]] virtual auto preview(LocalPreviewRequest request,
                                     std::stop_token stop = {})
      -> std::expected<LocalPreviewResult, domain::LocalSourceError> = 0;
  [[nodiscard]] virtual auto revalidate(LocalRevalidateRequest request,
                                        std::stop_token stop = {})
      -> std::expected<LocalReadResult, domain::LocalSourceError> = 0;
};

} // namespace aiforge::runtime
