#include <aiforge/runtime/local_source_grant.hpp>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::runtime {
namespace {
auto validate_request(const LocalFolderGrantRequest& request)
    -> std::expected<void, domain::LocalSourceError> {
  if (auto valid = domain::validate_local_source_limits(request.limits); !valid)
    return valid;
  const auto& path = request.absolute_path;
  if (request.token.session_epoch == 0 || request.token.request_id == 0 ||
      request.lease_generation == 0 || path.empty() || path.front() != '/' ||
      path.size() > request.limits.maximum_path_bytes ||
      !detail::is_safe_utf8_text(path) ||
      path.find_first_of("\r\n\t") != std::string::npos)
    return std::unexpected(
        domain::LocalSourceError{domain::LocalSourceErrorCode::invalid_request,
                                 "local folder grant request is invalid"});
  return {};
}
auto validate_result(const LocalFolderGrantRequest& request,
                     const LocalFolderGrantResult& result)
    -> std::expected<void, domain::LocalSourceError> {
  if (auto valid = validate_request(request); !valid) return valid;
  if (result.token != request.token || !result.lease ||
      !domain::validate_local_root_identity(result.root) ||
      !result.lease->guarantees_pinned_read_only_sources() ||
      result.lease->root_identity() != result.root ||
      result.lease->session_id() != request.token.session_id ||
      result.lease->lease_generation() != request.lease_generation)
    return std::unexpected(
        domain::LocalSourceError{domain::LocalSourceErrorCode::invalid_result,
                                 "local folder grant result is invalid"});
  return {};
}
} // namespace

auto validate_local_folder_grant_request(const LocalFolderGrantRequest& request)
    -> std::expected<void, domain::LocalSourceError> {
  try {
    return validate_request(request);
  } catch (...) {
    return std::unexpected(
        domain::LocalSourceError{domain::LocalSourceErrorCode::internal_failure,
                                 "local grant validation failed internally"});
  }
}
auto validate_local_folder_grant_result(const LocalFolderGrantRequest& request,
                                        const LocalFolderGrantResult& result)
    -> std::expected<void, domain::LocalSourceError> {
  try {
    return validate_result(request, result);
  } catch (...) {
    return std::unexpected(
        domain::LocalSourceError{domain::LocalSourceErrorCode::internal_failure,
                                 "local grant validation failed internally"});
  }
}
} // namespace aiforge::runtime
