#include <aiforge/runtime/repository_read_tool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/tool_policy.hpp>

#include <nlohmann/json.hpp>

namespace aiforge::runtime {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumArgumentBytes{std::size_t{64U} * 1024U};
constexpr std::size_t kMaximumResultBytes{std::size_t{4U} * 1024U * 1024U};
constexpr std::uint64_t kMaximumSourceBytes{std::uint64_t{1024U} * 1024U};
constexpr repository::RepositorySnapshotLimits kSnapshotMaximums{
    16384,
    4096,
    kMaximumSourceBytes,
    std::uint64_t{64U} * 1024U * 1024U,
    std::size_t{4U} * 1024U * 1024U,
    std::chrono::seconds{5},
    std::chrono::seconds{15}};

class DuplicateJsonKey final : public std::exception {};

[[nodiscard]] auto registry_error(std::string message)
    -> std::unexpected<ToolRegistryError> {
  return std::unexpected(ToolRegistryError{
      ToolRegistryErrorCode::invalid_declaration, std::move(message)});
}

[[nodiscard]] auto execution_error(const ToolExecutionErrorCode code,
                                   std::string message,
                                   const bool retryable = false)
    -> std::unexpected<ToolExecutionError> {
  return std::unexpected(
      ToolExecutionError{code, std::move(message), retryable});
}

[[nodiscard]] auto read_scope(const std::string& root)
    -> domain::CapabilityScope {
  return {domain::Effect::read, "filesystem.root", root};
}

[[nodiscard]] auto requested_read_scope(const std::string& root,
                                        const std::string& relative_path)
    -> domain::CapabilityScope {
  return read_scope(
      (std::filesystem::path{root} / relative_path).generic_string());
}

[[nodiscard]] auto valid_configuration(
    const RepositoryReadToolConfiguration& configuration)
    -> std::expected<RepositoryReadToolConfiguration, ToolRegistryError> {
  constexpr repository::ExactSourceEditLimits read_maximums;
  const auto& snapshot = configuration.snapshot_limits;
  const auto& read = configuration.read_limits;
  if (configuration.maximum_argument_bytes == 0 ||
      configuration.maximum_argument_bytes > kMaximumArgumentBytes ||
      configuration.maximum_result_bytes == 0 ||
      configuration.maximum_result_bytes > kMaximumResultBytes ||
      snapshot.maximum_entries == 0 || snapshot.maximum_path_bytes == 0 ||
      snapshot.maximum_file_bytes == 0 || snapshot.maximum_total_bytes == 0 ||
      snapshot.maximum_command_output_bytes == 0 ||
      snapshot.command_timeout <= std::chrono::milliseconds::zero() ||
      snapshot.observation_timeout <= std::chrono::milliseconds::zero() ||
      snapshot.maximum_entries > kSnapshotMaximums.maximum_entries ||
      snapshot.maximum_path_bytes > kSnapshotMaximums.maximum_path_bytes ||
      snapshot.maximum_file_bytes > kSnapshotMaximums.maximum_file_bytes ||
      snapshot.maximum_total_bytes > kSnapshotMaximums.maximum_total_bytes ||
      snapshot.maximum_command_output_bytes >
          kSnapshotMaximums.maximum_command_output_bytes ||
      snapshot.command_timeout > kSnapshotMaximums.command_timeout ||
      snapshot.observation_timeout > kSnapshotMaximums.observation_timeout ||
      snapshot.command_timeout > snapshot.observation_timeout ||
      read.maximum_path_bytes == 0 || read.maximum_source_bytes == 0 ||
      read.maximum_replacement_bytes == 0 ||
      read.timeout <= std::chrono::milliseconds::zero() ||
      read.maximum_path_bytes > read_maximums.maximum_path_bytes ||
      read.maximum_source_bytes > kMaximumSourceBytes ||
      read.maximum_replacement_bytes >
          read_maximums.maximum_replacement_bytes ||
      read.timeout > read_maximums.timeout ||
      snapshot.maximum_path_bytes > read.maximum_path_bytes) {
    return registry_error("repository-read limits are invalid or unbounded");
  }
  if (!detail::is_safe_utf8_text(configuration.repository_root)) {
    return registry_error("repository-read root contains unsafe text");
  }
  auto normalized =
      normalize_capability_scope(read_scope(configuration.repository_root));
  if (!normalized || normalized->value != configuration.repository_root) {
    return registry_error(
        "repository-read root must be an absolute normalized path");
  }
  return configuration;
}

[[nodiscard]] auto parse_json(const std::string& text) -> Json {
  std::vector<std::set<std::string>> keys;
  const auto callback = [&keys](const int, const Json::parse_event_t event,
                                Json& parsed) {
    if (event == Json::parse_event_t::object_start) {
      keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (keys.empty() ||
          !keys.back().insert(parsed.get<std::string>()).second) {
        throw DuplicateJsonKey{};
      }
    } else if (event == Json::parse_event_t::object_end) {
      keys.pop_back();
    }
    return true;
  };
  return Json::parse(text, callback, true, false);
}

[[nodiscard]] auto valid_relative_path(const std::string& value,
                                       const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum ||
      !detail::is_safe_utf8_text(value) ||
      std::ranges::any_of(value, [](const unsigned char character) {
        return character < 0x20U || character == 0x7fU;
      })) {
    return false;
  }
  if (value.front() == '/' || value.back() == '/' ||
      value.find("//") != std::string::npos ||
      value.find('\\') != std::string::npos) {
    return false;
  }
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.generic_string() != value || path.lexically_normal() != path) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& component) {
    return component.empty() || component == "." || component == ".." ||
           component == ".git";
  });
}

