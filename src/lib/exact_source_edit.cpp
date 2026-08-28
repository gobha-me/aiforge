#include <aiforge/repository/exact_source_edit.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <ranges>
#include <string_view>

namespace aiforge::repository {
namespace {

[[nodiscard]] auto failure(
    const ExactSourceEditErrorCode code, std::string message,
    std::optional<domain::RepositorySnapshotIdentity> observed_snapshot = {},
    std::optional<domain::RepositorySourceIdentity> observed_source = {},
    const bool retryable = false, const bool may_have_applied = false)
    -> std::unexpected<ExactSourceEditError> {
  return std::unexpected(ExactSourceEditError{
      code, std::move(message), std::move(observed_snapshot),
      std::move(observed_source), retryable, may_have_applied});
}

[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         std::ranges::none_of(value, [](const unsigned char character) {
           return character == 0 || character == 0x7FU;
         });
}

[[nodiscard]] auto valid_relative_path(const std::string& value,
                                       const std::size_t maximum) -> bool {
  if (!bounded_text(value, maximum)) return false;
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.generic_string() != value || path.lexically_normal() != path) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& component) {
    return component.empty() || component == "." || component == "..";
  });
}

[[nodiscard]] auto valid_digest(const domain::ContentDigest& digest,
                                const std::uint64_t maximum_bytes) -> bool {
  if (!bounded_text(digest.algorithm, 128) ||
      !bounded_text(digest.value, 512) || digest.byte_size > maximum_bytes) {
    return false;
  }
  return std::ranges::all_of(digest.algorithm,
                             [](const unsigned char value) {
                               return std::isalnum(value) != 0 ||
                                      value == '-' || value == '_' ||
                                      value == '.';
                             }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] auto valid_limits(const ExactSourceEditLimits& limits) -> bool {
  constexpr ExactSourceEditLimits maximums;
  return limits.maximum_path_bytes > 0 && limits.maximum_source_bytes > 0 &&
         limits.maximum_replacement_bytes > 0 &&
         limits.timeout > std::chrono::milliseconds::zero() &&
         limits.maximum_path_bytes <= maximums.maximum_path_bytes &&
         limits.maximum_source_bytes <= maximums.maximum_source_bytes &&
         limits.maximum_replacement_bytes <=
             maximums.maximum_replacement_bytes &&
         limits.timeout <= maximums.timeout;
}

[[nodiscard]] auto validate_baseline(const domain::RepositorySnapshot& baseline,
                                     const ExactSourceEditLimits& limits)
    -> std::expected<void, ExactSourceEditError> {
  RepositorySnapshotLimits snapshot_limits;
  snapshot_limits.maximum_path_bytes = limits.maximum_path_bytes;
  snapshot_limits.maximum_file_bytes = limits.maximum_source_bytes;
  snapshot_limits.observation_timeout = limits.timeout;
  snapshot_limits.command_timeout =
      std::min(snapshot_limits.command_timeout, limits.timeout);
  const auto valid = validate_repository_snapshot(baseline, snapshot_limits);
  if (!valid) {
    return failure(ExactSourceEditErrorCode::invalid_request,
                   "exact-source baseline is invalid");
  }
  return {};
}

} // namespace

auto validate_exact_source_read_request(const ExactSourceReadRequest& request)
    -> std::expected<void, ExactSourceEditError> {
  if (!valid_limits(request.limits) ||
      !valid_relative_path(request.relative_path,
                           request.limits.maximum_path_bytes)) {
    return failure(ExactSourceEditErrorCode::invalid_request,
                   "exact-source read request is invalid");
  }
  return validate_baseline(request.baseline, request.limits);
}

auto validate_exact_source_edit_request(const ExactSourceEditRequest& request)
    -> std::expected<void, ExactSourceEditError> {
  if (!valid_limits(request.limits)) {
    return failure(ExactSourceEditErrorCode::invalid_request,
                   "exact-source edit request is invalid");
  }
  if (request.replacement.size() > request.limits.maximum_replacement_bytes) {
    return failure(ExactSourceEditErrorCode::resource_exhausted,
                   "exact-source replacement exceeds its byte budget");
  }
  auto baseline = validate_baseline(request.baseline, request.limits);
  if (!baseline) return baseline;
  const auto& source = request.expected_source;
  if (!same_source_state(source.snapshot,
                         snapshot_identity(request.baseline)) ||
      !valid_relative_path(source.relative_path,
                           request.limits.maximum_path_bytes) ||
      !valid_digest(source.content_digest,
                    request.limits.maximum_source_bytes) ||
      source.range || request.range.begin > request.range.end ||
      request.range.end > source.content_digest.byte_size) {
    return failure(ExactSourceEditErrorCode::invalid_request,
                   "exact-source edit precondition is invalid");
  }
  const auto removed = request.range.end - request.range.begin;
  if (request.replacement.size() >
      std::numeric_limits<std::uint64_t>::max() -
          (source.content_digest.byte_size - removed)) {
    return failure(ExactSourceEditErrorCode::resource_exhausted,
                   "exact-source edit size overflowed");
  }
  const auto result_size =
      source.content_digest.byte_size - removed + request.replacement.size();
  if (result_size > request.limits.maximum_source_bytes) {
    return failure(ExactSourceEditErrorCode::resource_exhausted,
                   "exact-source edit exceeds its source byte budget");
  }
  return {};
}

auto validate_exact_source_edit_receipt(const ExactSourceEditRequest& request,
                                        const ExactSourceEditReceipt& receipt)
    -> std::expected<void, ExactSourceEditError> {
  auto valid = validate_exact_source_edit_request(request);
  if (!valid) return valid;
  const auto expected_after_size =
      request.expected_source.content_digest.byte_size -
      (request.range.end - request.range.begin) + request.replacement.size();
  const auto replacement_end = request.range.begin + request.replacement.size();
  if (receipt.previous_source != request.expected_source ||
      receipt.replaced_range != request.range ||
      receipt.resulting_range !=
          domain::SourceByteRange{request.range.begin, replacement_end} ||
      !same_source_state(receipt.before_snapshot,
                         request.expected_source.snapshot) ||
      receipt.resulting_source.snapshot != receipt.after_snapshot ||
      receipt.resulting_source.snapshot.repository_id !=
          request.expected_source.snapshot.repository_id ||
      receipt.resulting_source.relative_path !=
          request.expected_source.relative_path ||
      receipt.resulting_source.range ||
      receipt.resulting_source.content_digest.byte_size !=
          expected_after_size ||
      !valid_digest(receipt.resulting_source.content_digest,
                    request.limits.maximum_source_bytes)) {
    return failure(ExactSourceEditErrorCode::internal_failure,
                   "exact-source edit receipt is inconsistent");
  }
  return {};
}

} // namespace aiforge::repository
