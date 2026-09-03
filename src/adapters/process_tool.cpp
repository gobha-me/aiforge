#include <aiforge/adapters/process_tool.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <aiforge/runtime/tool_policy.hpp>

namespace aiforge::adapters {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_path_bytes{4096};
constexpr std::size_t result_excerpt_bytes{4096};

[[nodiscard]] auto registry_error(const runtime::ToolRegistryErrorCode code,
                                  std::string message)
    -> runtime::ToolRegistryError {
  return {code, std::move(message)};
}

[[nodiscard]] auto execution_error(const runtime::ToolExecutionErrorCode code,
                                   std::string message,
                                   const bool retryable = false)
    -> runtime::ToolExecutionError {
  return {code, std::move(message), retryable};
}

[[nodiscard]] auto has_control(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto valid_environment_name(const std::string_view name) -> bool {
  if (name.empty() || name.size() > 255 || name.front() == '=' ||
      std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
    return false;
  }
  return std::ranges::all_of(name, [](const unsigned char character) {
    return std::isalnum(character) != 0 || character == '_';
  });
}

[[nodiscard]] auto normalized_path(const std::filesystem::path& path)
    -> std::optional<std::string> {
  auto text = path.generic_string();
  if (!path.is_absolute() || text.empty() || text.size() > maximum_path_bytes ||
      has_control(text) || path.lexically_normal().generic_string() != text) {
    return std::nullopt;
  }
  return text;
}

[[nodiscard]] auto path_is_within(const std::string_view parent_text,
                                  const std::string_view child_text) -> bool {
  const std::filesystem::path parent{parent_text};
  const std::filesystem::path child{child_text};
  auto parent_part = parent.begin();
  auto child_part = child.begin();
  for (; parent_part != parent.end() && child_part != child.end();
       ++parent_part, ++child_part) {
    if (*parent_part != *child_part) return false;
  }
  return parent_part == parent.end();
}

template <typename Value>
[[nodiscard]] auto unique(const std::vector<Value>& values) -> bool {
  std::set<Value> found;
  return std::ranges::all_of(
      values, [&](const auto& value) { return found.insert(value).second; });
}

[[nodiscard]] auto add_within(std::size_t& total, const std::size_t value,
                              const std::size_t limit) -> bool {
  if (value > limit || total > limit - value) return false;
  total += value;
  return true;
}

[[nodiscard]] auto valid_limits(const ProcessToolLimits& limits) -> bool {
  constexpr ProcessToolLimits maximums;
  return limits.executables != 0 && limits.arguments != 0 &&
         limits.argument_bytes != 0 && limits.roots != 0 &&
         limits.environment_variables != 0 &&
         limits.timeout > std::chrono::milliseconds::zero() &&
         limits.output_bytes != 0 && limits.inline_output_bytes != 0 &&
         limits.inline_output_bytes <= limits.output_bytes &&
         limits.progress_chunk_bytes != 0 && limits.progress_events != 0 &&
         limits.termination_grace > std::chrono::milliseconds::zero() &&
         limits.executables <= maximums.executables &&
         limits.arguments <= maximums.arguments &&
         limits.argument_bytes <= maximums.argument_bytes &&
         limits.roots <= maximums.roots &&
         limits.environment_variables <= maximums.environment_variables &&
         limits.timeout <= maximums.timeout &&
         limits.output_bytes <= maximums.output_bytes &&
         limits.inline_output_bytes <= maximums.inline_output_bytes &&
         limits.progress_chunk_bytes <= maximums.progress_chunk_bytes &&
         limits.progress_events <= maximums.progress_events &&
         limits.termination_grace <= maximums.termination_grace;
}

struct NormalizedConfiguration {
  ProcessToolConfiguration source;
  std::vector<std::string> executables;
  std::vector<std::string> readable_roots;
  std::vector<std::string> writable_roots;
  std::map<std::string, std::string> environment;
};

[[nodiscard]] auto normalize_configuration(
    const ProcessToolConfiguration& configuration)
    -> std::expected<NormalizedConfiguration, runtime::ToolRegistryError> {
  if (!valid_limits(configuration.limits) ||
      configuration.executable_allowlist.empty() ||
      configuration.readable_roots.empty() ||
      configuration.executable_allowlist.size() >
          configuration.limits.executables ||
      configuration.readable_roots.size() > configuration.limits.roots ||
      configuration.writable_roots.size() > configuration.limits.roots ||
      configuration.environment_allowlist.size() >
          configuration.limits.environment_variables) {
    return std::unexpected(
        registry_error(runtime::ToolRegistryErrorCode::invalid_declaration,
                       "process tool configuration exceeds its bounds"));
  }

  NormalizedConfiguration normalized{configuration, {}, {}, {}, {}};
  const auto append_paths = [](const auto& paths, auto& output) -> bool {
    for (const auto& path : paths) {
      auto normalized = normalized_path(path);
      if (!normalized) return false;
      output.push_back(std::move(*normalized));
    }
    return unique(output);
  };
  if (!append_paths(configuration.executable_allowlist,
                    normalized.executables) ||
      !append_paths(configuration.readable_roots, normalized.readable_roots) ||
      !append_paths(configuration.writable_roots, normalized.writable_roots)) {
    return std::unexpected(registry_error(
        runtime::ToolRegistryErrorCode::invalid_declaration,
        "process tool paths must be unique normalized absolute paths"));
  }
  if (std::ranges::any_of(normalized.writable_roots, [&](const auto& writable) {
        return std::ranges::none_of(normalized.readable_roots,
                                    [&](const auto& readable) {
                                      return path_is_within(readable, writable);
                                    });
      })) {
    return std::unexpected(registry_error(
        runtime::ToolRegistryErrorCode::invalid_declaration,
        "every writable root must be covered by a readable root"));
  }

  std::size_t environment_bytes{};
  for (const auto& variable : configuration.environment_allowlist) {
    if (!valid_environment_name(variable.name) ||
        variable.value.find('\0') != std::string::npos ||
        !add_within(environment_bytes,
                    variable.name.size() + variable.value.size() + 2U,
                    configuration.limits.argument_bytes) ||
        !normalized.environment.emplace(variable.name, variable.value).second) {
      return std::unexpected(
          registry_error(runtime::ToolRegistryErrorCode::invalid_declaration,
                         "process environment allowlist is malformed"));
    }
  }
  return normalized;
}

[[nodiscard]] auto declaration_from(
    const NormalizedConfiguration& configuration) -> backend::ToolDeclaration {
  Json executable_values = Json::array();
  for (const auto& path : configuration.executables) {
    executable_values.push_back(path);
  }
  Json environment_values = Json::array();
  for (const auto& [name, value] : configuration.environment) {
    static_cast<void>(value);
    environment_values.push_back(name);
  }

  const auto path_schema =
      Json{{"type", "string"}, {"maxLength", maximum_path_bytes}};
  const auto root_schema = Json{{"type", "array"},
                                {"maxItems", configuration.source.limits.roots},
                                {"uniqueItems", true},
                                {"items", path_schema}};
  const auto environment_schema =
      Json{{"type", "array"},
           {"maxItems", configuration.source.limits.environment_variables},
           {"uniqueItems", true},
           {"items",
            Json{{"type", "string"}, {"enum", std::move(environment_values)}}}};
  Json properties = {
      {"executable",
       {{"type", "string"}, {"enum", std::move(executable_values)}}},
      {"arguments",
       {{"type", "array"},
        {"maxItems", configuration.source.limits.arguments},
        {"items", {{"type", "string"}}}}},
      {"working_directory", path_schema},
      {"readable_roots", root_schema},
      {"writable_roots", root_schema},
      {"environment", environment_schema},
      {"stdin", {{"const", "closed"}}},
      {"timeout_ms",
       {{"type", "integer"},
        {"minimum", 1},
        {"maximum", configuration.source.limits.timeout.count()}}},
      {"output_bytes",
       {{"type", "integer"},
        {"minimum", 1},
        {"maximum", configuration.source.limits.output_bytes}}}};
  Json schema = {
      {"type", "object"},
      {"additionalProperties", false},
      {"required",
       Json::array({"executable", "arguments", "working_directory",
                    "readable_roots", "writable_roots", "environment", "stdin",
                    "timeout_ms", "output_bytes"})},
      {"properties", std::move(properties)}};

  std::vector<domain::Effect> effects{domain::Effect::execute,
                                      domain::Effect::read};
  if (!configuration.writable_roots.empty()) {
    effects.push_back(domain::Effect::write);
  }
  std::vector<domain::CapabilityScope> scopes;
  scopes.reserve(configuration.executables.size() +
                 configuration.readable_roots.size() +
                 configuration.writable_roots.size());
  for (const auto& executable : configuration.executables) {
    scopes.push_back({domain::Effect::execute, "process.command", executable});
  }
  for (const auto& root : configuration.readable_roots) {
    scopes.push_back({domain::Effect::read, "filesystem.root", root});
  }
  for (const auto& root : configuration.writable_roots) {
    scopes.push_back({domain::Effect::write, "filesystem.root", root});
  }
  return {"run_process",
          "Run one executable with an argument vector under explicit process, "
          "filesystem, environment, time, and output bounds. No shell is used.",
          {"application/schema+json", schema.dump()},
          std::move(effects),
          std::move(scopes)};
}

struct ProcessRequest {
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;
  std::vector<std::string> readable_roots;
  std::vector<std::string> writable_roots;
  std::vector<std::string> environment;
  std::chrono::milliseconds timeout{};
  std::size_t output_bytes{};
};

class DuplicateJsonKey final : public std::exception {
 public:
  [[nodiscard]] auto what() const noexcept -> const char* override {
    return "duplicate process argument key";
  }
};

[[nodiscard]] auto string_array(const Json& value,
                                const std::size_t maximum_items,
                                const std::size_t maximum_total_bytes,
                                const bool reject_controls)
    -> std::optional<std::vector<std::string>> {
  if (!value.is_array() || value.size() > maximum_items) return std::nullopt;
  std::vector<std::string> result;
  std::size_t total{};
  result.reserve(value.size());
  for (const auto& item : value) {
    if (!item.is_string()) return std::nullopt;
    auto text = item.get<std::string>();
    if (text.find('\0') != std::string::npos ||
        (reject_controls && has_control(text)) ||
        text.size() >
            maximum_total_bytes - std::min(total, maximum_total_bytes)) {
      return std::nullopt;
    }
    total += text.size();
    result.push_back(std::move(text));
  }
  return result;
}

[[nodiscard]] auto parse_request(const domain::StructuredDataBlock& arguments,
                                 const NormalizedConfiguration& configuration)
    -> std::expected<ProcessRequest, runtime::ToolExecutionError> {
  try {
    if (arguments.media_type != "application/json" || arguments.data.empty() ||
        arguments.data.size() > configuration.source.limits.argument_bytes) {
      return std::unexpected(execution_error(
          runtime::ToolExecutionErrorCode::invalid_arguments,
          "process arguments must be bounded application/json"));
    }
    std::vector<std::set<std::string>> object_keys;
    const auto callback = [&object_keys](const int,
                                         const Json::parse_event_t event,
                                         Json& parsed) {
      if (event == Json::parse_event_t::object_start) {
        object_keys.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        if (object_keys.empty() ||
            !object_keys.back().insert(parsed.get<std::string>()).second) {
          throw DuplicateJsonKey{};
        }
      } else if (event == Json::parse_event_t::object_end &&
                 !object_keys.empty()) {
        object_keys.pop_back();
      }
      return true;
    };
    auto value = Json::parse(arguments.data, callback, true, false);
    static const std::set<std::string> fields{
        "executable",     "arguments",      "working_directory",
        "readable_roots", "writable_roots", "environment",
        "stdin",          "timeout_ms",     "output_bytes"};
    if (!value.is_object() || value.size() != fields.size() ||
        std::ranges::any_of(
            value.items(),
            [&](const auto& item) { return !fields.contains(item.key()); }) ||
        std::ranges::any_of(
            fields,
            [&](const auto& field) { return !value.contains(field); }) ||
        !value.at("executable").is_string() ||
        !value.at("working_directory").is_string() ||
        !value.at("stdin").is_string() ||
        value.at("stdin").get<std::string>() != "closed" ||
        !value.at("timeout_ms").is_number_unsigned() ||
        !value.at("output_bytes").is_number_unsigned()) {
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                          "process arguments are malformed"));
    }

    ProcessRequest request;
    request.executable = value.at("executable").get<std::string>();
    request.working_directory =
        value.at("working_directory").get<std::string>();
    auto arguments_array = string_array(
        value.at("arguments"), configuration.source.limits.arguments,
        configuration.source.limits.argument_bytes, false);
    auto readable = string_array(
        value.at("readable_roots"), configuration.source.limits.roots,
        maximum_path_bytes * configuration.source.limits.roots, true);
    auto writable = string_array(
        value.at("writable_roots"), configuration.source.limits.roots,
        maximum_path_bytes * configuration.source.limits.roots, true);
    auto environment = string_array(
        value.at("environment"),
        configuration.source.limits.environment_variables,
        255U * configuration.source.limits.environment_variables, true);
    const auto timeout = value.at("timeout_ms").get<std::uint64_t>();
    const auto output = value.at("output_bytes").get<std::uint64_t>();
    if (!arguments_array || !readable || !writable || !environment ||
        timeout == 0 ||
        timeout > static_cast<std::uint64_t>(
                      configuration.source.limits.timeout.count()) ||
        output == 0 || output > configuration.source.limits.output_bytes ||
        output > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                          "process arguments exceed configured limits"));
    }
    request.arguments = std::move(*arguments_array);
    request.readable_roots = std::move(*readable);
    request.writable_roots = std::move(*writable);
    request.environment = std::move(*environment);
    request.timeout = std::chrono::milliseconds{timeout};
    request.output_bytes = static_cast<std::size_t>(output);

    auto executable = normalized_path(request.executable);
    auto working_directory = normalized_path(request.working_directory);
    if (!executable || !working_directory ||
        *executable != request.executable ||
        *working_directory != request.working_directory ||
        !std::ranges::contains(configuration.executables, request.executable) ||
        request.readable_roots.empty() || !unique(request.readable_roots) ||
        !unique(request.writable_roots) || !unique(request.environment)) {
      return std::unexpected(execution_error(
          runtime::ToolExecutionErrorCode::invalid_arguments,
          "process paths and lists must be unique normalized allowed values"));
    }
    for (const auto& root : request.readable_roots) {
      auto normalized = normalized_path(root);
      if (!normalized || *normalized != root ||
          std::ranges::none_of(configuration.readable_roots,
                               [&](const auto& ceiling) {
                                 return path_is_within(ceiling, root);
                               })) {
        return std::unexpected(
            execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                            "readable root exceeds the configured ceiling"));
      }
    }
    for (const auto& root : request.writable_roots) {
      auto normalized = normalized_path(root);
      if (!normalized || *normalized != root ||
          std::ranges::none_of(configuration.writable_roots,
                               [&](const auto& ceiling) {
                                 return path_is_within(ceiling, root);
                               }) ||
          std::ranges::none_of(request.readable_roots,
                               [&](const auto& readable_root) {
                                 return path_is_within(readable_root, root);
                               })) {
        return std::unexpected(execution_error(
            runtime::ToolExecutionErrorCode::invalid_arguments,
            "writable root exceeds the requested or configured ceiling"));
      }
    }
    if (std::ranges::none_of(request.readable_roots,
                             [&](const auto& root) {
                               return path_is_within(root,
                                                     request.working_directory);
                             }) ||
        std::ranges::any_of(request.environment, [&](const auto& name) {
          return !configuration.environment.contains(name);
        })) {
      return std::unexpected(execution_error(
          runtime::ToolExecutionErrorCode::invalid_arguments,
          "working directory or environment exceeds requested authority"));
    }
    std::size_t process_bytes{};
    if (!add_within(process_bytes, request.executable.size() + 1U,
                    configuration.source.limits.argument_bytes) ||
        std::ranges::any_of(request.arguments,
                            [&](const auto& argument) {
                              return !add_within(
                                  process_bytes, argument.size() + 1U,
                                  configuration.source.limits.argument_bytes);
                            }) ||
        std::ranges::any_of(request.environment, [&](const auto& name) {
          const auto& environment_value = configuration.environment.at(name);
          return !add_within(process_bytes,
                             name.size() + environment_value.size() + 2U,
                             configuration.source.limits.argument_bytes);
        })) {
      return std::unexpected(execution_error(
          runtime::ToolExecutionErrorCode::invalid_arguments,
          "process argv and environment exceed the configured byte limit"));
    }
    return request;
  } catch (...) {
    return std::unexpected(
        execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                        "process arguments are malformed"));
  }
}