[[nodiscard]] auto parse_relative_path(
    const domain::StructuredDataBlock& arguments,
    const RepositoryReadToolConfiguration& configuration)
    -> std::expected<std::string, ToolExecutionError> {
  if (arguments.media_type != "application/json" || arguments.data.empty() ||
      arguments.data.size() > configuration.maximum_argument_bytes) {
    return execution_error(ToolExecutionErrorCode::invalid_arguments,
                           "repository-read arguments must be bounded JSON");
  }
  try {
    const auto root = parse_json(arguments.data);
    if (!root.is_object() || root.size() != 1 ||
        !root.contains("relative_path") ||
        !root.at("relative_path").is_string()) {
      return execution_error(
          ToolExecutionErrorCode::invalid_arguments,
          "repository-read arguments require only a relative_path string");
    }
    auto path = root.at("relative_path").get<std::string>();
    if (!valid_relative_path(path,
                             configuration.read_limits.maximum_path_bytes)) {
      return execution_error(
          ToolExecutionErrorCode::invalid_arguments,
          "repository-read relative_path is invalid or oversized");
    }
    return path;
  } catch (...) {
    return execution_error(ToolExecutionErrorCode::invalid_arguments,
                           "repository-read arguments are malformed");
  }
}

[[nodiscard]] auto snapshot_failure(
    const repository::RepositorySnapshotError& error)
    -> std::unexpected<ToolExecutionError> {
  using Code = repository::RepositorySnapshotErrorCode;
  switch (error.code) {
    case Code::cancelled:
      return execution_error(ToolExecutionErrorCode::cancelled,
                             "repository observation was cancelled");
    case Code::timed_out:
      return execution_error(ToolExecutionErrorCode::timed_out,
                             "repository observation timed out", true);
    case Code::resource_exhausted:
      return execution_error(ToolExecutionErrorCode::output_limit,
                             "repository observation exceeded its limits");
    case Code::invalid_request:
      return execution_error(ToolExecutionErrorCode::protocol_failure,
                             "repository observation request was rejected");
    case Code::not_found:
    case Code::not_directory:
    case Code::permission_denied:
    case Code::unsupported_entry:
    case Code::unstable:
    case Code::vcs_failure:
    case Code::io_failure:
    case Code::internal_failure:
      return execution_error(ToolExecutionErrorCode::unavailable,
                             "repository could not be observed",
                             error.retryable);
  }
  return execution_error(ToolExecutionErrorCode::internal_failure,
                         "repository observation failed internally");
}

