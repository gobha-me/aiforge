#include <aiforge/credentials/credential.hpp>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace aiforge::credentials {
namespace {

class UniqueFd final {
 public:
  explicit UniqueFd(const int value = -1) : m_value(value) {}
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
  ~UniqueFd() { reset(); }

  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  [[nodiscard]] explicit operator bool() const noexcept { return m_value >= 0; }
  auto reset() noexcept -> void {
    if (m_value >= 0) static_cast<void>(::close(m_value));
    m_value = -1;
  }

 private:
  int m_value{-1};
};

[[nodiscard]] auto failure(const CredentialErrorCode code,
                           const std::filesystem::path& path,
                           std::string message) -> CredentialError {
  return {code, path, std::move(message)};
}

[[nodiscard]] auto errno_failure(const CredentialErrorCode fallback,
                                 const std::filesystem::path& path,
                                 const std::string_view action)
    -> CredentialError {
  auto code = fallback;
  if (errno == ELOOP) code = CredentialErrorCode::path_escape;
  std::string message{action};
  message.append(": ");
  message.append(std::strerror(errno));
  return failure(code, path, std::move(message));
}

[[nodiscard]] auto validate_directory(const std::filesystem::path& directory,
                                      const bool create)
    -> std::expected<void, CredentialError> {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return std::unexpected(failure(CredentialErrorCode::read_failed, directory,
                                   "cannot inspect the credential directory"));
  }
  if (std::filesystem::exists(status)) {
    if (std::filesystem::is_symlink(status)) {
      return std::unexpected(failure(
          CredentialErrorCode::path_escape, directory,
          "the AIForge credential directory cannot be a symlink"));
    }
    if (!std::filesystem::is_directory(status)) {
      return std::unexpected(failure(
          CredentialErrorCode::not_regular, directory,
          "the AIForge credential path is not a directory"));
    }
    struct stat info {};
    if (::stat(directory.c_str(), &info) != 0) {
      return std::unexpected(errno_failure(CredentialErrorCode::read_failed,
                                           directory,
                                           "cannot inspect credential directory permissions"));
    }
    if ((info.st_mode & 0077) != 0) {
      return std::unexpected(failure(
          CredentialErrorCode::insecure_permissions, directory,
          "the AIForge credential directory must have mode 0700"));
    }
    return {};
  }
  if (!create) return {};

  const auto base = directory.parent_path();
  std::filesystem::create_directories(base, error);
  if (error) {
    return std::unexpected(failure(
        CredentialErrorCode::write_failed, base,
        "cannot create the credential base directory"));
  }
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return std::unexpected(errno_failure(CredentialErrorCode::write_failed,
                                         directory,
                                         "cannot create the AIForge credential directory"));
  }
  if (::chmod(directory.c_str(), 0700) != 0) {
    return std::unexpected(errno_failure(CredentialErrorCode::write_failed,
                                         directory,
                                         "cannot secure the AIForge credential directory"));
  }
  return validate_directory(directory, false);
}

[[nodiscard]] auto inspect_target(const std::filesystem::path& path)
    -> std::expected<bool, CredentialError> {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) return false;
    return std::unexpected(errno_failure(CredentialErrorCode::read_failed, path,
                                         "cannot inspect the credential file"));
  }
  if (S_ISLNK(info.st_mode)) {
    return std::unexpected(failure(CredentialErrorCode::path_escape, path,
                                   "the credential file cannot be a symlink"));
  }
  if (!S_ISREG(info.st_mode)) {
    return std::unexpected(failure(CredentialErrorCode::not_regular, path,
                                   "the credential file is not regular"));
  }
  if ((info.st_mode & 0077) != 0) {
    return std::unexpected(failure(
        CredentialErrorCode::insecure_permissions, path,
        "the credential file must have mode 0600"));
  }
  return true;
}

[[nodiscard]] auto write_all(const int descriptor, const std::string_view value,
                             const std::filesystem::path& path)
    -> std::expected<void, CredentialError> {
  std::size_t written{};
  while (written < value.size()) {
    const auto count =
        ::write(descriptor, value.data() + written, value.size() - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(errno_failure(CredentialErrorCode::write_failed,
                                           path,
                                           "cannot write the temporary credential file"));
    }
    if (count == 0) {
      return std::unexpected(failure(CredentialErrorCode::write_failed, path,
                                     "cannot complete the credential write"));
    }
    written += static_cast<std::size_t>(count);
  }
  return {};
}

auto clear_string(std::string& value) noexcept -> void {
  volatile char* bytes = value.empty() ? nullptr : value.data();
  for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = 0;
  value.clear();
}

}  // namespace

Secret::Secret(std::string value) : m_value(std::move(value)) {}

Secret::Secret(Secret&& other) noexcept : m_value(std::move(other.m_value)) {
  other.clear();
}

