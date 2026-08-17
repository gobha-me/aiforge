#include <aiforge/config/file_store.hpp>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aiforge::config {
namespace {

using Json = nlohmann::json;
constexpr std::size_t maximum_document_bytes = 1024U * 1024U;

class UniqueFd final {
 public:
  explicit UniqueFd(const int fd = -1) : m_fd(fd) {}
  UniqueFd(const UniqueFd&) = delete;
  auto operator=(const UniqueFd&) -> UniqueFd& = delete;
  UniqueFd(UniqueFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
  auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
    if (this != &other) {
      reset();
      m_fd = std::exchange(other.m_fd, -1);
    }
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] auto get() const -> int { return m_fd; }
  [[nodiscard]] explicit operator bool() const { return m_fd >= 0; }
  auto reset() -> void {
    if (m_fd >= 0) static_cast<void>(::close(m_fd));
    m_fd = -1;
  }

 private:
  int m_fd{-1};
};

struct ParsedDocument {
  Json root;
  mode_t mode{};
};

class DuplicateJsonKey final : public std::exception {
 public:
  [[nodiscard]] auto what() const noexcept -> const char* override {
    return "duplicate JSON object key";
  }
};

[[nodiscard]] auto file_error(const ConfigFileErrorCode code,
                              const std::filesystem::path& path,
                              std::string message) -> ConfigFileError {
  return {code, path, std::move(message)};
}

[[nodiscard]] auto error_from_errno(const ConfigFileErrorCode fallback,
                                    const std::filesystem::path& path,
                                    const std::string_view action)
    -> ConfigFileError {
  auto code = fallback;
  if (errno == ELOOP) code = ConfigFileErrorCode::path_escape;
  std::string message{action};
  message.append(": ");
  message.append(std::strerror(errno));
  return file_error(code, path, std::move(message));
}

[[nodiscard]] auto split_key(const std::string_view key)
    -> std::vector<std::string> {
  std::vector<std::string> parts;
  std::size_t start{};
  while (start <= key.size()) {
    const auto end = key.find('.', start);
    parts.emplace_back(key.substr(start, end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

[[nodiscard]] auto parse_json(const std::string& text,
                              const std::filesystem::path& path)
    -> std::expected<Json, ConfigFileError> {
  try {
    std::vector<std::unordered_set<std::string>> object_keys;
    const auto callback = [&object_keys](const int, const Json::parse_event_t event,
                                         Json& parsed) {
      if (event == Json::parse_event_t::object_start) {
        object_keys.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        if (object_keys.empty() ||
            !object_keys.back().insert(parsed.get<std::string>()).second) {
          throw DuplicateJsonKey{};
        }
      } else if (event == Json::parse_event_t::object_end) {
        if (!object_keys.empty()) object_keys.pop_back();
      }
      return true;
    };
    auto parsed = Json::parse(text, callback, true, false);
    if (!parsed.is_object()) {
      return std::unexpected(file_error(
          ConfigFileErrorCode::malformed, path,
          "the configuration document root must be an object"));
    }
    return parsed;
  } catch (const DuplicateJsonKey&) {
    return std::unexpected(file_error(ConfigFileErrorCode::duplicate_key, path,
                                      "the configuration document has a duplicate key"));
  } catch (const Json::exception&) {
    return std::unexpected(file_error(
        ConfigFileErrorCode::malformed, path,
        "the configuration document is not strict UTF-8 JSON"));
  }
}

[[nodiscard]] auto check_app_directory(const std::filesystem::path& directory,
                                       const bool create)
    -> std::expected<void, ConfigFileError> {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return std::unexpected(file_error(ConfigFileErrorCode::read_failed,
                                      directory,
                                      "cannot inspect the configuration directory"));
  }
  if (std::filesystem::exists(status)) {
    if (std::filesystem::is_symlink(status)) {
      return std::unexpected(file_error(ConfigFileErrorCode::path_escape,
                                        directory,
                                        "the AIForge configuration directory cannot be a symlink"));
    }
    if (!std::filesystem::is_directory(status)) {
      return std::unexpected(file_error(ConfigFileErrorCode::not_regular,
                                        directory,
                                        "the AIForge configuration path is not a directory"));
    }
    struct stat info {};
    if (::stat(directory.c_str(), &info) != 0) {
      return std::unexpected(error_from_errno(ConfigFileErrorCode::read_failed,
                                              directory,
                                              "cannot inspect directory permissions"));
    }
    if ((info.st_mode & 0077) != 0) {
      return std::unexpected(file_error(
          ConfigFileErrorCode::insecure_permissions, directory,
          "the AIForge configuration directory must have mode 0700"));
    }
    return {};
  }
  if (!create) return {};

  const auto base = directory.parent_path();
  std::filesystem::create_directories(base, error);
  if (error) {
    return std::unexpected(file_error(ConfigFileErrorCode::write_failed, base,
                                      "cannot create the configuration base directory"));
  }
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return std::unexpected(error_from_errno(ConfigFileErrorCode::write_failed,
                                            directory,
                                            "cannot create the AIForge configuration directory"));
  }
  if (::chmod(directory.c_str(), 0700) != 0) {
    return std::unexpected(error_from_errno(ConfigFileErrorCode::write_failed,
                                            directory,
                                            "cannot secure the AIForge configuration directory"));
  }
  return {};
}

[[nodiscard]] auto read_document(const std::filesystem::path& path)
    -> std::expected<std::optional<ParsedDocument>, ConfigFileError> {
  if (auto checked = check_app_directory(path.parent_path(), false); !checked) {
    return std::unexpected(std::move(checked.error()));
  }
  UniqueFd descriptor{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (!descriptor) {
    if (errno == ENOENT) return std::optional<ParsedDocument>{};
    return std::unexpected(error_from_errno(ConfigFileErrorCode::read_failed,
                                            path,
                                            "cannot open the configuration file"));
  }

  struct stat info {};
  if (::fstat(descriptor.get(), &info) != 0) {
    return std::unexpected(error_from_errno(ConfigFileErrorCode::read_failed,
                                            path,
                                            "cannot inspect the configuration file"));
  }
  if (!S_ISREG(info.st_mode)) {
    return std::unexpected(file_error(ConfigFileErrorCode::not_regular, path,
                                      "the configuration file is not regular"));
  }
  if (info.st_size < 0 ||
      static_cast<std::uintmax_t>(info.st_size) > maximum_document_bytes) {
    return std::unexpected(file_error(ConfigFileErrorCode::too_large, path,
                                      "the configuration file exceeds 1 MiB"));
  }

  std::string contents;
  contents.reserve(static_cast<std::size_t>(info.st_size));
  char buffer[8192];
  while (true) {
    const auto count = ::read(descriptor.get(), buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(error_from_errno(ConfigFileErrorCode::read_failed,
                                              path,
                                              "cannot read the configuration file"));
    }
    if (count == 0) break;
    if (contents.size() + static_cast<std::size_t>(count) >
        maximum_document_bytes) {
      return std::unexpected(file_error(ConfigFileErrorCode::too_large, path,
                                        "the configuration file exceeds 1 MiB"));
    }
    contents.append(buffer, static_cast<std::size_t>(count));
  }
  auto parsed = parse_json(contents, path);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  return std::optional<ParsedDocument>{
      ParsedDocument{std::move(*parsed), info.st_mode}};
}

[[nodiscard]] auto json_value(const Json& value, const ConfigKeySpec& spec)
    -> std::expected<ConfigValue, ConfigDiagnostic> {
  const auto invalid = [&]() {
    return std::unexpected(ConfigDiagnostic{
        ConfigDiagnosticCode::invalid_value, ConfigSource::file, spec.id,
        "the JSON value type does not match the configuration key"});
  };
  try {
    switch (spec.value_kind) {
      case ConfigValueKind::boolean:
        return value.is_boolean() ? std::expected<ConfigValue, ConfigDiagnostic>{
                                        ConfigValue{value.get<bool>()}}
                                  : invalid();
      case ConfigValueKind::signed_integer:
        if (value.is_number_integer()) {
          return ConfigValue{value.get<std::int64_t>()};
        }
        if (value.is_number_unsigned() &&
            value.get<std::uint64_t>() <=
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return ConfigValue{static_cast<std::int64_t>(value.get<std::uint64_t>())};
        }
        return invalid();
      case ConfigValueKind::unsigned_integer:
        if (value.is_number_unsigned()) {
          return ConfigValue{value.get<std::uint64_t>()};
        }
        if (value.is_number_integer()) {
          const auto integer = value.get<std::int64_t>();
          if (integer >= 0) return ConfigValue{static_cast<std::uint64_t>(integer)};
        }
        return invalid();
      case ConfigValueKind::text:
        return value.is_string()
                   ? std::expected<ConfigValue, ConfigDiagnostic>{
                         ConfigValue{value.get<std::string>()}}
                   : invalid();
      case ConfigValueKind::text_list: {
        if (!value.is_array()) return invalid();
        std::vector<std::string> result;
        result.reserve(value.size());
        for (const auto& item : value) {
          if (!item.is_string()) return invalid();
          result.push_back(item.get<std::string>());
        }
        return ConfigValue{std::move(result)};
      }
    }
  } catch (const Json::exception&) {
    return invalid();
  }
  return invalid();
}

[[nodiscard]] auto find_json_value(const Json& root, const std::string_view key)
    -> const Json* {
  const Json* current = &root;
  for (const auto& part : split_key(key)) {
    if (!current->is_object()) return nullptr;
    const auto found = current->find(part);
    if (found == current->end()) return nullptr;
    current = &*found;
  }
  return current;
}

auto collect_leaf_keys(const Json& value, const std::string& prefix,
                       std::vector<std::string>& output) -> void {
  if (value.is_object() && !value.empty()) {
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
      const auto key = prefix.empty() ? iterator.key()
                                      : prefix + "." + iterator.key();
      collect_leaf_keys(iterator.value(), key, output);
    }
    return;
  }
  if (!prefix.empty()) output.push_back(prefix);
}

[[nodiscard]] auto config_value_json(const ConfigValue& value) -> Json {
  return std::visit([](const auto& concrete) { return Json(concrete); }, value);
}

[[nodiscard]] auto apply_set(Json& root, const std::string_view key,
                             const ConfigValue& value,
                             const std::filesystem::path& path)
    -> std::expected<void, ConfigFileError> {
  auto parts = split_key(key);
  Json* current = &root;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    auto& child = (*current)[parts[index]];
    if (child.is_null()) child = Json::object();
    if (!child.is_object()) {
      return std::unexpected(file_error(
          ConfigFileErrorCode::malformed, path,
          "a dotted configuration key conflicts with an existing scalar"));
    }
    current = &child;
  }
  (*current)[parts.back()] = config_value_json(value);
  return {};
}

auto apply_unset(Json& root, const std::string_view key) -> void {
  auto parts = split_key(key);
  Json* current = &root;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    const auto found = current->find(parts[index]);
    if (found == current->end() || !found->is_object()) return;
    current = &*found;
  }
  current->erase(parts.back());
}

[[nodiscard]] auto write_all(const int descriptor, const std::string& contents,
                             const std::filesystem::path& path)
    -> std::expected<void, ConfigFileError> {
  std::size_t written{};
  while (written < contents.size()) {
    const auto count = ::write(descriptor, contents.data() + written,
                               contents.size() - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(error_from_errno(ConfigFileErrorCode::write_failed,
                                              path,
                                              "cannot write the temporary configuration file"));
    }
    written += static_cast<std::size_t>(count);
  }
  return {};
}

[[nodiscard]] auto write_document(const std::filesystem::path& path,
                                  const Json& root)
    -> std::expected<void, ConfigFileError> {
  static std::atomic_uint64_t sequence{};
  auto temporary = path;
  temporary += ".tmp." + std::to_string(::getpid()) + "." +
               std::to_string(sequence.fetch_add(1));

  UniqueFd descriptor{::open(temporary.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                             0600)};
  if (!descriptor) {
    return std::unexpected(error_from_errno(ConfigFileErrorCode::write_failed,
                                            temporary,
                                            "cannot create the temporary configuration file"));
  }
  const auto cleanup = [&]() { static_cast<void>(::unlink(temporary.c_str())); };
  const auto contents = root.dump(2) + "\n";
  if (auto written = write_all(descriptor.get(), contents, temporary); !written) {
    cleanup();
    return std::unexpected(std::move(written.error()));
  }
  if (::fsync(descriptor.get()) != 0) {
    auto error = error_from_errno(ConfigFileErrorCode::sync_failed, temporary,
                                  "cannot sync the temporary configuration file");
    cleanup();
    return std::unexpected(std::move(error));
  }
  descriptor.reset();
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    auto error = error_from_errno(ConfigFileErrorCode::rename_failed, path,
                                  "cannot replace the configuration file");
    cleanup();
    return std::unexpected(std::move(error));
  }
  UniqueFd directory{::open(path.parent_path().c_str(),
                            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  if (!directory || ::fsync(directory.get()) != 0) {
    return std::unexpected(error_from_errno(ConfigFileErrorCode::sync_failed,
                                            path.parent_path(),
                                            "cannot sync the configuration directory"));
  }
  return {};
}

[[nodiscard]] auto find_spec(const ConfigRegistry& registry,
                             const std::string_view key)
    -> const ConfigKeySpec* {
  const auto found = std::ranges::find(registry.keys, key, &ConfigKeySpec::id);
  return found == registry.keys.end() ? nullptr : &*found;
}

template <typename Mutation>
[[nodiscard]] auto mutate_file(const std::filesystem::path& path,
                               const ConfigRegistry& registry,
                               const std::string_view key, Mutation&& mutation)
    -> std::expected<void, ConfigFileError> {
  if (auto registry_valid = validate_registry(registry); !registry_valid) {
    return std::unexpected(file_error(ConfigFileErrorCode::malformed, path,
                                      "the configuration registry is invalid"));
  }
  const auto* spec = find_spec(registry, key);
  if (spec == nullptr || !spec->file_writable || spec->sensitive) {
    return std::unexpected(file_error(ConfigFileErrorCode::malformed, path,
                                      "the configuration key is not file-writable"));
  }
  if (auto directory = check_app_directory(path.parent_path(), true); !directory) {
    return std::unexpected(std::move(directory.error()));
  }

  auto lock_path = path.parent_path() / "config.lock";
  UniqueFd lock{::open(lock_path.c_str(),
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (!lock) {
    return std::unexpected(error_from_errno(ConfigFileErrorCode::lock_failed,
                                            lock_path,
                                            "cannot open the configuration lock"));
  }
  if (::fchmod(lock.get(), 0600) != 0 || ::flock(lock.get(), LOCK_EX) != 0) {
    return std::unexpected(error_from_errno(ConfigFileErrorCode::lock_failed,
                                            lock_path,
                                            "cannot acquire the configuration lock"));
  }

  auto document = read_document(path);
  if (!document) return std::unexpected(std::move(document.error()));
  Json root = document->has_value() ? std::move(document->value().root)
                                    : Json::object();
  if (document->has_value() && (document->value().mode & 0077) != 0) {
    return std::unexpected(file_error(
        ConfigFileErrorCode::insecure_permissions, path,
        "refusing to replace a configuration file with permissions broader than 0600"));
  }
  if (auto changed = mutation(root, *spec); !changed) {
    return std::unexpected(std::move(changed.error()));
  }
  return write_document(path, root);
}

}  // namespace

auto resolve_config_path(const ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path, ConfigFileError> {
  try {
    std::filesystem::path base;
    if (environment.xdg_config_home && environment.xdg_config_home->is_absolute()) {
      base = *environment.xdg_config_home;
    } else {
      if (!environment.home) {
        return std::unexpected(file_error(ConfigFileErrorCode::missing_home, {},
                                          "HOME is required when XDG_CONFIG_HOME "
                                          "is unset or relative"));
      }
      if (!environment.home->is_absolute()) {
        return std::unexpected(file_error(ConfigFileErrorCode::invalid_base_path,
                                          *environment.home,
                                          "the configuration home must be absolute"));
      }
      base = *environment.home / ".config";
    }
    return (base / "aiforge" / "config.json").lexically_normal();
  } catch (...) {
    return std::unexpected(file_error(ConfigFileErrorCode::invalid_base_path, {},
                                      "cannot resolve the configuration path"));
  }
}

JsonConfigFileStore::JsonConfigFileStore(std::filesystem::path path)
    : m_path(std::move(path)) {}

auto JsonConfigFileStore::path() const -> const std::filesystem::path& {
  return m_path;
}

auto JsonConfigFileStore::load(const ConfigRegistry& registry) const
    -> std::expected<ConfigLayer, ConfigFileError> {
  try {
    if (auto registry_valid = validate_registry(registry); !registry_valid) {
      return std::unexpected(file_error(ConfigFileErrorCode::malformed, m_path,
                                        "the configuration registry is invalid"));
    }
    auto document = read_document(m_path);
    if (!document) return std::unexpected(std::move(document.error()));
    ConfigLayer layer{ConfigSource::file, {}, {}};
    if (!document->has_value()) return layer;
    if ((document->value().mode & 0077) != 0) {
      layer.diagnostics.push_back(
          {ConfigDiagnosticCode::source_warning, ConfigSource::file, {},
           "configuration file permissions are broader than 0600"});
    }

    for (const auto& spec : registry.keys) {
      const auto* value = find_json_value(document->value().root, spec.id);
      if (value == nullptr) continue;
      auto parsed = json_value(*value, spec);
      if (parsed) {
        layer.candidates.push_back({spec.id, std::move(*parsed), std::nullopt});
      } else {
        layer.candidates.push_back(
            {spec.id, std::nullopt, std::move(parsed.error())});
      }
    }

    std::unordered_set<std::string> known;
    for (const auto& spec : registry.keys) known.insert(spec.id);
    std::vector<std::string> leaves;
    collect_leaf_keys(document->value().root, {}, leaves);
    for (auto& leaf : leaves) {
      if (!known.contains(leaf)) {
        layer.diagnostics.push_back(
            {ConfigDiagnosticCode::unknown_key, ConfigSource::file,
             std::move(leaf),
             "the configuration file contains an unknown key"});
      }
    }
    return layer;
  } catch (const std::exception&) {
    return std::unexpected(file_error(ConfigFileErrorCode::read_failed, m_path,
                                      "the configuration file adapter failed safely"));
  } catch (...) {
    return std::unexpected(file_error(ConfigFileErrorCode::read_failed, m_path,
                                      "the configuration file adapter failed safely"));
  }
}

auto JsonConfigFileStore::set(const ConfigRegistry& registry,
                              const std::string_view key,
                              const ConfigValue& value)
    -> std::expected<void, ConfigFileError> {
  try {
    return mutate_file(
        m_path, registry, key,
        [&](Json& root, const ConfigKeySpec& spec)
            -> std::expected<void, ConfigFileError> {
          std::vector<std::string_view> raw_list;
          std::string rendered;
          if (spec.value_kind == ConfigValueKind::text_list) {
            const auto* list = std::get_if<std::vector<std::string>>(&value);
            if (list == nullptr) {
              return std::unexpected(file_error(
                  ConfigFileErrorCode::malformed, m_path,
                  "the value is invalid for the configuration key"));
            }
            raw_list.reserve(list->size());
            for (const auto& item : *list) raw_list.push_back(item);
          } else {
            // Re-run public type and size validation without exposing JSON.
            rendered = format_config_value(value);
            raw_list.push_back(rendered);
          }
          const auto validated =
              parse_config_value(spec, raw_list, ConfigSource::file);
          if (!validated || *validated != value) {
            return std::unexpected(file_error(ConfigFileErrorCode::malformed,
                                              m_path,
                                              "the value is invalid for the configuration key"));
          }
          return apply_set(root, key, value, m_path);
        });
  } catch (...) {
    return std::unexpected(file_error(ConfigFileErrorCode::write_failed, m_path,
                                      "the configuration file adapter failed safely"));
  }
}

auto JsonConfigFileStore::unset(const ConfigRegistry& registry,
                                const std::string_view key)
    -> std::expected<void, ConfigFileError> {
  try {
    return mutate_file(
        m_path, registry, key,
        [&](Json& root, const ConfigKeySpec&)
            -> std::expected<void, ConfigFileError> {
          apply_unset(root, key);
          return {};
        });
  } catch (...) {
    return std::unexpected(file_error(ConfigFileErrorCode::write_failed, m_path,
                                      "the configuration file adapter failed safely"));
  }
}

auto process_config_path()
    -> std::expected<std::filesystem::path, ConfigFileError> {
  try {
    ConfigPathEnvironment environment;
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME")) {
      environment.xdg_config_home = std::filesystem::path{xdg};
    }
    if (const auto* home = std::getenv("HOME")) {
      environment.home = std::filesystem::path{home};
    }
    return resolve_config_path(environment);
  } catch (...) {
    return std::unexpected(file_error(ConfigFileErrorCode::invalid_base_path, {},
                                      "cannot read the configuration environment"));
  }
}

}  // namespace aiforge::config