[[nodiscard]] auto source_failure(const repository::ExactSourceEditError& error)
    -> std::unexpected<ToolExecutionError> {
  using Code = repository::ExactSourceEditErrorCode;
  switch (error.code) {
    case Code::cancelled:
      return execution_error(ToolExecutionErrorCode::cancelled,
                             "repository read was cancelled");
    case Code::timed_out:
      return execution_error(ToolExecutionErrorCode::timed_out,
                             "repository read timed out", true);
    case Code::resource_exhausted:
      return execution_error(ToolExecutionErrorCode::output_limit,
                             "repository file exceeded its read limit");
    case Code::invalid_request:
      return execution_error(ToolExecutionErrorCode::protocol_failure,
                             "repository read request was rejected");
    case Code::not_found:
      return execution_error(ToolExecutionErrorCode::unavailable,
                             "repository file was not found");
    case Code::outside_repository:
    case Code::permission_denied:
    case Code::unsupported_entry:
      return execution_error(ToolExecutionErrorCode::unavailable,
                             "repository file is not readable");
    case Code::stale_snapshot:
    case Code::source_mismatch:
    case Code::concurrent_change:
      return execution_error(ToolExecutionErrorCode::unavailable,
                             "repository changed during the read", true);
    case Code::io_failure:
    case Code::durability_failure:
      return execution_error(ToolExecutionErrorCode::unavailable,
                             "repository file could not be read",
                             error.retryable);
    case Code::internal_failure:
      return execution_error(ToolExecutionErrorCode::internal_failure,
                             "repository read failed internally");
  }
  return execution_error(ToolExecutionErrorCode::internal_failure,
                         "repository read failed internally");
}

[[nodiscard]] auto valid_digest(const domain::ContentDigest& digest,
                                const std::uint64_t maximum_bytes) -> bool {
  if (digest.algorithm.empty() || digest.algorithm.size() > 128 ||
      digest.value.empty() || digest.value.size() > 512 ||
      digest.byte_size > maximum_bytes) {
    return false;
  }
  return std::ranges::all_of(digest.algorithm,
                             [](const unsigned char value) {
                               return (value >= '0' && value <= '9') ||
                                      (value >= 'A' && value <= 'Z') ||
                                      (value >= 'a' && value <= 'z') ||
                                      value == '-' || value == '_' ||
                                      value == '.';
                             }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'A' && value <= 'F') ||
                  (value >= 'a' && value <= 'f');
         });
}

[[nodiscard]] auto valid_result(
    const repository::ExactSourceReadRequest& request,
    const repository::ExactSourceReadResult& result) -> bool {
  return result.source.snapshot ==
             domain::snapshot_identity(request.baseline) &&
         result.source.relative_path == request.relative_path &&
         !result.source.range &&
         result.content.size() <= request.limits.maximum_source_bytes &&
         result.source.content_digest.byte_size == result.content.size() &&
         valid_digest(result.source.content_digest,
                      request.limits.maximum_source_bytes) &&
         (result.content.empty() || detail::is_safe_utf8_text(result.content));
}

class RepositoryReadStream final : public ToolExecutionStream {
 public:
  explicit RepositoryReadStream(domain::StructuredDataBlock result)
      : m_result(std::move(result)) {}

  auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<ToolExecutionEvent>,
                       ToolExecutionError> override {
    if (stop_token.stop_requested()) {
      return execution_error(ToolExecutionErrorCode::cancelled,
                             "repository read was cancelled");
    }
    if (m_emitted) return std::optional<ToolExecutionEvent>{};
    m_emitted = true;
    return std::optional<ToolExecutionEvent>{ToolResult{{m_result}, {}}};
  }

 private:
  domain::StructuredDataBlock m_result;
  bool m_emitted{};
};

class RepositoryReadExecutor final : public ToolExecutor {
 public:
  RepositoryReadExecutor(repository::RepositorySnapshotSource& snapshots,
                         repository::ExactSourceEditor& sources,
                         RepositoryReadToolConfiguration configuration)
      : m_snapshots(snapshots), m_sources(sources),
        m_configuration(std::move(configuration)) {}