[[nodiscard]] auto required_effects(const ProcessRequest& request)
    -> std::vector<domain::Effect> {
  std::vector<domain::Effect> result{domain::Effect::execute,
                                     domain::Effect::read};
  if (!request.writable_roots.empty()) result.push_back(domain::Effect::write);
  return result;
}

[[nodiscard]] auto required_scopes(const ProcessRequest& request)
    -> std::vector<domain::CapabilityScope> {
  std::vector<domain::CapabilityScope> result{
      {domain::Effect::execute, "process.command", request.executable}};
  for (const auto& root : request.readable_roots) {
    result.push_back({domain::Effect::read, "filesystem.root", root});
  }
  for (const auto& root : request.writable_roots) {
    result.push_back({domain::Effect::write, "filesystem.root", root});
  }
  return result;
}

[[nodiscard]] auto base64(const std::string_view bytes) -> std::string {
  static constexpr std::string_view alphabet{
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
  std::string result;
  result.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t index = 0; index < bytes.size(); index += 3U) {
    const auto first = static_cast<unsigned char>(bytes[index]);
    const auto second = index + 1U < bytes.size()
                            ? static_cast<unsigned char>(bytes[index + 1U])
                            : 0U;
    const auto third = index + 2U < bytes.size()
                           ? static_cast<unsigned char>(bytes[index + 2U])
                           : 0U;
    const auto packed = (static_cast<std::uint32_t>(first) << 16U) |
                        (static_cast<std::uint32_t>(second) << 8U) |
                        static_cast<std::uint32_t>(third);
    result.push_back(alphabet[(packed >> 18U) & 0x3FU]);
    result.push_back(alphabet[(packed >> 12U) & 0x3FU]);
    result.push_back(
        index + 1U < bytes.size() ? alphabet[(packed >> 6U) & 0x3FU] : '=');
    result.push_back(index + 2U < bytes.size() ? alphabet[packed & 0x3FU]
                                               : '=');
  }
  return result;
}

