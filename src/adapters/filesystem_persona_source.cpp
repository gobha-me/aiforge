#include <aiforge/adapters/filesystem_persona_source.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace aiforge::adapters {
namespace {

using persona::PersonaEditorErrorCode;
using persona::PersonaErrorCode;

[[nodiscard]] auto failure(PersonaErrorCode code, std::string message,
                           std::optional<std::string> name = std::nullopt,
                           bool retryable = false)
    -> std::unexpected<persona::PersonaError> {
  return std::unexpected(persona::PersonaError{code, std::move(message),
                                               std::move(name), retryable});
}

[[nodiscard]] auto edit_failure(
    PersonaEditorErrorCode code, std::string message,
    std::optional<domain::PersonaReference> observed = std::nullopt,
    bool retryable = false, bool may_have_applied = false)
    -> std::unexpected<persona::PersonaEditorError> {
  return std::unexpected(
      persona::PersonaEditorError{code, std::move(message), std::move(observed),
                                  retryable, may_have_applied});
}

[[nodiscard]] auto valid_limits(const persona::PersonaLimits& limits) -> bool {
  return limits.maximum_personas != 0 && limits.maximum_name_bytes != 0 &&
         limits.maximum_file_bytes != 0 &&
         limits.maximum_description_bytes != 0;
}

[[nodiscard]] auto valid_name(const std::string_view value,
                              const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  const auto is_ascii_alnum = [](const unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (!is_ascii_alnum(static_cast<unsigned char>(value.front()))) return false;
  return std::ranges::all_of(value.substr(1),
                             [](const unsigned char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'A' && character <= 'Z') ||
                                      (character >= 'a' && character <= 'z') ||
                                      character == '-' || character == '_';
                             });
}

[[nodiscard]] auto canonical_name(std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::transform(result, result.begin(), [](const unsigned char ch) {
    return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
  });
  return result;
}

[[nodiscard]] auto content_digest(const std::string_view text) -> std::string {
  detail::Sha256 digest;
  digest.update(std::as_bytes(std::span{text.data(), text.size()}));
  return digest.finish();
}

struct IndexedPersona {
  std::string canonical;
  std::string name;
  std::string filename;
};

#ifndef _WIN32
class UniqueFd final {
 public:
  explicit UniqueFd(const int value = -1) : m_value(value) {}
  UniqueFd(const UniqueFd&) = delete;
  auto operator=(const UniqueFd&) -> UniqueFd& = delete;
  UniqueFd(UniqueFd&& other) noexcept
      : m_value(std::exchange(other.m_value, -1)) {}
  auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
    if (this != &other) reset(std::exchange(other.m_value, -1));
    return *this;
  }
  ~UniqueFd() { reset(); }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  [[nodiscard]] explicit operator bool() const noexcept { return m_value >= 0; }
  auto reset(const int value = -1) noexcept -> void {
    if (m_value >= 0) static_cast<void>(::close(m_value));
    m_value = value;
  }

 private:
  int m_value;
};

class UniqueDirectory final {
 public:
  explicit UniqueDirectory(DIR* value) : m_value(value) {}
  UniqueDirectory(const UniqueDirectory&) = delete;
  auto operator=(const UniqueDirectory&) -> UniqueDirectory& = delete;
  ~UniqueDirectory() {
    if (m_value != nullptr) static_cast<void>(::closedir(m_value));
  }
  [[nodiscard]] auto get() const noexcept -> DIR* { return m_value; }

 private:
  DIR* m_value;
};