auto Secret::operator=(Secret&& other) noexcept -> Secret& {
  if (this != &other) {
    clear();
    m_value = std::move(other.m_value);
    other.clear();
  }
  return *this;
}

Secret::~Secret() { clear(); }

auto Secret::view() const noexcept -> std::string_view { return m_value; }

auto Secret::release() && -> std::string {
  auto released = std::move(m_value);
  clear();
  return released;
}

auto Secret::clear() noexcept -> void { clear_string(m_value); }

auto make_secret(std::string value) -> std::expected<Secret, CredentialError> {
  const auto invalid = value.empty() ||
                       value.size() > maximum_credential_bytes ||
                       !std::ranges::all_of(value, [](const unsigned char byte) {
                         return byte >= 0x21U && byte <= 0x7eU;
                       });
  if (invalid) {
    clear_string(value);
    return std::unexpected(failure(
        CredentialErrorCode::invalid_value, {},
        "the Venice credential must be a nonempty printable token no larger than 64 KiB"));
  }
  return Secret{std::move(value)};
}

auto resolve_credential_path(const CredentialPathEnvironment& environment)
    -> std::expected<std::filesystem::path, CredentialError> {
  try {
    std::filesystem::path base;
    if (environment.xdg_config_home &&
        environment.xdg_config_home->is_absolute()) {
      base = *environment.xdg_config_home;
    } else {
      if (!environment.home) {
        return std::unexpected(failure(
            CredentialErrorCode::missing_home, {},
            "HOME is required when XDG_CONFIG_HOME is unset or relative"));
      }
      if (!environment.home->is_absolute()) {
        return std::unexpected(failure(
            CredentialErrorCode::invalid_base_path, *environment.home,
            "the credential home must be absolute"));
      }
      base = *environment.home / ".config";
    }
    return (base / "aiforge" / "credentials").lexically_normal();
  } catch (...) {
    return std::unexpected(failure(CredentialErrorCode::invalid_base_path, {},
                                   "cannot resolve the credential path"));
  }
}

auto process_credential_path()
    -> std::expected<std::filesystem::path, CredentialError> {
  try {
    CredentialPathEnvironment environment;
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME")) {
      environment.xdg_config_home = std::filesystem::path{xdg};
    }
    if (const auto* home = std::getenv("HOME")) {
      environment.home = std::filesystem::path{home};
    }
    return resolve_credential_path(environment);
  } catch (...) {
    return std::unexpected(failure(CredentialErrorCode::invalid_base_path, {},
                                   "cannot read the credential environment"));
  }
}

FileCredentialStore::FileCredentialStore(std::filesystem::path path)
    : m_path(std::move(path)) {}

auto FileCredentialStore::path() const noexcept
    -> const std::filesystem::path& {
  return m_path;
}