[[nodiscard]] auto safe_utf8(const std::string_view value) -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first < 0x80U) {
      if ((first < 0x20U && first != '\n' && first != '\r' && first != '\t') ||
          first == 0x7FU) {
        return false;
      }
      ++index;
      continue;
    }
    std::size_t length{};
    std::uint32_t point{};
    if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      point = first & 0x1FU;
      if (point < 2U) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      point = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      point = first & 0x07U;
    } else {
      return false;
    }
    if (index + length > value.size()) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) return false;
      point = (point << 6U) | (continuation & 0x3FU);
    }
    if ((length == 3 && point < 0x800U) || (length == 4 && point < 0x10000U) ||
        point > 0x10FFFFU || (point >= 0xD800U && point <= 0xDFFFU)) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto redact_environment_values(
    std::string output, const std::vector<std::string>& values) -> std::string {
  for (const auto& value : values) {
    if (value.empty()) continue;
    const std::string replacement(value.size(), '*');
    std::size_t position{};
    while ((position = output.find(value, position)) != std::string::npos) {
      output.replace(position, value.size(), replacement);
      position += replacement.size();
    }
  }
  return output;
}

[[nodiscard]] auto artifact_id_for(const domain::InvocationId& invocation_id,
                                   const std::string_view stream)
    -> domain::ArtifactId {
  std::uint64_t hash{1469598103934665603ULL};
  const auto append = [&](const std::string_view value) {
    for (const unsigned char byte : value) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
  };
  append(invocation_id.value());
  append(stream);
  static constexpr char digits[]{"0123456789abcdef"};
  std::string hex(16, '0');
  for (std::size_t index = 0; index < hex.size(); ++index) {
    const auto shift = static_cast<unsigned>((hex.size() - index - 1U) * 4U);
    hex[index] = digits[(hash >> shift) & 0xFU];
  }
  return domain::ArtifactId::from("process-" + std::string{stream} + "-" + hex)
      .value();
}