[[nodiscard]] auto same_file(const struct stat& left, const struct stat& right)
    -> bool {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

[[nodiscard]] auto open_existing(const char* path, const int flags) -> int {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX open API.
  return ::open(path, flags);
}

[[nodiscard]] auto open_existing_at(const int directory, const char* path,
                                    const int flags) -> int {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX openat API.
  return ::openat(directory, path, flags);
}

[[nodiscard]] auto create_at(const int directory, const char* path,
                             const int flags, const mode_t mode) -> int {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX create API.
  return ::openat(directory, path, flags, mode);
}

[[nodiscard]] auto exchange_entries(const int directory,
                                    const std::string& left,
                                    const std::string& right) -> int {
#ifdef __linux__
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux syscall API.
  return static_cast<int>(::syscall(SYS_renameat2, directory, left.c_str(),
                                    directory, right.c_str(), RENAME_EXCHANGE));
#else
  static_cast<void>(directory);
  static_cast<void>(left);
  static_cast<void>(right);
  errno = ENOTSUP;
  return -1;
#endif
}

[[nodiscard]] auto open_read_root(const std::filesystem::path& root,
                                  const bool missing_is_empty)
    -> std::expected<std::optional<UniqueFd>, persona::PersonaError> {
  if (!root.is_absolute()) {
    return failure(PersonaErrorCode::invalid_root,
                   "persona root must be absolute");
  }
  struct stat path_state{};
  if (::lstat(root.c_str(), &path_state) != 0) {
    if (errno == ENOENT && missing_is_empty) return std::nullopt;
    return failure(errno == ENOENT   ? PersonaErrorCode::not_found
                   : errno == EACCES ? PersonaErrorCode::permission_denied
                                     : PersonaErrorCode::io_failure,
                   errno == ENOENT ? "persona directory does not exist"
                                   : "persona directory could not be inspected",
                   std::nullopt, errno != ENOENT);
  }
  if (S_ISLNK(path_state.st_mode)) {
    return failure(PersonaErrorCode::path_escape,
                   "persona directory cannot be a symbolic link");
  }
  if (!S_ISDIR(path_state.st_mode)) {
    return failure(PersonaErrorCode::invalid_root,
                   "persona root is not a directory");
  }
  UniqueFd descriptor{open_existing(
      root.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  struct stat opened{};
  if (!descriptor || ::fstat(descriptor.get(), &opened) != 0) {
    return failure(errno == EACCES ? PersonaErrorCode::permission_denied
                                   : PersonaErrorCode::io_failure,
                   "persona directory could not be opened", std::nullopt, true);
  }
  if (!S_ISDIR(opened.st_mode) || opened.st_dev != path_state.st_dev ||
      opened.st_ino != path_state.st_ino) {
    return failure(PersonaErrorCode::invalid_root,
                   "persona root is not a stable directory");
  }
  return std::optional<UniqueFd>{std::move(descriptor)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Hostile entries.
[[nodiscard]] auto index_personas(const int root_descriptor,
                                  const persona::PersonaLimits& limits,
                                  const std::stop_token stop_token)
    -> std::expected<std::vector<IndexedPersona>, persona::PersonaError> {
  const auto iterator_descriptor = open_existing_at(
      root_descriptor, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (iterator_descriptor < 0) {
    return failure(PersonaErrorCode::io_failure,
                   "persona directory could not be listed", std::nullopt, true);
  }
  UniqueDirectory directory{::fdopendir(iterator_descriptor)};
  if (directory.get() == nullptr) {
    static_cast<void>(::close(iterator_descriptor));
    return failure(PersonaErrorCode::io_failure,
                   "persona directory could not be listed", std::nullopt, true);
  }
  std::map<std::string, IndexedPersona> indexed;
  errno = 0;
  // NOLINTNEXTLINE(concurrency-mt-unsafe) -- Private directory stream.
  while (const auto* entry = ::readdir(directory.get())) {
    if (stop_token.stop_requested()) {
      return failure(PersonaErrorCode::cancelled, "persona listing cancelled");
    }
    const std::string filename{&entry->d_name[0]};
    const auto extension = std::filesystem::path{filename}.extension().string();
    if (extension != ".md" && extension != ".txt") continue;
    const auto name = std::filesystem::path{filename}.stem().string();
    if (!valid_name(name, limits.maximum_name_bytes)) {
      return failure(PersonaErrorCode::invalid_name,
                     "persona directory contains an invalid persona name",
                     name);
    }
    struct stat state{};
    if (::fstatat(root_descriptor, filename.c_str(), &state,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      return failure(errno == EACCES ? PersonaErrorCode::permission_denied
                                     : PersonaErrorCode::io_failure,
                     "persona entry could not be inspected", name, true);
    }
    if (S_ISLNK(state.st_mode)) {
      return failure(PersonaErrorCode::path_escape,
                     "persona entry cannot be a symbolic link", name);
    }
    if (!S_ISREG(state.st_mode)) {
      return failure(PersonaErrorCode::unsupported_entry,
                     "persona entry must be a regular file", name);
    }
    auto canonical = canonical_name(name);
    IndexedPersona indexed_entry{canonical, name, filename};
    if (!indexed.emplace(canonical, std::move(indexed_entry)).second) {
      return failure(PersonaErrorCode::ambiguous_name,
                     "persona name has a case or extension alias", name);
    }
    if (indexed.size() > limits.maximum_personas) {
      return failure(PersonaErrorCode::resource_exhausted,
                     "persona listing exceeds its entry limit");
    }
    errno = 0;
  }
  if (errno != 0) {
    return failure(errno == EACCES ? PersonaErrorCode::permission_denied
                                   : PersonaErrorCode::io_failure,
                   "persona directory could not be listed", std::nullopt, true);
  }
  std::vector<IndexedPersona> result;
  result.reserve(indexed.size());
  for (auto& [key, entry] : indexed) {
    static_cast<void>(key);
    result.push_back(std::move(entry));
  }
  return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Stable reads.
[[nodiscard]] auto load_indexed(const int root_descriptor,
                                const IndexedPersona& entry,
                                const persona::PersonaLimits& limits,
                                const std::stop_token stop_token)
    -> std::expected<domain::PersonaDocument, persona::PersonaError> {
  UniqueFd descriptor{open_existing_at(root_descriptor, entry.filename.c_str(),
                                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (!descriptor) {
    if (errno == ELOOP) {
      return failure(PersonaErrorCode::path_escape,
                     "persona entry cannot be a symbolic link", entry.name);
    }
    return failure(errno == ENOENT   ? PersonaErrorCode::not_found
                   : errno == EACCES ? PersonaErrorCode::permission_denied
                                     : PersonaErrorCode::io_failure,
                   "persona file could not be opened", entry.name, true);
  }
  struct stat before{};
  if (::fstat(descriptor.get(), &before) != 0 || !S_ISREG(before.st_mode)) {
    return failure(PersonaErrorCode::unsupported_entry,
                   "persona entry must be a regular file", entry.name);
  }
  if (before.st_size <= 0 ||
      static_cast<std::uint64_t>(before.st_size) > limits.maximum_file_bytes) {
    return failure(before.st_size <= 0 ? PersonaErrorCode::malformed_text
                                       : PersonaErrorCode::resource_exhausted,
                   before.st_size <= 0 ? "persona file is empty"
                                       : "persona file exceeds its byte limit",
                   entry.name);
  }
  std::string text;
  text.reserve(static_cast<std::size_t>(before.st_size));
  std::array<char, 8192> buffer{};
  for (;;) {
    if (stop_token.stop_requested()) {
      return failure(PersonaErrorCode::cancelled, "persona loading cancelled",
                     entry.name);
    }
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return failure(PersonaErrorCode::io_failure,
                     "persona file could not be read", entry.name, true);
    }
    if (static_cast<std::size_t>(count) >
        limits.maximum_file_bytes - text.size()) {
      return failure(PersonaErrorCode::resource_exhausted,
                     "persona file exceeds its byte limit", entry.name);
    }
    text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  struct stat after{};
  struct stat path_state{};
  if (::fstat(descriptor.get(), &after) != 0 ||
      ::fstatat(root_descriptor, entry.filename.c_str(), &path_state,
                AT_SYMLINK_NOFOLLOW) != 0) {
    return failure(PersonaErrorCode::io_failure,
                   "persona file could not be verified", entry.name, true);
  }
  if (!same_file(before, after) || after.st_dev != path_state.st_dev ||
      after.st_ino != path_state.st_ino || S_ISLNK(path_state.st_mode) ||
      static_cast<std::uint64_t>(after.st_size) != text.size()) {
    return failure(PersonaErrorCode::unstable,
                   "persona file changed while it was being read", entry.name,
                   true);
  }
  if (!detail::is_safe_utf8_text(text)) {
    return failure(
        PersonaErrorCode::malformed_text,
        "persona file must be nonempty UTF-8 text without unsafe controls",
        entry.name);
  }
  auto persona_id = domain::PersonaId::from("persona:" + entry.canonical);
  if (!persona_id) {
    return failure(PersonaErrorCode::internal_failure,
                   "persona identity could not be represented", entry.name);
  }
  return domain::PersonaDocument{
      {std::move(*persona_id),
       entry.name,
       "personas/" + entry.filename,
       {"sha256", content_digest(text), text.size()}},
      std::move(text)};
}

[[nodiscard]] auto editor_error(persona::PersonaError error)
    -> persona::PersonaEditorError {
  auto code = PersonaEditorErrorCode::io_failure;
  switch (error.code) {
    case PersonaErrorCode::invalid_request:
      code = PersonaEditorErrorCode::invalid_request;
      break;
    case PersonaErrorCode::invalid_name:
      code = PersonaEditorErrorCode::invalid_name;
      break;
    case PersonaErrorCode::not_found:
      code = PersonaEditorErrorCode::not_found;
      break;
    case PersonaErrorCode::path_escape:
    case PersonaErrorCode::invalid_root:
      code = PersonaEditorErrorCode::path_escape;
      break;
    case PersonaErrorCode::unsupported_entry:
    case PersonaErrorCode::ambiguous_name:
      code = PersonaEditorErrorCode::unsupported_entry;
      break;
    case PersonaErrorCode::malformed_text:
      code = PersonaEditorErrorCode::malformed_text;
      break;
    case PersonaErrorCode::unstable:
      code = PersonaEditorErrorCode::concurrent_change;
      break;
    case PersonaErrorCode::resource_exhausted:
      code = PersonaEditorErrorCode::resource_exhausted;
      break;
    case PersonaErrorCode::permission_denied:
      code = PersonaEditorErrorCode::permission_denied;
      break;
    case PersonaErrorCode::cancelled:
      code = PersonaEditorErrorCode::cancelled;
      break;
    case PersonaErrorCode::missing_home:
    case PersonaErrorCode::io_failure:
      code = PersonaEditorErrorCode::io_failure;
      break;
    case PersonaErrorCode::internal_failure:
      code = PersonaEditorErrorCode::internal_failure;
      break;
  }
  return {code, std::move(error.message), std::nullopt, error.retryable, false};
}

struct WriteRoot {
  UniqueFd parent;
  UniqueFd directory;
  std::string basename;
  struct stat identity{};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Secure paths.
[[nodiscard]] auto ensure_private_directory(const std::filesystem::path& path)
    -> std::expected<UniqueFd, persona::PersonaEditorError> {
  if (!path.is_absolute() || path.empty() || path.lexically_normal() != path) {
    return edit_failure(PersonaEditorErrorCode::invalid_request,
                        "persona parent must be an absolute normalized path");
  }

  UniqueFd current{
      open_existing("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  if (!current) {
    return edit_failure(PersonaEditorErrorCode::io_failure,
                        "filesystem root could not be opened", {}, true);
  }

  const auto relative = path.relative_path();
  for (auto iterator = relative.begin(); iterator != relative.end();
       ++iterator) {
    const auto component = iterator->string();
    if (component.empty() || component == "." || component == "..") {
      return edit_failure(PersonaEditorErrorCode::invalid_request,
                          "persona parent contains an invalid component");
    }
    const bool final = std::next(iterator) == relative.end();
    struct stat path_state{};
    bool created{};
    if (::fstatat(current.get(), component.c_str(), &path_state,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT) {
        return edit_failure(
            errno == EACCES ? PersonaEditorErrorCode::permission_denied
                            : PersonaEditorErrorCode::io_failure,
            "persona parent component could not be inspected", {}, true);
      }
      if (::mkdirat(current.get(), component.c_str(), 0700) == 0) {
        created = true;
      } else if (errno != EEXIST) {
        return edit_failure(
            errno == EACCES ? PersonaEditorErrorCode::permission_denied
                            : PersonaEditorErrorCode::io_failure,
            "persona parent component could not be created", {}, true);
      }
      if (::fstatat(current.get(), component.c_str(), &path_state,
                    AT_SYMLINK_NOFOLLOW) != 0) {
        return edit_failure(PersonaEditorErrorCode::io_failure,
                            "created persona parent could not be inspected", {},
                            true);
      }
    }
    if (S_ISLNK(path_state.st_mode)) {
      return edit_failure(PersonaEditorErrorCode::path_escape,
                          "persona parent cannot traverse a symbolic link");
    }
    if (!S_ISDIR(path_state.st_mode)) {
      return edit_failure(PersonaEditorErrorCode::unsupported_entry,
                          "persona parent component is not a directory");
    }

    UniqueFd next{
        open_existing_at(current.get(), component.c_str(),
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    struct stat opened{};
    if (!next || ::fstat(next.get(), &opened) != 0) {
      return edit_failure(
          errno == ELOOP || errno == ENOTDIR
              ? PersonaEditorErrorCode::path_escape
          : errno == EACCES ? PersonaEditorErrorCode::permission_denied
                            : PersonaEditorErrorCode::io_failure,
          "persona parent component could not be opened", {}, true);
    }
    if (!S_ISDIR(opened.st_mode) || opened.st_dev != path_state.st_dev ||
        opened.st_ino != path_state.st_ino) {
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "persona parent changed while it was opened", {},
                          true);
    }
    if (created && ::fchmod(next.get(), 0700) != 0) {
      return edit_failure(PersonaEditorErrorCode::permission_denied,
                          "persona parent permissions could not be restricted");
    }
    if (created && (::fsync(next.get()) != 0 || ::fsync(current.get()) != 0)) {
      return edit_failure(PersonaEditorErrorCode::durability_failure,
                          "created persona parent could not be synchronized",
                          {}, true);
    }
    if (final &&
        (opened.st_uid != ::geteuid() || (opened.st_mode & 0077) != 0)) {
      return edit_failure(
          PersonaEditorErrorCode::permission_denied,
          "persona parent directory must be owned by the current "
          "user with mode 0700");
    }
    current = std::move(next);
  }

  struct stat attributes{};
  if (::fstat(current.get(), &attributes) != 0 ||
      !S_ISDIR(attributes.st_mode) || attributes.st_uid != ::geteuid() ||
      (attributes.st_mode & 0077) != 0) {
    return edit_failure(PersonaEditorErrorCode::permission_denied,
                        "persona parent directory must be owned by the current "
                        "user with mode 0700");
  }
  return current;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Root identity.
[[nodiscard]] auto open_write_root(const std::filesystem::path& root)
    -> std::expected<WriteRoot, persona::PersonaEditorError> {
  if (!root.is_absolute() || root.filename().empty() ||
      root.lexically_normal() != root) {
    return edit_failure(PersonaEditorErrorCode::invalid_request,
                        "persona root must be an absolute normalized path");
  }
  auto parent = ensure_private_directory(root.parent_path());
  if (!parent) return std::unexpected(std::move(parent.error()));
  const auto basename = root.filename().string();
  struct stat path_state{};
  bool created{};
  if (::fstatat(parent->get(), basename.c_str(), &path_state,
                AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno != ENOENT) {
      return edit_failure(errno == EACCES
                              ? PersonaEditorErrorCode::permission_denied
                              : PersonaEditorErrorCode::io_failure,
                          "persona root could not be inspected", {}, true);
    }
    if (::mkdirat(parent->get(), basename.c_str(), 0700) == 0) {
      created = true;
    } else if (errno != EEXIST) {
      return edit_failure(errno == EACCES
                              ? PersonaEditorErrorCode::permission_denied
                              : PersonaEditorErrorCode::io_failure,
                          "persona root could not be created", {}, true);
    }
    if (::fstatat(parent->get(), basename.c_str(), &path_state,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      return edit_failure(PersonaEditorErrorCode::io_failure,
                          "created persona root could not be inspected", {},
                          true);
    }
  }
  if (S_ISLNK(path_state.st_mode)) {
    return edit_failure(PersonaEditorErrorCode::path_escape,
                        "persona root cannot be a symbolic link");
  }
  if (!S_ISDIR(path_state.st_mode)) {
    return edit_failure(PersonaEditorErrorCode::unsupported_entry,
                        "persona root is not a directory");
  }
  UniqueFd directory{
      open_existing_at(parent->get(), basename.c_str(),
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  struct stat opened{};
  if (!directory || ::fstat(directory.get(), &opened) != 0) {
    return edit_failure(errno == EACCES
                            ? PersonaEditorErrorCode::permission_denied
                            : PersonaEditorErrorCode::io_failure,
                        "persona root could not be opened", {}, true);
  }
  if (created && ::fchmod(directory.get(), 0700) != 0) {
    return edit_failure(PersonaEditorErrorCode::permission_denied,
                        "persona root permissions could not be restricted");
  }
  if (created && ::fstat(directory.get(), &opened) != 0) {
    return edit_failure(PersonaEditorErrorCode::io_failure,
                        "created persona root could not be verified", {}, true);
  }
  if (created && ::fsync(parent->get()) != 0) {
    return edit_failure(PersonaEditorErrorCode::durability_failure,
                        "created persona root could not be synchronized", {},
                        true);
  }
  if (!S_ISDIR(opened.st_mode) || opened.st_dev != path_state.st_dev ||
      opened.st_ino != path_state.st_ino) {
    return edit_failure(PersonaEditorErrorCode::concurrent_change,
                        "persona root changed while it was being opened", {},
                        true);
  }
  if (opened.st_uid != ::geteuid() || (opened.st_mode & 0077) != 0) {
    return edit_failure(PersonaEditorErrorCode::permission_denied,
                        "persona root must be owned by the current user with "
                        "mode 0700");
  }
  return WriteRoot{std::move(*parent), std::move(directory), basename, opened};
}

[[nodiscard]] auto root_unchanged(const WriteRoot& root)
    -> std::expected<void, persona::PersonaEditorError> {
  struct stat current{};
  if (::fstatat(root.parent.get(), root.basename.c_str(), &current,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(current.st_mode) || S_ISLNK(current.st_mode) ||
      current.st_dev != root.identity.st_dev ||
      current.st_ino != root.identity.st_ino || current.st_uid != ::geteuid() ||
      (current.st_mode & 0077) != 0) {
    return edit_failure(PersonaEditorErrorCode::concurrent_change,
                        "persona root changed before publication", {}, true);
  }
  return {};
}

[[nodiscard]] auto acquire_write_lock(const int root_descriptor,
                                      const std::stop_token stop_token)
    -> std::expected<UniqueFd, persona::PersonaEditorError> {
  UniqueFd lock{create_at(root_descriptor, ".aiforge-personas.lock",
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (!lock) {
    return edit_failure(errno == ELOOP ? PersonaEditorErrorCode::path_escape
                        : errno == EACCES
                            ? PersonaEditorErrorCode::permission_denied
                            : PersonaEditorErrorCode::io_failure,
                        "persona write lock could not be opened", {}, true);
  }
  struct stat attributes{};
  if (::fstat(lock.get(), &attributes) != 0 || !S_ISREG(attributes.st_mode) ||
      attributes.st_uid != ::geteuid() || attributes.st_nlink != 1) {
    return edit_failure(PersonaEditorErrorCode::permission_denied,
                        "persona write lock has unsafe ownership or type");
  }
  if (::fchmod(lock.get(), 0600) != 0) {
    return edit_failure(
        PersonaEditorErrorCode::permission_denied,
        "persona write lock permissions could not be restricted");
  }
  while (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
      return edit_failure(PersonaEditorErrorCode::io_failure,
                          "persona write lock could not be acquired", {}, true);
    }
    if (stop_token.stop_requested()) {
      return edit_failure(PersonaEditorErrorCode::cancelled,
                          "persona write cancelled while waiting for its lock");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  return lock;
}

[[nodiscard]] auto write_all(const int descriptor, const std::string_view text,
                             const std::stop_token stop_token)
    -> std::expected<void, persona::PersonaEditorError> {
  std::size_t offset{};
  while (offset < text.size()) {
    if (stop_token.stop_requested()) {
      return edit_failure(PersonaEditorErrorCode::cancelled,
                          "persona write cancelled before publication");
    }
    const auto count =
        ::write(descriptor, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      return edit_failure(
          errno == ENOSPC ? PersonaEditorErrorCode::resource_exhausted
                          : PersonaEditorErrorCode::io_failure,
          "persona temporary file could not be written", {}, true);
    }
    if (count == 0) {
      return edit_failure(PersonaEditorErrorCode::io_failure,
                          "persona temporary write made no progress", {}, true);
    }
    offset += static_cast<std::size_t>(count);
  }
  return {};
}

class PreparedPersona final {
 public:
  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Secure file stages.
  [[nodiscard]] static auto create(const int root_descriptor,
                                   const std::string_view text,
                                   const std::stop_token stop_token)
      -> std::expected<PreparedPersona, persona::PersonaEditorError> {
    // clang-format on
    static std::atomic_uint64_t sequence{};
    for (std::size_t attempt{}; attempt < 128; ++attempt) {
      auto name = ".aiforge-persona-" + std::to_string(::getpid()) + "-" +
                  std::to_string(sequence.fetch_add(1));
      UniqueFd descriptor{create_at(
          root_descriptor, name.c_str(),
          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
      if (!descriptor && errno == EEXIST) continue;
      if (!descriptor) {
        return edit_failure(
            errno == EACCES ? PersonaEditorErrorCode::permission_denied
                            : PersonaEditorErrorCode::io_failure,
            "persona temporary file could not be created", {}, true);
      }
      struct stat state{};
      if (::fstat(descriptor.get(), &state) != 0 || !S_ISREG(state.st_mode) ||
          state.st_uid != ::geteuid() || state.st_nlink != 1 ||
          ::fchmod(descriptor.get(), 0600) != 0) {
        static_cast<void>(::unlinkat(root_descriptor, name.c_str(), 0));
        return edit_failure(PersonaEditorErrorCode::permission_denied,
                            "persona temporary file has unsafe attributes");
      }
      PreparedPersona prepared{root_descriptor, std::move(name),
                               std::move(descriptor)};
      if (auto written =
              write_all(prepared.m_descriptor.get(), text, stop_token);
          !written) {
        return std::unexpected(std::move(written.error()));
      }
      if (::fsync(prepared.m_descriptor.get()) != 0) {
        return edit_failure(
            errno == ENOSPC ? PersonaEditorErrorCode::resource_exhausted
                            : PersonaEditorErrorCode::io_failure,
            "persona temporary file could not be synchronized", {}, true);
      }
      struct stat complete{};
      if (::fstat(prepared.m_descriptor.get(), &complete) != 0 ||
          complete.st_size < 0 ||
          static_cast<std::size_t>(complete.st_size) != text.size()) {
        return edit_failure(PersonaEditorErrorCode::io_failure,
                            "persona temporary file could not be verified", {},
                            true);
      }
      return prepared;
    }
    return edit_failure(PersonaEditorErrorCode::resource_exhausted,
                        "persona temporary name space is exhausted", {}, true);
  }

  PreparedPersona(PreparedPersona&& other) noexcept
      : m_root_descriptor(other.m_root_descriptor),
        m_name(std::move(other.m_name)),
        m_descriptor(std::move(other.m_descriptor)) {
    other.m_name.clear();
  }
  PreparedPersona(const PreparedPersona&) = delete;
  auto operator=(const PreparedPersona&) -> PreparedPersona& = delete;
  ~PreparedPersona() {
    if (!m_name.empty())
      static_cast<void>(::unlinkat(m_root_descriptor, m_name.c_str(), 0));
  }
  [[nodiscard]] auto name() const noexcept -> const std::string& {
    return m_name;
  }
  [[nodiscard]] auto path_is_prepared() const noexcept -> bool {
    struct stat descriptor_state{};
    struct stat path_state{};
    return !m_name.empty() &&
           ::fstat(m_descriptor.get(), &descriptor_state) == 0 &&
           ::fstatat(m_root_descriptor, m_name.c_str(), &path_state,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(path_state.st_mode) &&
           descriptor_state.st_dev == path_state.st_dev &&
           descriptor_state.st_ino == path_state.st_ino;
  }
  auto disarm() noexcept -> void { m_name.clear(); }

 private:
  PreparedPersona(const int root_descriptor, std::string name,
                  UniqueFd descriptor)
      : m_root_descriptor(root_descriptor), m_name(std::move(name)),
        m_descriptor(std::move(descriptor)) {}

  int m_root_descriptor;
  std::string m_name;
  UniqueFd m_descriptor;
};

[[nodiscard]] auto indexed_for_name(const std::vector<IndexedPersona>& indexed,
                                    const std::string_view name)
    -> const IndexedPersona* {
  const auto canonical = canonical_name(name);
  const auto found =
      std::ranges::find(indexed, canonical, &IndexedPersona::canonical);
  return found == indexed.end() ? nullptr : &*found;
}

[[nodiscard]] auto editor_index(const int root_descriptor,
                                const persona::PersonaLimits& limits,
                                const std::stop_token stop_token)
    -> std::expected<std::vector<IndexedPersona>, persona::PersonaEditorError> {
  auto indexed = index_personas(root_descriptor, limits, stop_token);
  if (!indexed)
    return std::unexpected(editor_error(std::move(indexed.error())));
  return std::move(*indexed);
}

[[nodiscard]] auto editor_load(const int root_descriptor,
                               const IndexedPersona& indexed,
                               const persona::PersonaLimits& limits,
                               const std::stop_token stop_token)
    -> std::expected<domain::PersonaDocument, persona::PersonaEditorError> {
  struct stat attributes{};
  if (::fstatat(root_descriptor, indexed.filename.c_str(), &attributes,
                AT_SYMLINK_NOFOLLOW) != 0) {
    return edit_failure(
        errno == EACCES ? PersonaEditorErrorCode::permission_denied
                        : PersonaEditorErrorCode::io_failure,
        "persona file ownership could not be inspected", {}, true);
  }
  if (attributes.st_uid != ::geteuid()) {
    return edit_failure(PersonaEditorErrorCode::permission_denied,
                        "persona file must be owned by the current user");
  }
  auto loaded = load_indexed(root_descriptor, indexed, limits, stop_token);
  if (!loaded) return std::unexpected(editor_error(std::move(loaded.error())));
  return std::move(*loaded);
}
#endif

[[nodiscard]] auto bounded_description(const std::string_view text,
                                       const std::size_t maximum)
    -> std::string {
  for (const auto raw : text | std::views::split('\n')) {
    std::string_view line{raw.begin(), raw.end()};
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' ||
                             line.front() == '\r'))
      line.remove_prefix(1);
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
      line.remove_suffix(1);
    if (line.empty()) continue;
    std::size_t bytes{};
    while (bytes < line.size() && bytes < maximum) {
      const auto lead = static_cast<unsigned char>(line[bytes]);
      const std::size_t length = lead < 0x80U   ? 1U
                                 : lead < 0xe0U ? 2U
                                 : lead < 0xf0U ? 3U
                                                : 4U;
      if (length > maximum - bytes) break;
      bytes += length;
    }
    return std::string{line.substr(0, bytes)};
  }
  return {};
}

} // namespace

auto resolve_persona_root(const config::ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path, persona::PersonaError> {
  auto config_path = config::resolve_config_path(environment);
  if (!config_path) {
    return failure(config_path.error().code ==
                           config::ConfigFileErrorCode::missing_home
                       ? PersonaErrorCode::missing_home
                       : PersonaErrorCode::invalid_root,
                   "persona root could not be resolved");
  }
  return config_path->parent_path() / "personas";
}

auto process_persona_root()
    -> std::expected<std::filesystem::path, persona::PersonaError> {
  try {
    config::ConfigPathEnvironment environment;
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- Startup environment snapshot.
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"))
      environment.xdg_config_home = std::filesystem::path{xdg};
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- Startup environment snapshot.
    if (const auto* home = std::getenv("HOME"))
      environment.home = std::filesystem::path{home};
    return resolve_persona_root(environment);
  } catch (...) {
    return failure(PersonaErrorCode::invalid_root,
                   "persona root could not be resolved");
  }
}

auto FilesystemPersonaSource::list(const persona::PersonaLimits limits,
                                   const std::stop_token stop_token)
    -> std::expected<std::vector<domain::PersonaSummary>,
                     persona::PersonaError> {
  try {
    if (!valid_limits(limits)) {
      return failure(PersonaErrorCode::invalid_request,
                     "persona limits are invalid");
    }
#ifdef _WIN32
    static_cast<void>(stop_token);
    return failure(PersonaErrorCode::io_failure,
                   "filesystem personas are unavailable on this platform");
#else
    auto root = open_read_root(m_root, true);
    if (!root) return std::unexpected(std::move(root.error()));
    if (!root->has_value()) return std::vector<domain::PersonaSummary>{};
    auto root_directory = std::move(*root);
    if (!root_directory) return std::vector<domain::PersonaSummary>{};
    const int root_descriptor = root_directory->get();
    auto indexed = index_personas(root_descriptor, limits, stop_token);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    std::vector<domain::PersonaSummary> result;
    result.reserve(indexed->size());
    for (const auto& entry : *indexed) {
      auto document = load_indexed(root_descriptor, entry, limits, stop_token);
      if (!document) return std::unexpected(std::move(document.error()));
      result.push_back({document->reference,
                        bounded_description(document->text,
                                            limits.maximum_description_bytes)});
    }
    return result;
#endif
  } catch (...) {
    return failure(PersonaErrorCode::internal_failure,
                   "persona listing failed internally");
  }
}

auto FilesystemPersonaSource::load(std::string name,
                                   const persona::PersonaLimits limits,
                                   const std::stop_token stop_token)
    -> std::expected<domain::PersonaDocument, persona::PersonaError> {
  try {
    if (!valid_limits(limits)) {
      return failure(PersonaErrorCode::invalid_request,
                     "persona limits are invalid", std::move(name));
    }
    if (!valid_name(name, limits.maximum_name_bytes)) {
      return failure(PersonaErrorCode::invalid_name,
                     "persona name must be a bounded bare name",
                     std::move(name));
    }
#ifdef _WIN32
    static_cast<void>(stop_token);
    return failure(PersonaErrorCode::io_failure,
                   "persona loading is unavailable on this platform",
                   std::move(name));
#else
    auto root = open_read_root(m_root, false);
    if (!root) return std::unexpected(std::move(root.error()));
    auto root_directory = std::move(*root);
    if (!root_directory) {
      return failure(PersonaErrorCode::not_found, "persona root was not found",
                     std::move(name));
    }
    const int root_descriptor = root_directory->get();
    auto indexed = index_personas(root_descriptor, limits, stop_token);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    const auto* found = indexed_for_name(*indexed, name);
    if (found == nullptr) {
      return failure(PersonaErrorCode::not_found, "persona was not found",
                     std::move(name));
    }
    return load_indexed(root_descriptor, *found, limits, stop_token);
#endif
  } catch (...) {
    return failure(PersonaErrorCode::internal_failure,
                   "persona loading failed internally", std::move(name));
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Atomic create.
auto FilesystemPersonaSource::create(persona::PersonaCreate request,
                                     const std::stop_token stop_token)
    -> std::expected<persona::PersonaWriteReceipt,
                     persona::PersonaEditorError> {
  bool published{};
  try {
    auto candidate = persona::prepare_persona_create(request);
    if (!candidate) return std::unexpected(std::move(candidate.error()));
    if (stop_token.stop_requested()) {
      return edit_failure(PersonaEditorErrorCode::cancelled,
                          "persona creation cancelled");
    }
#ifdef _WIN32
    return edit_failure(PersonaEditorErrorCode::io_failure,
                        "persona creation is unavailable on this platform");
#else
    auto root = open_write_root(m_root);
    if (!root) return std::unexpected(std::move(root.error()));
    auto lock = acquire_write_lock(root->directory.get(), stop_token);
    if (!lock) return std::unexpected(std::move(lock.error()));
    auto indexed =
        editor_index(root->directory.get(), request.limits, stop_token);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    if (indexed_for_name(*indexed, request.draft.name) != nullptr) {
      return edit_failure(PersonaEditorErrorCode::already_exists,
                          "persona name already exists");
    }
    if (indexed->size() >= request.limits.maximum_personas) {
      return edit_failure(PersonaEditorErrorCode::resource_exhausted,
                          "persona directory has reached its entry limit");
    }
    auto prepared = PreparedPersona::create(root->directory.get(),
                                            request.draft.text, stop_token);
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    if (m_checkpoint) {
      auto checkpoint =
          m_checkpoint(PersonaFilesystemCheckpointStage::temporary_synced);
      if (!checkpoint) return std::unexpected(std::move(checkpoint.error()));
    }
    indexed = editor_index(root->directory.get(), request.limits, stop_token);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    if (indexed_for_name(*indexed, request.draft.name) != nullptr) {
      return edit_failure(PersonaEditorErrorCode::already_exists,
                          "persona name appeared before publication");
    }
    if (indexed->size() >= request.limits.maximum_personas) {
      return edit_failure(PersonaEditorErrorCode::resource_exhausted,
                          "persona directory reached its entry limit before "
                          "publication",
                          {}, true);
    }
    if (auto stable = root_unchanged(*root); !stable)
      return std::unexpected(std::move(stable.error()));
    if (stop_token.stop_requested()) {
      return edit_failure(PersonaEditorErrorCode::cancelled,
                          "persona creation cancelled before publication");
    }
    const auto filename = candidate->reference.source_location.substr(9);
    if (::linkat(root->directory.get(), prepared->name().c_str(),
                 root->directory.get(), filename.c_str(), 0) != 0) {
      return edit_failure(
          errno == EEXIST   ? PersonaEditorErrorCode::already_exists
          : errno == EACCES ? PersonaEditorErrorCode::permission_denied
                            : PersonaEditorErrorCode::io_failure,
          errno == EEXIST ? "persona name collided during publication"
                          : "persona file could not be published",
          {}, errno != EEXIST);
    }
    published = true;
    std::optional<persona::PersonaEditorError> post_error;
    if (::unlinkat(root->directory.get(), prepared->name().c_str(), 0) != 0) {
      post_error = persona::PersonaEditorError{
          PersonaEditorErrorCode::io_failure,
          "persona was published but its temporary link could not be removed",
          {},
          true,
          true};
    } else {
      prepared->disarm();
    }
    if (m_checkpoint) {
      auto checkpoint =
          m_checkpoint(PersonaFilesystemCheckpointStage::published);
      if (!checkpoint && !post_error) {
        post_error = std::move(checkpoint.error());
        post_error->may_have_applied = true;
      }
    }
    if (::fsync(root->directory.get()) != 0 && !post_error) {
      post_error = persona::PersonaEditorError{
          PersonaEditorErrorCode::durability_failure,
          "persona was published but its directory could not be synchronized",
          {},
          true,
          true};
    }
    if (post_error) return std::unexpected(std::move(*post_error));
    auto after_index = editor_index(root->directory.get(), request.limits, {});
    if (!after_index) {
      auto error = std::move(after_index.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    const auto* resulting = indexed_for_name(*after_index, request.draft.name);
    if (resulting == nullptr) {
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "published persona could not be found", {}, true,
                          true);
    }
    auto loaded =
        editor_load(root->directory.get(), *resulting, request.limits, {});
    if (!loaded) {
      auto error = std::move(loaded.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    if (loaded->reference != candidate->reference) {
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "published persona did not match its candidate",
                          loaded->reference, true, true);
    }
    if (auto stable = root_unchanged(*root); !stable) {
      auto error = std::move(stable.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    persona::PersonaWriteReceipt receipt{std::nullopt, loaded->reference};
    if (auto valid = persona::validate_persona_write_receipt(request, receipt);
        !valid) {
      auto error = std::move(valid.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    return receipt;
#endif
  } catch (...) {
    return edit_failure(PersonaEditorErrorCode::internal_failure,
                        "persona creation failed internally", {}, false,
                        published);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Exact replace.
auto FilesystemPersonaSource::replace(persona::PersonaReplace request,
                                      const std::stop_token stop_token)
    -> std::expected<persona::PersonaWriteReceipt,
                     persona::PersonaEditorError> {
  bool published{};
  try {
    auto candidate = persona::prepare_persona_replace(request);
    if (!candidate) return std::unexpected(std::move(candidate.error()));
    if (stop_token.stop_requested()) {
      return edit_failure(PersonaEditorErrorCode::cancelled,
                          "persona replacement cancelled");
    }
#ifdef _WIN32
    return edit_failure(PersonaEditorErrorCode::io_failure,
                        "persona replacement is unavailable on this platform");
#else
    auto root = open_write_root(m_root);
    if (!root) return std::unexpected(std::move(root.error()));
    auto lock = acquire_write_lock(root->directory.get(), stop_token);
    if (!lock) return std::unexpected(std::move(lock.error()));
    auto indexed =
        editor_index(root->directory.get(), request.limits, stop_token);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    const auto* found = indexed_for_name(*indexed, request.expected.name);
    if (found == nullptr) {
      return edit_failure(PersonaEditorErrorCode::not_found,
                          "persona to replace was not found");
    }
    auto current =
        editor_load(root->directory.get(), *found, request.limits, stop_token);
    if (!current) return std::unexpected(std::move(current.error()));
    if (current->reference != request.expected) {
      return edit_failure(PersonaEditorErrorCode::source_mismatch,
                          "persona replacement precondition no longer matches",
                          current->reference, true);
    }
    auto prepared = PreparedPersona::create(root->directory.get(), request.text,
                                            stop_token);
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    if (m_checkpoint) {
      auto checkpoint =
          m_checkpoint(PersonaFilesystemCheckpointStage::temporary_synced);
      if (!checkpoint) return std::unexpected(std::move(checkpoint.error()));
    }
    indexed = editor_index(root->directory.get(), request.limits, stop_token);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    found = indexed_for_name(*indexed, request.expected.name);
    if (found == nullptr) {
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "persona disappeared before replacement", {}, true);
    }
    auto final_check =
        editor_load(root->directory.get(), *found, request.limits, stop_token);
    if (!final_check) return std::unexpected(std::move(final_check.error()));
    if (final_check->reference != request.expected) {
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "persona changed before replacement",
                          final_check->reference, true);
    }
    if (auto stable = root_unchanged(*root); !stable)
      return std::unexpected(std::move(stable.error()));
    if (stop_token.stop_requested()) {
      return edit_failure(PersonaEditorErrorCode::cancelled,
                          "persona replacement cancelled before publication");
    }
    if (m_checkpoint) {
      auto checkpoint =
          m_checkpoint(PersonaFilesystemCheckpointStage::replacement_ready);
      if (!checkpoint) return std::unexpected(std::move(checkpoint.error()));
    }
    const auto target_filename = found->filename;
    if (exchange_entries(root->directory.get(), prepared->name(),
                         target_filename) != 0) {
      return edit_failure(
          errno == EACCES ? PersonaEditorErrorCode::permission_denied
                          : PersonaEditorErrorCode::io_failure,
          errno == ENOTSUP || errno == ENOSYS
              ? "exact persona replacement is unavailable on this filesystem"
              : "persona replacement could not be published",
          {}, true);
    }
    published = true;

    const IndexedPersona displaced{canonical_name(request.expected.name),
                                   request.expected.name, prepared->name()};
    auto displaced_document =
        editor_load(root->directory.get(), displaced, request.limits, {});
    const bool displaced_matches =
        displaced_document &&
        displaced_document->reference.persona_id ==
            request.expected.persona_id &&
        displaced_document->reference.name == request.expected.name &&
        displaced_document->reference.content_digest ==
            request.expected.content_digest;
    if (!displaced_matches) {
      std::optional<domain::PersonaReference> observed;
      if (displaced_document) {
        observed = displaced_document->reference;
        observed->source_location = request.expected.source_location;
      }
      std::optional<persona::PersonaEditorError> rollback_checkpoint_error;
      if (m_checkpoint) {
        auto checkpoint =
            m_checkpoint(PersonaFilesystemCheckpointStage::rollback_ready);
        if (!checkpoint) {
          rollback_checkpoint_error = std::move(checkpoint.error());
          rollback_checkpoint_error->may_have_applied = true;
        }
      }
      if (exchange_entries(root->directory.get(), prepared->name(),
                           target_filename) != 0) {
        prepared->disarm();
        return edit_failure(
            PersonaEditorErrorCode::concurrent_change,
            "persona changed during publication and rollback failed",
            std::move(observed), true, true);
      }
      if (!prepared->path_is_prepared()) {
        prepared->disarm();
        return edit_failure(
            PersonaEditorErrorCode::concurrent_change,
            "another persona change raced with exact-replacement rollback",
            std::move(observed), true, true);
      }
      if (::unlinkat(root->directory.get(), prepared->name().c_str(), 0) != 0) {
        return edit_failure(
            PersonaEditorErrorCode::io_failure,
            "persona rollback could not remove its prepared candidate",
            std::move(observed), true, true);
      }
      prepared->disarm();
      if (::fsync(root->directory.get()) != 0) {
        return edit_failure(PersonaEditorErrorCode::durability_failure,
                            "persona rollback could not be synchronized",
                            std::move(observed), true, true);
      }
      published = false;
      if (rollback_checkpoint_error) {
        return std::unexpected(std::move(*rollback_checkpoint_error));
      }
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "persona changed during exact replacement",
                          std::move(observed), true);
    }
    if (::unlinkat(root->directory.get(), prepared->name().c_str(), 0) != 0) {
      return edit_failure(
          PersonaEditorErrorCode::io_failure,
          "persona was replaced but its displaced file could not be removed",
          {}, true, true);
    }
    prepared->disarm();
    std::optional<persona::PersonaEditorError> post_error;
    if (m_checkpoint) {
      auto checkpoint =
          m_checkpoint(PersonaFilesystemCheckpointStage::published);
      if (!checkpoint) {
        post_error = std::move(checkpoint.error());
        post_error->may_have_applied = true;
      }
    }
    if (::fsync(root->directory.get()) != 0 && !post_error) {
      post_error = persona::PersonaEditorError{
          PersonaEditorErrorCode::durability_failure,
          "persona was replaced but its directory could not be synchronized",
          {},
          true,
          true};
    }
    if (post_error) return std::unexpected(std::move(*post_error));
    auto after_index = editor_index(root->directory.get(), request.limits, {});
    if (!after_index) {
      auto error = std::move(after_index.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    const auto* resulting =
        indexed_for_name(*after_index, request.expected.name);
    if (resulting == nullptr) {
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "replaced persona could not be found", {}, true,
                          true);
    }
    auto loaded =
        editor_load(root->directory.get(), *resulting, request.limits, {});
    if (!loaded) {
      auto error = std::move(loaded.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    if (loaded->reference != candidate->reference) {
      return edit_failure(PersonaEditorErrorCode::concurrent_change,
                          "replaced persona did not match its candidate",
                          loaded->reference, true, true);
    }
    if (auto stable = root_unchanged(*root); !stable) {
      auto error = std::move(stable.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    persona::PersonaWriteReceipt receipt{request.expected, loaded->reference};
    if (auto valid = persona::validate_persona_write_receipt(request, receipt);
        !valid) {
      auto error = std::move(valid.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    return receipt;
#endif
  } catch (...) {
    return edit_failure(PersonaEditorErrorCode::internal_failure,
                        "persona replacement failed internally", {}, false,
                        published);
  }
}

} // namespace aiforge::adapters
