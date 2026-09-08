#include <aiforge/runtime/local_source.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace aiforge {
namespace {
using Code = domain::LocalSourceErrorCode;
using Status = std::expected<void, domain::LocalSourceError>;
auto failure(Code code, std::string message) -> Status {
  return std::unexpected(domain::LocalSourceError{code, std::move(message)});
}
auto hex_digest(std::string_view value) -> bool {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}
auto raw_path(std::string_view path, bool allow_root,
              const domain::LocalSourceLimits& limits) -> bool {
  if (path.empty()) return allow_root;
  if (path.size() > limits.maximum_path_bytes || path.front() == '/' ||
      path.back() == '/' || path.find('\0') != std::string_view::npos)
    return false;
  std::size_t begin{};
  std::size_t depth{};
  while (begin < path.size()) {
    const auto end = path.find('/', begin);
    const auto part =
        path.substr(begin, end == std::string_view::npos ? end : end - begin);
    if (part.empty() || part == "." || part == ".." || part.size() > 255 ||
        ++depth > limits.maximum_depth)
      return false;
    if (end == std::string_view::npos) return true;
    begin = end + 1;
  }
  return false;
}
auto supported_name(std::string_view path) -> bool {
  return detail::is_safe_utf8_text(path) &&
         path.find_first_of("\r\n\t") == std::string_view::npos;
}
auto token(const runtime::LocalSourceRequestToken& value) -> Status {
  if (auto valid = domain::validate_local_root_identity(value.root); !valid)
    return valid;
  if (value.lease_generation == 0 || value.request_id == 0 ||
      value.selection_revision == 0)
    return failure(Code::invalid_request, "local request token is invalid");
  return {};
}
auto request_path(const runtime::LocalSourceRequestToken& value,
                  std::string_view path, bool allow_root,
                  const domain::LocalSourceLimits& limits) -> Status {
  if (auto valid = token(value); !valid) return valid;
  if (auto valid =
          domain::validate_local_relative_path(path, allow_root, limits);
      !valid)
    return valid;
  if (!path.empty() && !supported_name(path))
    return failure(Code::unsupported_entry, "local path cannot be selected");
  return {};
}
auto binding(const runtime::LocalSourceRequestToken& request,
             const runtime::LocalSourceRequestToken& result) -> Status {
  if (request != result)
    return failure(Code::stale_lease,
                   "local result does not match its request");
  return {};
}
auto exact_text(const domain::LocalSourceIdentity& source,
                std::string_view text, const domain::LocalSourceLimits& limits)
    -> Status {
  if (auto valid = domain::validate_local_source_identity(source, limits);
      !valid)
    return valid;
  if (text.size() > limits.maximum_file_bytes ||
      text.size() != source.content_digest.byte_size)
    return failure(Code::invalid_result, "local source size does not match");
  if (!text.empty() && !detail::is_safe_utf8_text(text))
    return failure(Code::invalid_text, "local source is not supported text");
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  if (hash.finish() != source.content_digest.value)
    return failure(Code::source_mismatch, "local source bytes do not match");
  return {};
}
auto immediate_name(std::string_view directory, std::string_view path)
    -> std::optional<std::string_view> {
  if (!directory.empty()) {
    if (!path.starts_with(directory) || path.size() <= directory.size() ||
        path[directory.size()] != '/')
      return std::nullopt;
    path.remove_prefix(directory.size() + 1);
  }
  if (path.empty() || path.find('/') != std::string_view::npos)
    return std::nullopt;
  return path;
}
auto known_kind(runtime::LocalEntryKind kind) -> bool {
  using enum runtime::LocalEntryKind;
  switch (kind) {
    case regular_file:
    case directory:
    case symbolic_link:
    case unsupported: return true;
  }
  return false;
}
auto listing_counts(const runtime::LocalListRequest& request,
                    const runtime::LocalListResult& result) -> Status {
  if (result.state != runtime::LocalListingState::complete &&
      result.state != runtime::LocalListingState::partial)
    return failure(Code::invalid_result, "local listing completion is invalid");
  if (result.entries.size() > request.limits.maximum_list_entries ||
      result.scanned_entries > request.limits.maximum_scanned_entries ||
      result.entries.size() > result.scanned_entries)
    return failure(Code::resource_exhausted,
                   "local listing exceeds entry limits");
  auto remainder = result.scanned_entries - result.entries.size();
  if (result.skipped_unsupported_entries > remainder)
    return failure(Code::invalid_result,
                   "local listing skipped counts are invalid");
  remainder -= result.skipped_unsupported_entries;
  if (result.skipped_filtered_entries > remainder ||
      (request.filename_filter.empty() && result.skipped_filtered_entries != 0))
    return failure(Code::invalid_result,
                   "local listing scan counts are incomplete");
  remainder -= result.skipped_filtered_entries;
  if (result.skipped_capacity_entries != remainder ||
      (result.skipped_capacity_entries != 0 &&
       result.state != runtime::LocalListingState::partial))
    return failure(Code::invalid_result,
                   "local listing capacity counts are invalid");
  return {};
}
} // namespace