#ifndef _WIN32

class UniqueFd final {
 public:
  UniqueFd() = default;
  explicit UniqueFd(const int value) : m_value(value) {}
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd&) = delete;
  auto operator=(const UniqueFd&) -> UniqueFd& = delete;
  UniqueFd(UniqueFd&& other) noexcept
      : m_value(std::exchange(other.m_value, -1)) {}
  auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
    if (this != &other) {
      reset();
      m_value = std::exchange(other.m_value, -1);
    }
    return *this;
  }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  [[nodiscard]] auto release() noexcept -> int {
    return std::exchange(m_value, -1);
  }
  auto reset(const int value = -1) noexcept -> void {
    if (m_value >= 0) static_cast<void>(::close(m_value));
    m_value = value;
  }

 private:
  int m_value{-1};
};

struct FileIdentity {
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint32_t mode{};
  auto operator==(const FileIdentity&) const -> bool = default;
};

[[nodiscard]] auto identity(const int descriptor)
    -> std::optional<FileIdentity> {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) return std::nullopt;
  return FileIdentity{static_cast<std::uint64_t>(status.st_dev),
                      static_cast<std::uint64_t>(status.st_ino),
                      static_cast<std::uint32_t>(status.st_mode)};
}

[[nodiscard]] auto open_without_symlinks(const std::string_view path,
                                         const bool directory)
    -> std::expected<UniqueFd, runtime::ToolExecutionError> {
  UniqueFd current{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (current.get() < 0) {
    return std::unexpected(
        execution_error(runtime::ToolExecutionErrorCode::unavailable,
                        "process path resolution is unavailable", true));
  }
  std::size_t position{1};
  while (position < path.size()) {
    const auto slash = path.find('/', position);
    const auto end = slash == std::string_view::npos ? path.size() : slash;
    const auto component = path.substr(position, end - position);
    if (component.empty() || component == "." || component == "..") {
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                          "process path is ambiguous"));
    }
    const bool last = end == path.size();
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                      ((!last || directory) ? O_DIRECTORY : 0);
    const std::string component_text{component};
    UniqueFd next{::openat(current.get(), component_text.c_str(), flags)};
    if (next.get() < 0) {
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::unavailable,
                          "process path could not be opened securely", false));
    }
    current = std::move(next);
    if (last) break;
    position = end + 1U;
  }
  auto status = identity(current.get());
  if (!status || (directory && !S_ISDIR(status->mode)) ||
      (!directory &&
       (!S_ISREG(status->mode) || (status->mode & 0111U) == 0U))) {
    return std::unexpected(execution_error(
        runtime::ToolExecutionErrorCode::unavailable,
        "process target has an invalid filesystem type", false));
  }
  return current;
}

struct PreparedConfiguration {
  NormalizedConfiguration normalized;
  std::map<std::string, FileIdentity> identities;
};

[[nodiscard]] auto prepare_configuration(NormalizedConfiguration configuration)
    -> std::expected<PreparedConfiguration, runtime::ToolRegistryError> {
  PreparedConfiguration prepared{std::move(configuration), {}};
  const auto pin = [&](const std::string& path, const bool directory) -> bool {
    auto opened = open_without_symlinks(path, directory);
    if (!opened) return false;
    auto current_identity = identity(opened->get());
    return current_identity &&
           prepared.identities.emplace(path, *current_identity).second;
  };
  for (const auto& executable : prepared.normalized.executables) {
    if (!pin(executable, false)) {
      return std::unexpected(registry_error(
          runtime::ToolRegistryErrorCode::invalid_declaration,
          "configured executable must be a stable regular executable"));
    }
  }
  for (const auto& root : prepared.normalized.readable_roots) {
    if (!pin(root, true)) {
      return std::unexpected(registry_error(
          runtime::ToolRegistryErrorCode::invalid_declaration,
          "configured readable root must be a stable directory"));
    }
  }
  for (const auto& root : prepared.normalized.writable_roots) {
    if (!prepared.identities.contains(root) && !pin(root, true)) {
      return std::unexpected(registry_error(
          runtime::ToolRegistryErrorCode::invalid_declaration,
          "configured writable root must be a stable directory"));
    }
  }
  return prepared;
}

[[nodiscard]] auto matches_pinned(const PreparedConfiguration& configuration,
                                  const std::string& path, const bool directory)
    -> bool {
  const auto expected = configuration.identities.find(path);
  if (expected == configuration.identities.end()) return false;
  auto opened = open_without_symlinks(path, directory);
  if (!opened) return false;
  auto current = identity(opened->get());
  return current && *current == expected->second;
}

[[nodiscard]] auto descriptor_matches_pinned(
    const PreparedConfiguration& configuration, const std::string& path,
    const int descriptor) -> bool {
  const auto expected = configuration.identities.find(path);
  const auto current = identity(descriptor);
  return expected != configuration.identities.end() && current &&
         *current == expected->second;
}