  [[nodiscard]] auto validate(
      const domain::StructuredDataBlock& arguments) const
      -> std::expected<ValidatedToolArguments, ToolExecutionError> override {
    auto path = parse_relative_path(arguments, m_configuration);
    if (!path) return std::unexpected(std::move(path.error()));
    return ValidatedToolArguments{
        arguments,
        {requested_read_scope(m_configuration.repository_root, *path)},
        {domain::Effect::read}};
  }

  auto start(ToolInvocation invocation, const std::stop_token stop_token)
      -> std::expected<std::unique_ptr<ToolExecutionStream>,
                       ToolExecutionError> override {
    try {
      auto path =
          parse_relative_path(invocation.arguments.value, m_configuration);
      if (!path) return std::unexpected(std::move(path.error()));
      if (stop_token.stop_requested()) {
        return execution_error(ToolExecutionErrorCode::cancelled,
                               "repository read was cancelled");
      }
      auto snapshot = m_snapshots.observe(
          {m_configuration.repository_root, m_configuration.snapshot_limits},
          stop_token);
      if (!snapshot) return snapshot_failure(snapshot.error());
      if (snapshot->root.canonical_path != m_configuration.repository_root ||
          !repository::validate_repository_snapshot(
              *snapshot, m_configuration.snapshot_limits)) {
        return execution_error(
            ToolExecutionErrorCode::protocol_failure,
            "repository source returned an invalid snapshot");
      }
      if (!snapshot->vcs || snapshot->vcs->system != "git") {
        return execution_error(
            ToolExecutionErrorCode::protocol_failure,
            "repository source did not return a Git snapshot");
      }
      const auto untracked = std::ranges::find(
          snapshot->changes, *path, &domain::RepositoryChange::relative_path);
      if (untracked != snapshot->changes.end() &&
          untracked->stage == domain::RepositoryChangeStage::untracked) {
        return execution_error(ToolExecutionErrorCode::unavailable,
                               "repository path is not a tracked file");
      }
      repository::ExactSourceReadRequest request{
          std::move(*snapshot), std::move(*path), m_configuration.read_limits};
      auto content = m_sources.read(request, stop_token);
      if (!content) return source_failure(content.error());
      if (stop_token.stop_requested()) {
        return execution_error(ToolExecutionErrorCode::cancelled,
                               "repository read was cancelled");
      }
      if (!valid_result(request, *content)) {
        return execution_error(ToolExecutionErrorCode::protocol_failure,
                               "repository source returned an invalid result");
      }
      Json result{
          {"repository_id", content->source.snapshot.repository_id.value()},
          {"snapshot",
           {{"algorithm", content->source.snapshot.fingerprint.algorithm},
            {"value", content->source.snapshot.fingerprint.value},
            {"byte_size", content->source.snapshot.fingerprint.byte_size}}},
          {"relative_path", content->source.relative_path},
          {"content_digest",
           {{"algorithm", content->source.content_digest.algorithm},
            {"value", content->source.content_digest.value},
            {"byte_size", content->source.content_digest.byte_size}}},
          {"content", content->content}};
      auto encoded = result.dump();
      if (encoded.size() > m_configuration.maximum_result_bytes) {
        return execution_error(ToolExecutionErrorCode::output_limit,
                               "repository result exceeded its output limit");
      }
      return std::make_unique<RepositoryReadStream>(
          domain::StructuredDataBlock{"application/json", std::move(encoded)});
    } catch (...) {
      return execution_error(ToolExecutionErrorCode::internal_failure,
                             "repository read failed internally");
    }
  }

 private:
  repository::RepositorySnapshotSource& m_snapshots;
  repository::ExactSourceEditor& m_sources;
  RepositoryReadToolConfiguration m_configuration;
};

} // namespace