namespace domain {
auto validate_local_source_limits(const LocalSourceLimits& limits) -> Status {
  const LocalSourceLimits maximum;
  const std::array<std::pair<std::uint64_t, std::uint64_t>, 10> bounds{
      {{limits.maximum_roots, maximum.maximum_roots},
       {limits.maximum_selected_files, maximum.maximum_selected_files},
       {limits.maximum_file_bytes, maximum.maximum_file_bytes},
       {limits.maximum_total_bytes, maximum.maximum_total_bytes},
       {limits.maximum_path_bytes, maximum.maximum_path_bytes},
       {limits.maximum_depth, maximum.maximum_depth},
       {limits.maximum_list_entries, maximum.maximum_list_entries},
       {limits.maximum_scanned_entries, maximum.maximum_scanned_entries},
       {limits.maximum_listing_bytes, maximum.maximum_listing_bytes},
       {limits.maximum_preview_bytes, maximum.maximum_file_bytes}}};
  for (const auto& [value, ceiling] : bounds)
    if (value == 0 || value > ceiling)
      return failure(Code::invalid_request, "local source limits are invalid");
  if (limits.maximum_preview_bytes > limits.maximum_file_bytes ||
      limits.maximum_file_bytes > limits.maximum_total_bytes ||
      limits.maximum_list_entries > limits.maximum_scanned_entries ||
      limits.timeout <= std::chrono::milliseconds{0} ||
      limits.timeout > maximum.timeout)
    return failure(Code::invalid_request,
                   "local source limits are inconsistent");
  return {};
}
auto validate_local_root_identity(const LocalRootIdentity& root) -> Status {
  if (root.version != 1)
    return failure(Code::unsupported_version,
                   "local root version is unsupported");
  if (!hex_digest(root.binding))
    return failure(Code::invalid_request, "local root identity is invalid");
  return {};
}
auto validate_local_relative_path(std::string_view path, bool allow_root,
                                  const LocalSourceLimits& limits) -> Status {
  if (auto valid = validate_local_source_limits(limits); !valid) return valid;
  if (!raw_path(path, allow_root, limits))
    return failure(Code::invalid_request, "local relative path is invalid");
  return {};
}
auto validate_local_source_identity(const LocalSourceIdentity& source,
                                    const LocalSourceLimits& limits) -> Status {
  if (auto valid = validate_local_root_identity(source.root); !valid)
    return valid;
  if (auto valid =
          validate_local_relative_path(source.relative_path, false, limits);
      !valid)
    return valid;
  if (!supported_name(source.relative_path))
    return failure(Code::unsupported_entry, "local source path is unsupported");
  if (source.content_digest.algorithm != "sha256" ||
      !hex_digest(source.content_digest.value) ||
      source.content_digest.byte_size > limits.maximum_file_bytes)
    return failure(Code::invalid_request, "local source digest is invalid");
  return {};
}
auto validate_local_source_selection(
    std::span<const LocalSourceIdentity> sources,
    const LocalSourceLimits& limits) -> Status {
  try {
    if (auto valid = validate_local_source_limits(limits); !valid) return valid;
    if (sources.size() > limits.maximum_selected_files)
      return failure(Code::resource_exhausted,
                     "local selection exceeds file limit");
    std::set<std::string_view> roots;
    std::set<std::pair<std::string_view, std::string_view>> paths;
    std::uint64_t bytes{};
    for (const auto& source : sources) {
      if (auto valid = validate_local_source_identity(source, limits); !valid)
        return valid;
      if (source.content_digest.byte_size == 0)
        return failure(Code::unavailable,
                       "empty local source cannot be selected as evidence");
      if (!paths.emplace(source.root.binding, source.relative_path).second)
        return failure(Code::invalid_request,
                       "local selection repeats a source");
      roots.insert(source.root.binding);
      if (roots.size() > limits.maximum_roots ||
          source.content_digest.byte_size > limits.maximum_total_bytes - bytes)
        return failure(Code::resource_exhausted,
                       "local selection exceeds aggregate limits");
      bytes += source.content_digest.byte_size;
    }
    return {};
  } catch (...) {
    return failure(Code::internal_failure, "local selection validation failed");
  }
}
} // namespace domain