[[noreturn]] auto child_failure(const int descriptor,
                                const int error_number) noexcept -> void {
  const auto* data = reinterpret_cast<const char*>(&error_number);
  std::size_t written{};
  while (written < sizeof(error_number)) {
    const auto count =
        ::write(descriptor, data + written, sizeof(error_number) - written);
    if (count > 0) {
      written += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  _exit(127);
}

auto close_extra_descriptors(const int descriptor_limit) noexcept -> void {
#if defined(__linux__) && defined(SYS_close_range)
  if (::syscall(SYS_close_range, 5U, ~0U, 0U) == 0) return;
#endif
  for (int descriptor = 5; descriptor < descriptor_limit; ++descriptor) {
    static_cast<void>(::close(descriptor));
  }
}

[[nodiscard]] auto scopes_cover(
    const std::vector<domain::CapabilityScope>& granted,
    const std::vector<domain::CapabilityScope>& requested) -> bool {
  return std::ranges::all_of(requested, [&](const auto& scope) {
    return std::ranges::any_of(granted, [&](const auto& grant) {
      return runtime::capability_scope_covers(grant, scope);
    });
  });
}

struct PendingProgress {
  std::string stream;
  std::string bytes;
};

class ProcessStream final : public runtime::ToolExecutionStream {
 public:
  ProcessStream(pid_t process, UniqueFd standard_output,
                UniqueFd standard_error, UniqueFd exec_error,
                ProcessRequest request, domain::InvocationId invocation_id,
                storage::ArtifactStore& artifact_store,
                ProcessToolLimits limits,
                std::vector<std::string> environment_values)
      : m_process(process), m_standard_output(std::move(standard_output)),
        m_standard_error(std::move(standard_error)),
        m_exec_error(std::move(exec_error)), m_request(std::move(request)),
        m_invocation_id(std::move(invocation_id)),
        m_artifact_store(artifact_store), m_limits(limits),
        m_buffer(limits.progress_chunk_bytes),
        m_environment_values(std::move(environment_values)),
        m_started(std::chrono::steady_clock::now()) {}

  ~ProcessStream() override {
    if (!m_result_emitted) {
      m_discard_output = true;
      m_discard_progress = true;
      terminate_tree();
    }
  }

  auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    try {
      if (m_result_emitted) {
        return std::optional<runtime::ToolExecutionEvent>{};
      }
      for (;;) {
        if (stop_token.stop_requested()) {
          terminate_tree();
          return std::unexpected(
              execution_error(runtime::ToolExecutionErrorCode::cancelled,
                              "process execution cancelled"));
        }
        if (!m_forced_status &&
            std::chrono::steady_clock::now() - m_started >= m_request.timeout) {
          m_forced_status = "timed_out";
          terminate_tree();
        }
        if (m_output_limit_hit && !m_forced_status) {
          m_forced_status = "output_limit";
          terminate_tree();
        }
        if (m_io_failure) {
          terminate_tree();
          return std::unexpected(
              execution_error(runtime::ToolExecutionErrorCode::unavailable,
                              "process output could not be collected", true));
        }
        if (!m_progress.empty()) {
          auto progress = std::move(m_progress.front());
          m_progress.pop_front();
          Json payload{{"stream", progress.stream},
                       {"encoding", "base64"},
                       {"data", base64(progress.bytes)}};
          return std::optional<runtime::ToolExecutionEvent>{
              runtime::ToolProgress{{domain::StructuredDataBlock{
                  "application/vnd.aiforge.process-progress+json",
                  payload.dump()}}}};
        }
        if (finished()) return make_result(stop_token);
        pump(25);
      }
    } catch (...) {
      m_discard_output = true;
      m_discard_progress = true;
      terminate_tree();
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::internal_failure,
                          "process execution failed internally"));
    }
  }

 private:
  [[nodiscard]] auto finished() const noexcept -> bool {
    return m_reaped && m_standard_output.get() < 0 &&
           m_standard_error.get() < 0 && m_exec_error.get() < 0;
  }

  auto queue_progress(const std::string_view stream,
                      const std::string_view bytes) -> void {
    if (m_discard_progress || !m_environment_values.empty() ||
        m_progress_events >= m_limits.progress_events || bytes.empty()) {
      return;
    }
    m_progress.push_back(
        PendingProgress{std::string{stream}, std::string{bytes}});
    ++m_progress_events;
  }

  auto collect(const std::string_view stream, const char* data,
               const std::size_t size, std::string& destination) -> void {
    if (m_discard_output) return;
    const auto remaining = m_observed_output < m_request.output_bytes
                               ? m_request.output_bytes - m_observed_output
                               : 0U;
    const auto accepted = std::min(size, remaining);
    if (accepted != 0) {
      destination.append(data, accepted);
      m_observed_output += accepted;
      queue_progress(stream, std::string_view{data, accepted});
    }
    if (accepted != size) m_output_limit_hit = true;
  }

  auto drain_fd(UniqueFd& descriptor, const std::string_view stream,
                std::string& destination) -> void {
    for (;;) {
      const auto count =
          ::read(descriptor.get(), m_buffer.data(), m_buffer.size());
      if (count > 0) {
        collect(stream, m_buffer.data(), static_cast<std::size_t>(count),
                destination);
        if (m_output_limit_hit) return;
        continue;
      }
      if (count == 0) {
        descriptor.reset();
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      descriptor.reset();
      m_io_failure = true;
      return;
    }
  }

  auto drain_exec_error() -> void {
    int value{};
    for (;;) {
      const auto count = ::read(m_exec_error.get(), &value, sizeof(value));
      if (count == static_cast<ssize_t>(sizeof(value))) {
        m_exec_errno = value;
        continue;
      }
      if (count == 0) {
        m_exec_error.reset();
        return;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      m_exec_error.reset();
      if (count < 0) m_io_failure = true;
      return;
    }
  }

  auto reap(const int options) -> void {
    if (m_reaped) return;
    int status{};
    const auto waited = ::waitpid(m_process, &status, options);
    if (waited == m_process) {
      m_wait_status = status;
      m_reaped = true;
      // Descendants may retain output descriptors. They cannot outlive the
      // invocation even when the process-group leader exits first.
      static_cast<void>(::kill(-m_process, SIGKILL));
    } else if (waited < 0 && errno != EINTR && errno != ECHILD) {
      m_io_failure = true;
    } else if (waited < 0 && errno == ECHILD) {
      m_reaped = true;
    }
  }

  auto pump(const int timeout_ms) -> void {
    std::array<struct pollfd, 3> descriptors{};
    std::array<UniqueFd*, 3> owners{&m_standard_output, &m_standard_error,
                                    &m_exec_error};
    nfds_t count{};
    for (auto* owner : owners) {
      if (owner->get() >= 0) {
        descriptors[count++] = {owner->get(), POLLIN | POLLHUP | POLLERR, 0};
      }
    }
    if (count != 0) {
      int status{};
      do {
        status = ::poll(descriptors.data(), count, timeout_ms);
      } while (status < 0 && errno == EINTR);
      if (status < 0) m_io_failure = true;
    } else if (!m_reaped && timeout_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{timeout_ms});
    }
    if (m_standard_output.get() >= 0) {
      drain_fd(m_standard_output, "stdout", m_stdout);
    }
    if (m_standard_error.get() >= 0) {
      drain_fd(m_standard_error, "stderr", m_stderr);
    }
    if (m_exec_error.get() >= 0) drain_exec_error();
    reap(WNOHANG);
    if (m_reaped) {
      if (!m_reaped_at) m_reaped_at = std::chrono::steady_clock::now();
      if (std::chrono::steady_clock::now() - *m_reaped_at >=
          m_limits.termination_grace) {
        m_standard_output.reset();
        m_standard_error.reset();
        m_exec_error.reset();
      }
    }
  }

  auto terminate_tree() noexcept -> void {
    if (m_process <= 0) return;
    try {
      static_cast<void>(::kill(-m_process, SIGTERM));
      const auto deadline =
          std::chrono::steady_clock::now() + m_limits.termination_grace;
      while (!m_reaped && std::chrono::steady_clock::now() < deadline) {
        pump(10);
      }
      static_cast<void>(::kill(-m_process, SIGKILL));
      while (!m_reaped) {
        reap(0);
        if (!m_reaped && errno == EINTR) continue;
        if (!m_reaped) break;
      }
      for (int attempt = 0; attempt < 4; ++attempt)
        pump(0);
    } catch (...) {
      m_discard_output = true;
      m_discard_progress = true;
      static_cast<void>(::kill(-m_process, SIGKILL));
      static_cast<void>(::kill(m_process, SIGKILL));
      int status{};
      while (::waitpid(m_process, &status, 0) < 0 && errno == EINTR) {
      }
      m_reaped = true;
    }
    m_standard_output.reset();
    m_standard_error.reset();
    m_exec_error.reset();
  }

  [[nodiscard]] auto store_output(const std::string_view stream,
                                  const std::string& output,
                                  const bool force_artifact,
                                  const std::stop_token& stop_token)
      -> std::expected<std::optional<domain::ArtifactMetadata>,
                       runtime::ToolExecutionError> {
    if (output.empty() ||
        (!force_artifact && output.size() <= m_limits.inline_output_bytes &&
         safe_utf8(output))) {
      return std::optional<domain::ArtifactMetadata>{};
    }
    const auto artifact_id = artifact_id_for(m_invocation_id, stream);
    const auto bytes = std::as_bytes(std::span{output.data(), output.size()});
    std::expected<domain::ArtifactMetadata, storage::ArtifactStoreError>
        stored = std::unexpected(storage::ArtifactStoreError{
            storage::ArtifactStoreErrorCode::internal_failure,
            "artifact write failed internally", false});
    try {
      stored = m_artifact_store.put(
          {artifact_id, "application/octet-stream", m_invocation_id}, bytes,
          stop_token);
    } catch (...) {
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::internal_failure,
                          "process artifact storage failed internally"));
    }
    if (!stored) {
      return std::unexpected(execution_error(
          stored.error().code == storage::ArtifactStoreErrorCode::cancelled
              ? runtime::ToolExecutionErrorCode::cancelled
              : runtime::ToolExecutionErrorCode::unavailable,
          stored.error().code == storage::ArtifactStoreErrorCode::cancelled
              ? "process output artifact storage cancelled"
              : "process output artifact could not be stored",
          stored.error().retryable));
    }
    if (stored->artifact_id != artifact_id ||
        stored->media_type != "application/octet-stream" ||
        stored->byte_size != output.size() || stored->digest.empty() ||
        !stored->producing_invocation_id ||
        *stored->producing_invocation_id != m_invocation_id || stored->width ||
        stored->height) {
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::protocol_failure,
                          "artifact store returned invalid process metadata"));
    }
    return std::optional<domain::ArtifactMetadata>{std::move(*stored)};
  }

  [[nodiscard]] auto make_result(const std::stop_token& stop_token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> {
    auto stdout_value =
        redact_environment_values(std::move(m_stdout), m_environment_values);
    auto stderr_value =
        redact_environment_values(std::move(m_stderr), m_environment_values);
    const bool force_artifact = m_forced_status == "output_limit";
    auto stdout_artifact =
        store_output("stdout", stdout_value, force_artifact, stop_token);
    if (!stdout_artifact) return std::unexpected(stdout_artifact.error());
    auto stderr_artifact =
        store_output("stderr", stderr_value, force_artifact, stop_token);
    if (!stderr_artifact) return std::unexpected(stderr_artifact.error());

    std::string status;
    Json exit_code = nullptr;
    Json signal = nullptr;
    Json spawn_error = nullptr;
    if (m_forced_status) {
      status = *m_forced_status;
    } else if (m_exec_errno) {
      status = "spawn_failed";
      if (*m_exec_errno == ENOENT) {
        spawn_error = "not_found";
      } else if (*m_exec_errno == EACCES) {
        spawn_error = "permission_denied";
      } else if (*m_exec_errno == ENOEXEC) {
        spawn_error = "invalid_format";
      } else {
        spawn_error = "operating_system_error";
      }
    } else if (WIFEXITED(m_wait_status)) {
      status = "exited";
      exit_code = WEXITSTATUS(m_wait_status);
    } else if (WIFSIGNALED(m_wait_status)) {
      status = "signaled";
      signal = WTERMSIG(m_wait_status);
    } else {
      status = "spawn_failed";
      spawn_error = "unknown_status";
    }

    const auto stream_json = [&](const std::string& output,
                                 const auto& artifact) {
      Json result{{"bytes", output.size()},
                  {"artifact_id", artifact ? Json(artifact->artifact_id.value())
                                           : Json(nullptr)}};
      if (!artifact && safe_utf8(output)) {
        result["text"] = output;
        result["excerpt_encoding"] = nullptr;
        result["excerpt"] = nullptr;
      } else {
        const auto head_size =
            std::min(output.size(), result_excerpt_bytes / 2U);
        const auto tail_size =
            std::min(output.size() - head_size, result_excerpt_bytes / 2U);
        std::string excerpt = output.substr(0, head_size);
        if (tail_size != 0) {
          excerpt.append(output.substr(output.size() - tail_size));
        }
        result["text"] = nullptr;
        result["excerpt_encoding"] = "base64";
        result["excerpt"] = base64(excerpt);
      }
      return result;
    };
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_started);
    Json result{{"status", status},
                {"exit_code", std::move(exit_code)},
                {"signal", std::move(signal)},
                {"spawn_error", std::move(spawn_error)},
                {"duration_ms", duration.count()},
                {"output_limit", m_request.output_bytes},
                {"stdout", stream_json(stdout_value, *stdout_artifact)},
                {"stderr", stream_json(stderr_value, *stderr_artifact)}};

    runtime::ToolResult tool_result{{domain::StructuredDataBlock{
        "application/vnd.aiforge.process-result+json", result.dump()}}};
    if (*stdout_artifact) {
      tool_result.content.push_back(domain::ArtifactReferenceBlock{
          (*stdout_artifact)->artifact_id, std::string{"stdout"}});
      tool_result.created_artifacts.push_back(std::move(**stdout_artifact));
    }
    if (*stderr_artifact) {
      tool_result.content.push_back(domain::ArtifactReferenceBlock{
          (*stderr_artifact)->artifact_id, std::string{"stderr"}});
      tool_result.created_artifacts.push_back(std::move(**stderr_artifact));
    }
    m_result_emitted = true;
    return std::optional<runtime::ToolExecutionEvent>{std::move(tool_result)};
  }

  pid_t m_process{-1};
  UniqueFd m_standard_output;
  UniqueFd m_standard_error;
  UniqueFd m_exec_error;
  ProcessRequest m_request;
  domain::InvocationId m_invocation_id;
  storage::ArtifactStore& m_artifact_store;
  ProcessToolLimits m_limits;
  std::vector<char> m_buffer;
  std::vector<std::string> m_environment_values;
  std::chrono::steady_clock::time_point m_started;
  std::optional<std::chrono::steady_clock::time_point> m_reaped_at;
  std::string m_stdout;
  std::string m_stderr;
  std::deque<PendingProgress> m_progress;
  std::size_t m_progress_events{};
  std::size_t m_observed_output{};
  int m_wait_status{};
  std::optional<int> m_exec_errno;
  std::optional<std::string> m_forced_status;
  bool m_output_limit_hit{};
  bool m_io_failure{};
  bool m_reaped{};
  bool m_result_emitted{};
  bool m_discard_output{};
  bool m_discard_progress{};
};