auto make_repository_read_approval_rule(
    std::shared_ptr<const DescriptorRelativePathAuthority> root,
    std::string allowed_relative_path,
    AutomaticApprovalRuleConstraints constraints)
    -> std::expected<AutomaticApprovalRule, AutomaticApprovalMatcherError> {
  try {
    constexpr std::string_view digest_prefix{"sha256:"};
    if (!root) {
      return std::unexpected(AutomaticApprovalMatcherError{
          AutomaticApprovalMatcherErrorCode::invalid_configuration,
          "repository-read automatic approval rule is invalid"});
    }
    const std::string root_identity{root->identity()};
    if (root_identity.size() != digest_prefix.size() + 64U ||
        !root_identity.starts_with(digest_prefix) ||
        std::ranges::any_of(root_identity.substr(digest_prefix.size()),
                            [](const unsigned char character) {
                              return !((character >= '0' && character <= '9') ||
                                       (character >= 'a' && character <= 'f'));
                            }) ||
        (!allowed_relative_path.empty() &&
         !valid_relative_path(allowed_relative_path, 4096U))) {
      return std::unexpected(AutomaticApprovalMatcherError{
          AutomaticApprovalMatcherErrorCode::invalid_configuration,
          "repository-read automatic approval rule is invalid"});
    }
    return AutomaticApprovalRule{RepositoryReadPathApprovalRule{
        std::move(root), std::move(allowed_relative_path),
        std::move(constraints)}};
  } catch (...) {
    return std::unexpected(AutomaticApprovalMatcherError{
        AutomaticApprovalMatcherErrorCode::internal_failure,
        "repository-read automatic approval rule failed internally"});
  }
}

auto repository_read_tool_declaration(
    const RepositoryReadToolConfiguration& configuration)
    -> std::expected<backend::ToolDeclaration, ToolRegistryError> {
  try {
    auto validated = valid_configuration(configuration);
    if (!validated) return std::unexpected(std::move(validated.error()));
    Json schema{
        {"type", "object"},
        {"additionalProperties", false},
        {"required", Json::array({"relative_path"})},
        {"properties",
         {{"relative_path",
           {{"type", "string"},
            {"minLength", 1},
            {"maxLength", configuration.read_limits.maximum_path_bytes}}}}}};
    return backend::ToolDeclaration{
        "read_repository_file",
        "Read one bounded UTF-8 Git-tracked regular file from the configured "
        "repository. The path must be relative; Git metadata, untracked or "
        "ignored files, directory traversal, and symbolic links are rejected.",
        {"application/schema+json", schema.dump()},
        {domain::Effect::read},
        {read_scope(configuration.repository_root)}};
  } catch (...) {
    return std::unexpected(
        ToolRegistryError{ToolRegistryErrorCode::internal_failure,
                          "repository-read declaration failed internally"});
  }
}

auto register_repository_read_tool(
    ToolRegistry& registry, repository::RepositorySnapshotSource& snapshots,
    repository::ExactSourceEditor& sources,
    RepositoryReadToolConfiguration configuration)
    -> std::expected<void, ToolRegistryError> {
  try {
    if (!snapshots.guarantees_read_only_observation() ||
        !sources.guarantees_tracked_regular_files() ||
        !sources.guarantees_read_only_execution() ||
        !sources.is_coupled_to(snapshots)) {
      return registry_error(
          "repository-read sources must guarantee coupled read-only tracked "
          "file observation");
    }
    auto declaration = repository_read_tool_declaration(configuration);
    if (!declaration) return std::unexpected(std::move(declaration.error()));
    const auto output_limit = configuration.maximum_result_bytes;
    const auto timeout = configuration.read_limits.timeout;
    return registry.register_tool(
        std::move(*declaration),
        std::make_shared<RepositoryReadExecutor>(snapshots, sources,
                                                 std::move(configuration)),
        ToolExecutionLimits{output_limit, 1, timeout},
        ToolExecutorContract{"aiforge.runtime.read_repository_file", "2"},
        ToolCategory::repository);
  } catch (...) {
    return std::unexpected(
        ToolRegistryError{ToolRegistryErrorCode::internal_failure,
                          "repository-read registration failed internally"});
  }
}

} // namespace aiforge::runtime