namespace runtime {
auto validate_local_list_request(const LocalListRequest& request) -> Status {
  if (auto valid =
          request_path(request.token, request.directory, true, request.limits);
      !valid)
    return valid;
  if (request.filename_filter.size() > 255 ||
      (!request.filename_filter.empty() &&
       !supported_name(request.filename_filter)) ||
      request.filename_filter.find('/') != std::string::npos)
    return failure(Code::invalid_request, "local filename filter is invalid");
  return {};
}
auto validate_local_list_result(const LocalListRequest& request,
                                const LocalListResult& result) -> Status {
  try {
    if (auto valid = validate_local_list_request(request); !valid) return valid;
    if (auto valid = binding(request.token, result.token); !valid) return valid;
    if (result.directory != request.directory)
      return failure(Code::invalid_result,
                     "local listing directory does not match");
    if (auto valid = listing_counts(request, result); !valid) return valid;
    std::size_t bytes{};
    std::set<std::string_view> paths;
    for (const auto& entry : result.entries) {
      if (!raw_path(entry.relative_path, false, request.limits) ||
          !known_kind(entry.kind))
        return failure(Code::invalid_result, "local listing entry is invalid");
      const auto name = immediate_name(request.directory, entry.relative_path);
      if (!name ||
          name->find(request.filename_filter) == std::string_view::npos ||
          !paths.insert(entry.relative_path).second)
        return failure(Code::invalid_result,
                       "local listing membership is invalid");
      if (!supported_name(entry.relative_path) &&
          entry.kind != LocalEntryKind::unsupported)
        return failure(Code::invalid_result,
                       "unsupported local name was not marked");
      if (entry.relative_path.size() >
          request.limits.maximum_listing_bytes - bytes)
        return failure(Code::resource_exhausted,
                       "local listing exceeds byte limit");
      bytes += entry.relative_path.size();
    }
    return {};
  } catch (...) {
    return failure(Code::internal_failure, "local listing validation failed");
  }
}
auto validate_local_preview_request(const LocalPreviewRequest& request)
    -> Status {
  if (auto valid = request_path(request.token, request.relative_path, false,
                                request.limits);
      !valid)
    return valid;
  if (request.mode != LocalPreviewMode::bounded_prefix &&
      request.mode != LocalPreviewMode::exact)
    return failure(Code::invalid_request, "local preview mode is invalid");
  return {};
}
auto validate_local_preview_result(const LocalPreviewRequest& request,
                                   const LocalPreviewResult& result) -> Status {
  try {
    if (auto valid = validate_local_preview_request(request); !valid)
      return valid;
    if (auto valid = binding(request.token, result.token); !valid) return valid;
    if (result.relative_path != request.relative_path)
      return failure(Code::invalid_result, "local preview path does not match");
    const auto limit = request.mode == LocalPreviewMode::exact
                           ? request.limits.maximum_file_bytes
                           : request.limits.maximum_preview_bytes;
    if (result.text.size() > limit)
      return failure(Code::resource_exhausted,
                     "local preview exceeds byte limit");
    if (!result.text.empty() && !detail::is_safe_utf8_text(result.text))
      return failure(Code::invalid_text, "local preview is not supported text");
    if (result.state == LocalPreviewState::prefix) {
      if (request.mode != LocalPreviewMode::bounded_prefix || result.source ||
          result.text.empty() ||
          result.text.size() >= result.observed_file_bytes)
        return failure(Code::invalid_result,
                       "local prefix completeness is invalid");
      return {};
    }
    if (result.state != LocalPreviewState::complete || !result.source ||
        result.source->root != request.token.root ||
        result.source->relative_path != request.relative_path ||
        result.observed_file_bytes != result.text.size())
      return failure(Code::invalid_result,
                     "local complete preview proof is invalid");
    return exact_text(*result.source, result.text, request.limits);
  } catch (...) {
    return failure(Code::internal_failure, "local preview validation failed");
  }
}
auto validate_local_revalidate_request(const LocalRevalidateRequest& request)
    -> Status {
  if (auto valid = token(request.token); !valid) return valid;
  if (auto valid = domain::validate_local_source_identity(
          request.expected_source, request.limits);
      !valid)
    return valid;
  if (request.token.root != request.expected_source.root)
    return failure(Code::source_mismatch, "local recovery root does not match");
  return {};
}
auto validate_local_read_result(const LocalRevalidateRequest& request,
                                const LocalReadResult& result) -> Status {
  try {
    if (auto valid = validate_local_revalidate_request(request); !valid)
      return valid;
    if (auto valid = binding(request.token, result.token); !valid) return valid;
    if (request.expected_source != result.source)
      return failure(Code::source_mismatch,
                     "local recovery source does not match");
    return exact_text(result.source, result.text, request.limits);
  } catch (...) {
    return failure(Code::internal_failure, "local recovery validation failed");
  }
}
} // namespace runtime
} // namespace aiforge