[[nodiscard]] auto set_nonblocking(const int descriptor) -> bool {
  const auto flags = ::fcntl(descriptor, F_GETFL);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] auto make_cloexec_pipe(std::array<int, 2>& descriptors) -> bool {
#ifdef __linux__
  return ::pipe2(descriptors.data(), O_CLOEXEC) == 0;
#else
  if (::pipe(descriptors.data()) != 0) return false;
  for (const auto descriptor : descriptors) {
    const auto flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
      static_cast<void>(::close(descriptors[0]));
      static_cast<void>(::close(descriptors[1]));
      descriptors = {-1, -1};
      return false;
    }
  }
  return true;
#endif
}

class ProcessExecutor final : public runtime::ToolExecutor {
 public:
  ProcessExecutor(PreparedConfiguration configuration,
                  storage::ArtifactStore& artifact_store)
      : m_configuration(std::move(configuration)),
        m_artifact_store(artifact_store) {}

  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    try {
      auto parsed = parse_request(arguments, m_configuration.normalized);
      if (!parsed) return std::unexpected(std::move(parsed.error()));
      return runtime::ValidatedToolArguments{
          arguments, required_scopes(*parsed), required_effects(*parsed)};
    } catch (...) {
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::internal_failure,
                          "process validation failed internally"));
    }
  }

  auto start(runtime::ToolInvocation invocation,
             const std::stop_token stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    std::array<int, 2> output_pipe{-1, -1};
    std::array<int, 2> error_pipe{-1, -1};
    std::array<int, 2> exec_pipe{-1, -1};
    pid_t spawned_process{-1};
    try {
      if (stop_token.stop_requested()) {
        return std::unexpected(
            execution_error(runtime::ToolExecutionErrorCode::cancelled,
                            "process start cancelled"));
      }
      auto request =
          parse_request(invocation.arguments.value, m_configuration.normalized);
      if (!request) return std::unexpected(std::move(request.error()));
      const auto scopes = required_scopes(*request);
      if (!scopes_cover(invocation.granted_scopes, scopes)) {
        return std::unexpected(execution_error(
            runtime::ToolExecutionErrorCode::unavailable,
            "process authority or executable changed before spawn"));
      }
      for (const auto& ceiling : m_configuration.normalized.readable_roots) {
        if (!matches_pinned(m_configuration, ceiling, true)) {
          return std::unexpected(
              execution_error(runtime::ToolExecutionErrorCode::unavailable,
                              "configured process root changed before spawn"));
        }
      }
      for (const auto& ceiling : m_configuration.normalized.writable_roots) {
        if (!matches_pinned(m_configuration, ceiling, true)) {
          return std::unexpected(
              execution_error(runtime::ToolExecutionErrorCode::unavailable,
                              "configured process root changed before spawn"));
        }
      }
      for (const auto& root : request->readable_roots) {
        if (!open_without_symlinks(root, true)) {
          return std::unexpected(
              execution_error(runtime::ToolExecutionErrorCode::unavailable,
                              "requested process root is unavailable"));
        }
      }
      for (const auto& root : request->writable_roots) {
        if (!open_without_symlinks(root, true)) {
          return std::unexpected(
              execution_error(runtime::ToolExecutionErrorCode::unavailable,
                              "requested process root is unavailable"));
        }
      }
      auto executable = open_without_symlinks(request->executable, false);
      auto working_directory =
          open_without_symlinks(request->working_directory, true);
      if (!executable || !working_directory ||
          !descriptor_matches_pinned(m_configuration, request->executable,
                                     executable->get())) {
        return std::unexpected(
            execution_error(runtime::ToolExecutionErrorCode::unavailable,
                            "process target is unavailable"));
      }

      std::vector<std::string> argv_storage;
      argv_storage.reserve(request->arguments.size() + 1U);
      argv_storage.push_back(request->executable);
      argv_storage.insert(argv_storage.end(), request->arguments.begin(),
                          request->arguments.end());
      std::vector<char*> argv;
      argv.reserve(argv_storage.size() + 1U);
      for (auto& value : argv_storage)
        argv.push_back(value.data());
      argv.push_back(nullptr);

      std::vector<std::string> environment_storage;
      std::vector<std::string> environment_values;
      environment_storage.reserve(request->environment.size());
      environment_values.reserve(request->environment.size());
      for (const auto& name : request->environment) {
        const auto& value = m_configuration.normalized.environment.at(name);
        std::string entry;
        entry.reserve(name.size() + value.size() + 1U);
        entry.append(name).push_back('=');
        entry.append(value);
        environment_storage.push_back(std::move(entry));
        environment_values.push_back(value);
      }
      std::vector<char*> environment;
      environment.reserve(environment_storage.size() + 1U);
      for (auto& value : environment_storage) {
        environment.push_back(value.data());
      }
      environment.push_back(nullptr);

      if (!make_cloexec_pipe(output_pipe) || !make_cloexec_pipe(error_pipe) ||
          !make_cloexec_pipe(exec_pipe)) {
        for (const auto descriptor :
             {output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1],
              exec_pipe[0], exec_pipe[1]}) {
          if (descriptor >= 0) static_cast<void>(::close(descriptor));
        }
        return std::unexpected(
            execution_error(runtime::ToolExecutionErrorCode::unavailable,
                            "process pipes could not be created", true));
      }
      if (!set_nonblocking(output_pipe[0]) || !set_nonblocking(error_pipe[0]) ||
          !set_nonblocking(exec_pipe[0])) {
        for (const auto descriptor :
             {output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1],
              exec_pipe[0], exec_pipe[1]}) {
          static_cast<void>(::close(descriptor));
        }
        return std::unexpected(
            execution_error(runtime::ToolExecutionErrorCode::unavailable,
                            "process pipes could not be bounded", true));
      }

      const auto maximum_descriptor = ::sysconf(_SC_OPEN_MAX);
      if (maximum_descriptor < 0) {
        for (const auto descriptor :
             {output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1],
              exec_pipe[0], exec_pipe[1]}) {
          static_cast<void>(::close(descriptor));
        }
        return std::unexpected(
            execution_error(runtime::ToolExecutionErrorCode::unavailable,
                            "process descriptor limit is unavailable", true));
      }
      const auto descriptor_limit = static_cast<int>(
          std::min<long>(maximum_descriptor, static_cast<long>(INT_MAX)));

      const auto process = ::fork();
      if (process < 0) {
        for (const auto descriptor :
             {output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1],
              exec_pipe[0], exec_pipe[1]}) {
          static_cast<void>(::close(descriptor));
        }
        return std::unexpected(
            execution_error(runtime::ToolExecutionErrorCode::unavailable,
                            "process could not be spawned", true));
      }
      if (process == 0) {
        const auto report_failure = [&](const int error_number) noexcept {
          child_failure(exec_pipe[1], error_number);
        };
        if (::setpgid(0, 0) != 0 || ::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(error_pipe[1], STDERR_FILENO) < 0) {
          report_failure(errno);
        }
        const auto null_input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null_input < 0 || ::dup2(null_input, STDIN_FILENO) < 0 ||
            ::fchdir(working_directory->get()) != 0) {
          report_failure(errno);
        }
        if (null_input != STDIN_FILENO) static_cast<void>(::close(null_input));

        sigset_t signal_mask;
        if (::sigemptyset(&signal_mask) != 0 ||
            ::sigprocmask(SIG_SETMASK, &signal_mask, nullptr) != 0) {
          report_failure(errno);
        }
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        if (::sigemptyset(&action.sa_mask) != 0) report_failure(errno);
        for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
          if (signal_number == SIGKILL || signal_number == SIGSTOP) continue;
          if (::sigaction(signal_number, &action, nullptr) != 0 &&
              errno != EINVAL) {
            report_failure(errno);
          }
        }

        if (::dup2(executable->get(), 3) < 0 || ::dup2(exec_pipe[1], 4) < 0 ||
            ::fcntl(3, F_SETFD, FD_CLOEXEC) != 0 ||
            ::fcntl(4, F_SETFD, FD_CLOEXEC) != 0) {
          report_failure(errno);
        }
        close_extra_descriptors(descriptor_limit);
        ::fexecve(3, argv.data(), environment.data());
        auto failure = errno;
        if (failure == ENOENT) {
          if (::fcntl(3, F_SETFD, 0) != 0) child_failure(4, errno);
          ::fexecve(3, argv.data(), environment.data());
          failure = errno;
        }
        child_failure(4, failure);
      }

      spawned_process = process;
      static_cast<void>(::setpgid(process, process));
      static_cast<void>(::close(output_pipe[1]));
      output_pipe[1] = -1;
      static_cast<void>(::close(error_pipe[1]));
      error_pipe[1] = -1;
      static_cast<void>(::close(exec_pipe[1]));
      exec_pipe[1] = -1;
      UniqueFd standard_output{std::exchange(output_pipe[0], -1)};
      UniqueFd standard_error{std::exchange(error_pipe[0], -1)};
      UniqueFd exec_error_descriptor{std::exchange(exec_pipe[0], -1)};
      auto stream = std::make_unique<ProcessStream>(
          process, std::move(standard_output), std::move(standard_error),
          std::move(exec_error_descriptor), std::move(*request),
          invocation.invocation_id, m_artifact_store,
          m_configuration.normalized.source.limits,
          std::move(environment_values));
      spawned_process = -1;
      return stream;
    } catch (...) {
      if (spawned_process > 0) {
        static_cast<void>(::kill(-spawned_process, SIGKILL));
        static_cast<void>(::kill(spawned_process, SIGKILL));
        int status{};
        while (::waitpid(spawned_process, &status, 0) < 0 && errno == EINTR) {
        }
      }
      for (const auto descriptor :
           {output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1],
            exec_pipe[0], exec_pipe[1]}) {
        if (descriptor >= 0) static_cast<void>(::close(descriptor));
      }
      return std::unexpected(
          execution_error(runtime::ToolExecutionErrorCode::internal_failure,
                          "process start failed internally"));
    }
  }

 private:
  PreparedConfiguration m_configuration;
  storage::ArtifactStore& m_artifact_store;
};