auto FileCredentialStore::load()
    -> std::expected<std::optional<Secret>, CredentialError> {
  try {
    if (auto directory = validate_directory(m_path.parent_path(), false);
        !directory) {
      return std::unexpected(std::move(directory.error()));
    }
    auto inspected = inspect_target(m_path);
    if (!inspected) return std::unexpected(std::move(inspected.error()));
    if (!*inspected) return std::optional<Secret>{};

    UniqueFd descriptor{
        ::open(m_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!descriptor) {
      if (errno == ENOENT) return std::optional<Secret>{};
      return std::unexpected(errno_failure(CredentialErrorCode::read_failed,
                                           m_path,
                                           "cannot open the credential file"));
    }
    struct stat info {};
    if (::fstat(descriptor.get(), &info) != 0) {
      return std::unexpected(errno_failure(CredentialErrorCode::read_failed,
                                           m_path,
                                           "cannot inspect the credential file"));
    }
    if (!S_ISREG(info.st_mode)) {
      return std::unexpected(failure(CredentialErrorCode::not_regular, m_path,
                                     "the credential file is not regular"));
    }
    if ((info.st_mode & 0077) != 0) {
      return std::unexpected(failure(CredentialErrorCode::insecure_permissions,
                                     m_path,
                                     "the credential file must have mode 0600"));
    }
    if (info.st_size < 0 ||
        static_cast<std::uintmax_t>(info.st_size) >
            maximum_credential_bytes + 1U) {
      return std::unexpected(failure(CredentialErrorCode::too_large, m_path,
                                     "the credential file exceeds 64 KiB"));
    }

    std::string contents;
    contents.reserve(static_cast<std::size_t>(info.st_size));
    char buffer[4096];
    while (true) {
      const auto count = ::read(descriptor.get(), buffer, sizeof(buffer));
      if (count < 0) {
        if (errno == EINTR) continue;
        clear_string(contents);
        return std::unexpected(errno_failure(CredentialErrorCode::read_failed,
                                             m_path,
                                             "cannot read the credential file"));
      }
      if (count == 0) break;
      if (contents.size() + static_cast<std::size_t>(count) >
          maximum_credential_bytes + 1U) {
        clear_string(contents);
        return std::unexpected(failure(CredentialErrorCode::too_large, m_path,
                                       "the credential file exceeds 64 KiB"));
      }
      contents.append(buffer, static_cast<std::size_t>(count));
    }
    if (contents.ends_with('\n')) contents.pop_back();
    auto secret = make_secret(std::move(contents));
    if (!secret) {
      auto error = std::move(secret.error());
      error.path = m_path;
      return std::unexpected(std::move(error));
    }
    return std::optional<Secret>{std::move(*secret)};
  } catch (...) {
    return std::unexpected(failure(CredentialErrorCode::read_failed, m_path,
                                   "the credential adapter failed safely"));
  }
}

auto FileCredentialStore::store(const Secret& credential)
    -> std::expected<void, CredentialError> {
  try {
    if (credential.view().empty() ||
        credential.view().size() > maximum_credential_bytes) {
      return std::unexpected(failure(CredentialErrorCode::invalid_value, {},
                                     "the Venice credential is invalid"));
    }
    if (auto directory = validate_directory(m_path.parent_path(), true);
        !directory) {
      return std::unexpected(std::move(directory.error()));
    }

    const auto lock_path = m_path.parent_path() / "credentials.lock";
    UniqueFd lock{::open(lock_path.c_str(),
                         O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!lock) {
      return std::unexpected(errno_failure(CredentialErrorCode::lock_failed,
                                           lock_path,
                                           "cannot open the credential lock"));
    }
    if (::fchmod(lock.get(), 0600) != 0 || ::flock(lock.get(), LOCK_EX) != 0) {
      return std::unexpected(errno_failure(CredentialErrorCode::lock_failed,
                                           lock_path,
                                           "cannot acquire the credential lock"));
    }
    auto target = inspect_target(m_path);
    if (!target) return std::unexpected(std::move(target.error()));

    static std::atomic_uint64_t sequence{};
    auto temporary = m_path;
    temporary += ".tmp." + std::to_string(::getpid()) + "." +
                 std::to_string(sequence.fetch_add(1));
    UniqueFd descriptor{::open(temporary.c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                   O_NOFOLLOW,
                               0600)};
    if (!descriptor) {
      return std::unexpected(errno_failure(CredentialErrorCode::write_failed,
                                           temporary,
                                           "cannot create the temporary credential file"));
    }
    const auto cleanup = [&]() { static_cast<void>(::unlink(temporary.c_str())); };
    std::string contents{credential.view()};
    contents.push_back('\n');
    auto written = write_all(descriptor.get(), contents, temporary);
    clear_string(contents);
    if (!written) {
      cleanup();
      return std::unexpected(std::move(written.error()));
    }
    if (::fsync(descriptor.get()) != 0) {
      auto error = errno_failure(CredentialErrorCode::sync_failed, temporary,
                                 "cannot sync the temporary credential file");
      cleanup();
      return std::unexpected(std::move(error));
    }
    descriptor.reset();
    if (::rename(temporary.c_str(), m_path.c_str()) != 0) {
      auto error = errno_failure(CredentialErrorCode::rename_failed, m_path,
                                 "cannot replace the credential file");
      cleanup();
      return std::unexpected(std::move(error));
    }
    UniqueFd directory{::open(m_path.parent_path().c_str(),
                              O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    if (!directory || ::fsync(directory.get()) != 0) {
      return std::unexpected(errno_failure(CredentialErrorCode::sync_failed,
                                           m_path.parent_path(),
                                           "cannot sync the credential directory"));
    }
    return {};
  } catch (...) {
    return std::unexpected(failure(CredentialErrorCode::write_failed, m_path,
                                   "the credential adapter failed safely"));
  }
}

auto resolve_credential(std::optional<std::string> environment_value,
                        CredentialStore& store)
    -> std::expected<CredentialResolution, CredentialError> {
  try {
    if (environment_value) {
      auto secret = make_secret(std::move(*environment_value));
      if (!secret) return std::unexpected(std::move(secret.error()));
      CredentialResolution resolution;
      resolution.credential.emplace(ResolvedCredential{
          std::move(*secret),
          {domain::CredentialSourceKind::environment, "VENICE_API_KEY"}});
      return resolution;
    }
    auto loaded = store.load();
    if (!loaded) {
      CredentialResolution resolution;
      resolution.warnings.push_back(std::move(loaded.error().message));
      return resolution;
    }
    CredentialResolution resolution;
    if (*loaded) {
      resolution.credential.emplace(ResolvedCredential{
          std::move(**loaded),
          {domain::CredentialSourceKind::configuration_file,
           "aiforge/credentials"}});
    }
    return resolution;
  } catch (...) {
    return std::unexpected(failure(CredentialErrorCode::read_failed, {},
                                   "credential resolution failed safely"));
  }
}

}  // namespace aiforge::credentials