#endif

} // namespace

auto process_tool_declaration(const ProcessToolConfiguration& configuration)
    -> std::expected<backend::ToolDeclaration, runtime::ToolRegistryError> {
  try {
    auto normalized = normalize_configuration(configuration);
    if (!normalized) return std::unexpected(std::move(normalized.error()));
    return declaration_from(*normalized);
  } catch (...) {
    return std::unexpected(
        registry_error(runtime::ToolRegistryErrorCode::internal_failure,
                       "process tool declaration failed internally"));
  }
}

auto register_process_tool(runtime::ToolRegistry& registry,
                           storage::ArtifactStore& artifact_store,
                           ProcessToolConfiguration configuration)
    -> std::expected<void, runtime::ToolRegistryError> {
  try {
    auto normalized = normalize_configuration(configuration);
    if (!normalized) return std::unexpected(std::move(normalized.error()));
    auto declaration = declaration_from(*normalized);
#ifdef _WIN32
    static_cast<void>(registry);
    static_cast<void>(artifact_store);
    return std::unexpected(registry_error(
        runtime::ToolRegistryErrorCode::invalid_declaration,
        "the bounded process executor requires a POSIX platform"));
#else
    auto prepared = prepare_configuration(std::move(*normalized));
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    const auto& limits = prepared->normalized.source.limits;
    const auto maximum = std::numeric_limits<std::size_t>::max();
    constexpr std::size_t result_budget{std::size_t{64} * 1024U};
    if (limits.progress_events >
        (maximum - result_budget - 2U * limits.inline_output_bytes) /
            (2U * limits.progress_chunk_bytes)) {
      return std::unexpected(
          registry_error(runtime::ToolRegistryErrorCode::invalid_declaration,
                         "process event budget overflows"));
    }
    const auto event_bytes =
        result_budget + 2U * limits.inline_output_bytes +
        limits.progress_events * (2U * limits.progress_chunk_bytes);
    return registry.register_tool(
        std::move(declaration),
        std::make_shared<ProcessExecutor>(std::move(*prepared), artifact_store),
        runtime::ToolExecutionLimits{event_bytes, limits.progress_events,
                                     limits.timeout +
                                         4 * limits.termination_grace +
                                         std::chrono::seconds{1}},
        runtime::ToolExecutorContract{"aiforge.adapters.run_process", "1"});
#endif
  } catch (...) {
    return std::unexpected(
        registry_error(runtime::ToolRegistryErrorCode::internal_failure,
                       "process tool registration failed internally"));
  }
}

} // namespace aiforge::adapters
