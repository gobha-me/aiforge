#include <aiforge/adapters/filesystem_user_global_instruction_source.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>

#ifndef _WIN32
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

using EditorCode = instructions::UserGlobalInstructionEditorErrorCode;
using EditorError = instructions::UserGlobalInstructionEditorError;
using SourceCode = instructions::UserGlobalInstructionErrorCode;
using SourceError = instructions::UserGlobalInstructionError;
constexpr std::string target_filename{"global.md"};
constexpr std::string_view target_directory{"instructions"};

[[nodiscard]] auto edit_failure(
    const EditorCode code, std::string message,
    std::optional<domain::UserGlobalInstructionReference> observed =
        std::nullopt,
    const bool retryable = false, const bool may_have_applied = false)
    -> std::unexpected<EditorError> {
  return std::unexpected(EditorError{code, std::move(message),
                                     std::move(observed), retryable,
                                     may_have_applied});
}

[[nodiscard]] auto source_failure(const SourceCode code, std::string message,
                                  const bool retryable = false)
    -> std::unexpected<SourceError> {
  return std::unexpected(SourceError{code, std::move(message), retryable});
}

[[nodiscard]] auto source_error(EditorError error) -> SourceError {
  auto code = SourceCode::io_failure;
  switch (error.code) {
    case EditorCode::invalid_request: code = SourceCode::invalid_request; break;
    case EditorCode::malformed_text: code = SourceCode::malformed_text; break;
    case EditorCode::path_escape: code = SourceCode::path_escape; break;
    case EditorCode::unsupported_entry:
      code = SourceCode::unsupported_entry;
      break;
    case EditorCode::resource_exhausted:
      code = SourceCode::resource_exhausted;
      break;
    case EditorCode::permission_denied:
      code = SourceCode::permission_denied;
      break;
    case EditorCode::cancelled: code = SourceCode::cancelled; break;
    case EditorCode::concurrent_change: code = SourceCode::unstable; break;
    case EditorCode::already_exists:
    case EditorCode::not_found:
    case EditorCode::source_mismatch:
    case EditorCode::durability_failure:
    case EditorCode::io_failure: code = SourceCode::io_failure; break;
    case EditorCode::internal_failure:
      code = SourceCode::internal_failure;
      break;
  }
  return {code, std::move(error.message), error.retryable};
}

[[nodiscard]] auto valid_limits(
    const instructions::UserGlobalInstructionLimits& limits) -> bool {
  constexpr instructions::UserGlobalInstructionLimits maximums;
  return limits.maximum_file_bytes != 0 &&
         limits.maximum_file_bytes <= maximums.maximum_file_bytes;
}

[[nodiscard]] auto contains_embedded_null(const std::filesystem::path& path)
    -> bool {
  return path.native().find(std::filesystem::path::value_type{}) !=
         std::filesystem::path::string_type::npos;
}

[[nodiscard]] auto valid_path(const std::filesystem::path& path) -> bool {
  return !contains_embedded_null(path) && path.is_absolute() && !path.empty() &&
         path.lexically_normal() == path &&
         path.filename() == target_filename &&
         path.parent_path().filename() == target_directory;
}

[[nodiscard]] auto digest(const std::string_view text)
    -> domain::ContentDigest {
  detail::Sha256 sha256;
  sha256.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", sha256.finish(), text.size()};
}

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
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX openat API.
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

[[nodiscard]] auto same_file(const struct stat& left, const struct stat& right)
    -> bool {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

struct DirectoryRoot {
  struct PathIdentity {
    std::string basename;
    dev_t device{};
    ino_t inode{};
  };

  UniqueFd directory;
  std::vector<PathIdentity> path_identities;
};

// NOLINTBEGIN(readability-function-cognitive-complexity) -- Hostile path
// boundary.
[[nodiscard]] auto open_directory_root(const std::filesystem::path& path,
                                       const bool create)
    -> std::expected<std::optional<DirectoryRoot>, EditorError> {
  if (contains_embedded_null(path) || !path.is_absolute() || path.empty() ||
      path.lexically_normal() != path) {
    return edit_failure(EditorCode::invalid_request,
                        "global instruction directory path is invalid");
  }
  std::vector<std::string> components;
  for (const auto& part : path.relative_path()) {
    const auto component = part.string();
    if (component.empty() || component.find('\0') != std::string::npos ||
        component == "." || component == "..") {
      return edit_failure(EditorCode::invalid_request,
                          "global instruction path has an invalid component");
    }
    components.push_back(component);
  }
  if (components.size() < 2) {
    return edit_failure(EditorCode::invalid_request,
                        "global instruction directory path is invalid");
  }

  UniqueFd current{
      open_existing("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  if (!current) {
    return edit_failure(EditorCode::io_failure,
                        "filesystem root could not be opened", {}, true);
  }
  std::vector<DirectoryRoot::PathIdentity> path_identities;
  path_identities.reserve(components.size());
  for (std::size_t index{}; index < components.size(); ++index) {
    const auto& component = components[index];
    struct stat path_state{};
    bool created{};
    if (::fstatat(current.get(), component.c_str(), &path_state,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT && !create) return std::nullopt;
      if (errno != ENOENT) {
        return edit_failure(
            errno == EACCES ? EditorCode::permission_denied
                            : EditorCode::io_failure,
            "global instruction directory could not be inspected", {}, true);
      }
      if (::mkdirat(current.get(), component.c_str(), 0700) == 0) {
        created = true;
      } else if (errno != EEXIST) {
        return edit_failure(errno == EACCES ? EditorCode::permission_denied
                                            : EditorCode::io_failure,
                            "global instruction directory could not be created",
                            {}, true);
      }
      if (::fstatat(current.get(), component.c_str(), &path_state,
                    AT_SYMLINK_NOFOLLOW) != 0) {
        return edit_failure(
            EditorCode::io_failure,
            "created global instruction directory could not be inspected", {},
            true);
      }
    }
    if (S_ISLNK(path_state.st_mode)) {
      return edit_failure(EditorCode::path_escape,
                          "global instruction path cannot traverse a symlink");
    }
    if (!S_ISDIR(path_state.st_mode)) {
      return edit_failure(
          EditorCode::unsupported_entry,
          "global instruction path component is not a directory");
    }
    UniqueFd next{
        open_existing_at(current.get(), component.c_str(),
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    struct stat opened{};
    if (!next || ::fstat(next.get(), &opened) != 0) {
      return edit_failure(
          errno == ELOOP || errno == ENOTDIR ? EditorCode::path_escape
          : errno == EACCES                  ? EditorCode::permission_denied
                                             : EditorCode::io_failure,
          "global instruction path component could not be opened", {}, true);
    }
    if (!S_ISDIR(opened.st_mode) || opened.st_dev != path_state.st_dev ||
        opened.st_ino != path_state.st_ino) {
      return edit_failure(EditorCode::concurrent_change,
                          "global instruction directory changed while opening",
                          {}, true);
    }
    if (created && ::fchmod(next.get(), 0700) != 0) {
      return edit_failure(EditorCode::permission_denied,
                          "global instruction directory could not be secured");
    }
    if (created && (::fsync(next.get()) != 0 || ::fsync(current.get()) != 0)) {
      return edit_failure(
          EditorCode::durability_failure,
          "global instruction directory could not be synchronized", {}, true);
    }
    if (index + 2U >= components.size() &&
        (opened.st_uid != ::geteuid() || (opened.st_mode & 0077) != 0)) {
      return edit_failure(
          EditorCode::permission_denied,
          "global instruction directories must be private and user-owned");
    }
    path_identities.push_back({component, opened.st_dev, opened.st_ino});
    if (index + 1U == components.size()) {
      return std::optional<DirectoryRoot>{
          DirectoryRoot{std::move(next), std::move(path_identities)}};
    }
    current = std::move(next);
  }
  return edit_failure(EditorCode::internal_failure,
                      "global instruction directory could not be resolved");
}
// NOLINTEND(readability-function-cognitive-complexity)

[[nodiscard]] auto root_unchanged(
    const DirectoryRoot& root,
    const UserGlobalInstructionFilesystemCheckpoint* checkpoint = nullptr)
    -> std::expected<void, EditorError> {
  UniqueFd current{
      open_existing("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  if (!current) {
    return edit_failure(EditorCode::io_failure,
                        "filesystem root could not be reopened", {}, true);
  }
  for (std::size_t index{}; index < root.path_identities.size(); ++index) {
    const auto& expected = root.path_identities[index];
    struct stat state{};
    if (::fstatat(current.get(), expected.basename.c_str(), &state,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(state.st_mode) || S_ISLNK(state.st_mode) ||
        state.st_dev != expected.device || state.st_ino != expected.inode ||
        (index + 2U >= root.path_identities.size() &&
         (state.st_uid != ::geteuid() || (state.st_mode & 0077) != 0))) {
      return edit_failure(EditorCode::concurrent_change,
                          "global instruction directory changed", {}, true);
    }
    if (checkpoint != nullptr && index + 1U == root.path_identities.size()) {
      auto reached =
          (*checkpoint)(UserGlobalInstructionFilesystemCheckpointStage::
                            root_revalidation_ready);
      if (!reached) return std::unexpected(std::move(reached.error()));
    }
    UniqueFd next{
        open_existing_at(current.get(), expected.basename.c_str(),
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    struct stat opened{};
    if (!next || ::fstat(next.get(), &opened) != 0 ||
        !S_ISDIR(opened.st_mode) || opened.st_dev != state.st_dev ||
        opened.st_ino != state.st_ino || opened.st_dev != expected.device ||
        opened.st_ino != expected.inode ||
        (index + 2U >= root.path_identities.size() &&
         (opened.st_uid != ::geteuid() || (opened.st_mode & 0077) != 0))) {
      return edit_failure(EditorCode::concurrent_change,
                          "global instruction directory changed", {}, true);
    }
    current = std::move(next);
  }
  return {};
}

// Stable hostile-file reads keep every identity, type, size, and text gate
// explicit.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto load_at(
    const DirectoryRoot& root, const std::string& filename,
    const instructions::UserGlobalInstructionLimits& limits,
    const std::stop_token stop_token)
    -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                     EditorError> {
  struct stat listed{};
  if (::fstatat(root.directory.get(), filename.c_str(), &listed,
                AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) return std::nullopt;
    return edit_failure(EditorCode::io_failure,
                        "global instruction file could not be inspected", {},
                        true);
  }
  if (S_ISLNK(listed.st_mode)) {
    return edit_failure(EditorCode::path_escape,
                        "global instruction file cannot be a symlink");
  }
  if (!S_ISREG(listed.st_mode) || listed.st_nlink != 1) {
    return edit_failure(EditorCode::unsupported_entry,
                        "global instruction entry must be one regular file");
  }
  if (listed.st_uid != ::geteuid() || (listed.st_mode & 0077) != 0) {
    return edit_failure(
        EditorCode::permission_denied,
        "global instruction file must be private and user-owned");
  }
  if (listed.st_size <= 0 ||
      std::cmp_greater(listed.st_size, limits.maximum_file_bytes)) {
    return edit_failure(listed.st_size <= 0 ? EditorCode::malformed_text
                                            : EditorCode::resource_exhausted,
                        listed.st_size <= 0
                            ? "global instruction file is empty"
                            : "global instruction file exceeds its byte limit");
  }
  UniqueFd descriptor{
      open_existing_at(root.directory.get(), filename.c_str(),
                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (!descriptor) {
    if (errno == ENOENT) return std::nullopt;
    return edit_failure(errno == ELOOP    ? EditorCode::path_escape
                        : errno == EACCES ? EditorCode::permission_denied
                                          : EditorCode::io_failure,
                        "global instruction file could not be opened", {},
                        true);
  }
  struct stat before{};
  if (::fstat(descriptor.get(), &before) != 0) {
    return edit_failure(EditorCode::io_failure,
                        "global instruction file could not be inspected", {},
                        true);
  }
  if (!same_file(listed, before) || !S_ISREG(before.st_mode) ||
      before.st_nlink != 1 || before.st_uid != ::geteuid() ||
      (before.st_mode & 0077) != 0) {
    return edit_failure(EditorCode::concurrent_change,
                        "global instruction file changed while opening", {},
                        true);
  }
  std::string text;
  text.reserve(static_cast<std::size_t>(before.st_size));
  std::array<char, 8192> buffer{};
  for (;;) {
    if (stop_token.stop_requested()) {
      return edit_failure(EditorCode::cancelled,
                          "global instruction loading was cancelled");
    }
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return edit_failure(EditorCode::io_failure,
                          "global instruction file could not be read", {},
                          true);
    }
    const auto bytes = static_cast<std::size_t>(count);
    if (bytes > limits.maximum_file_bytes - text.size()) {
      return edit_failure(EditorCode::resource_exhausted,
                          "global instruction file exceeds its byte limit");
    }
    text.append(buffer.data(), bytes);
  }
  struct stat after{};
  struct stat path_state{};
  if (::fstat(descriptor.get(), &after) != 0 ||
      ::fstatat(root.directory.get(), filename.c_str(), &path_state,
                AT_SYMLINK_NOFOLLOW) != 0) {
    return edit_failure(EditorCode::io_failure,
                        "global instruction file could not be verified", {},
                        true);
  }
  if (!same_file(before, after) || after.st_dev != path_state.st_dev ||
      after.st_ino != path_state.st_ino || S_ISLNK(path_state.st_mode) ||
      !std::cmp_equal(after.st_size, text.size())) {
    return edit_failure(EditorCode::concurrent_change,
                        "global instruction file changed while reading", {},
                        true);
  }
  if (!detail::is_safe_utf8_text(text)) {
    return edit_failure(
        EditorCode::malformed_text,
        "global instruction file must be safe nonempty UTF-8 text");
  }
  auto source_id = domain::ContextSourceId::from(
      std::string{domain::user_global_instruction_source_identity});
  if (!source_id) {
    return edit_failure(EditorCode::internal_failure,
                        "global instruction identity is invalid");
  }
  domain::UserGlobalInstructionDocument document{
      {std::move(*source_id),
       std::string{domain::user_global_instruction_source_location},
       digest(text)},
      std::move(text)};
  if (!domain::validate_user_global_instruction_document(document)) {
    return edit_failure(EditorCode::internal_failure,
                        "global instruction document is inconsistent");
  }
  return std::optional<domain::UserGlobalInstructionDocument>{
      std::move(document)};
}

[[nodiscard]] auto acquire_lock(const int directory,
                                const std::stop_token stop_token)
    -> std::expected<UniqueFd, EditorError> {
  const std::string lock_name{".aiforge-global-instructions.lock"};
  UniqueFd lock{create_at(directory, lock_name.c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (!lock) {
    return edit_failure(errno == ELOOP    ? EditorCode::path_escape
                        : errno == EACCES ? EditorCode::permission_denied
                                          : EditorCode::io_failure,
                        "global instruction lock could not be opened", {},
                        true);
  }
  struct stat state{};
  if (::fstat(lock.get(), &state) != 0 || !S_ISREG(state.st_mode) ||
      state.st_uid != ::geteuid() || state.st_nlink != 1 ||
      ::fchmod(lock.get(), 0600) != 0) {
    return edit_failure(EditorCode::permission_denied,
                        "global instruction lock has unsafe attributes");
  }
  while (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
      return edit_failure(EditorCode::io_failure,
                          "global instruction lock could not be acquired", {},
                          true);
    }
    if (stop_token.stop_requested()) {
      return edit_failure(EditorCode::cancelled,
                          "global instruction write was cancelled");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  return lock;
}

[[nodiscard]] auto write_all(const int descriptor, const std::string_view text,
                             const std::stop_token stop_token)
    -> std::expected<void, EditorError> {
  std::size_t offset{};
  while (offset < text.size()) {
    if (stop_token.stop_requested()) {
      return edit_failure(EditorCode::cancelled,
                          "global instruction write was cancelled");
    }
    const auto count =
        ::write(descriptor, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      return edit_failure(
          errno == ENOSPC ? EditorCode::resource_exhausted
                          : EditorCode::io_failure,
          "global instruction temporary file could not be written", {}, true);
    }
    offset += static_cast<std::size_t>(count);
  }
  return {};
}

class PreparedFile final {
 public:
  // Secure temporary publication keeps collision, file identity, write, sync,
  // and size gates explicit.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] static auto create(const int directory,
                                   const std::string_view text,
                                   const std::stop_token stop_token)
      -> std::expected<PreparedFile, EditorError> {
    static std::atomic_uint64_t sequence{};
    for (std::size_t attempt{}; attempt < 128; ++attempt) {
      auto name = ".aiforge-global-instruction-" + std::to_string(::getpid()) +
                  "-" + std::to_string(sequence.fetch_add(1));
      UniqueFd descriptor{create_at(
          directory, name.c_str(),
          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
      if (!descriptor && errno == EEXIST) continue;
      if (!descriptor) {
        return edit_failure(
            errno == EACCES ? EditorCode::permission_denied
                            : EditorCode::io_failure,
            "global instruction temporary file could not be created", {}, true);
      }
      struct stat state{};
      if (::fstat(descriptor.get(), &state) != 0 || !S_ISREG(state.st_mode) ||
          state.st_uid != ::geteuid() || state.st_nlink != 1 ||
          ::fchmod(descriptor.get(), 0600) != 0) {
        static_cast<void>(::unlinkat(directory, name.c_str(), 0));
        return edit_failure(EditorCode::permission_denied,
                            "global instruction temporary file is unsafe");
      }
      PreparedFile result{directory, std::move(name), std::move(descriptor)};
      if (auto written = write_all(result.m_descriptor.get(), text, stop_token);
          !written) {
        return std::unexpected(std::move(written.error()));
      }
      if (::fsync(result.m_descriptor.get()) != 0) {
        return edit_failure(
            errno == ENOSPC ? EditorCode::resource_exhausted
                            : EditorCode::io_failure,
            "global instruction temporary file could not be synchronized", {},
            true);
      }
      struct stat complete{};
      if (::fstat(result.m_descriptor.get(), &complete) != 0 ||
          complete.st_size < 0 ||
          static_cast<std::size_t>(complete.st_size) != text.size()) {
        return edit_failure(
            EditorCode::io_failure,
            "global instruction temporary file could not be verified", {},
            true);
      }
      return result;
    }
    return edit_failure(EditorCode::resource_exhausted,
                        "global instruction temporary name space is exhausted",
                        {}, true);
  }

  PreparedFile(PreparedFile&& other) noexcept
      : m_directory(other.m_directory), m_name(std::move(other.m_name)),
        m_descriptor(std::move(other.m_descriptor)) {
    other.m_name.clear();
  }
  PreparedFile(const PreparedFile&) = delete;
  auto operator=(const PreparedFile&) -> PreparedFile& = delete;
  ~PreparedFile() {
    if (!m_name.empty())
      static_cast<void>(::unlinkat(m_directory, m_name.c_str(), 0));
  }
  [[nodiscard]] auto name() const noexcept -> const std::string& {
    return m_name;
  }
  [[nodiscard]] auto path_is_prepared() const noexcept -> bool {
    struct stat descriptor_state{};
    struct stat path_state{};
    return !m_name.empty() &&
           ::fstat(m_descriptor.get(), &descriptor_state) == 0 &&
           ::fstatat(m_directory, m_name.c_str(), &path_state,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(path_state.st_mode) &&
           descriptor_state.st_dev == path_state.st_dev &&
           descriptor_state.st_ino == path_state.st_ino;
  }
  auto disarm() noexcept -> void { m_name.clear(); }

 private:
  PreparedFile(const int directory, std::string name, UniqueFd descriptor)
      : m_directory(directory), m_name(std::move(name)),
        m_descriptor(std::move(descriptor)) {}

  int m_directory;
  std::string m_name;
  UniqueFd m_descriptor;
};

[[nodiscard]] auto precondition(
    const std::optional<domain::UserGlobalInstructionDocument>& current,
    const std::optional<domain::UserGlobalInstructionReference>& expected,
    const bool final_check) -> std::expected<void, EditorError> {
  if (!expected) {
    if (!current) return {};
    return edit_failure(final_check ? EditorCode::concurrent_change
                                    : EditorCode::already_exists,
                        final_check
                            ? "global instruction appeared before publication"
                            : "global instruction already exists",
                        current->reference, final_check);
  }
  if (!current) {
    return edit_failure(
        final_check ? EditorCode::concurrent_change : EditorCode::not_found,
        final_check ? "global instruction disappeared before publication"
                    : "global instruction was not found",
        {}, final_check);
  }
  if (current->reference != *expected) {
    return edit_failure(final_check ? EditorCode::concurrent_change
                                    : EditorCode::source_mismatch,
                        final_check
                            ? "global instruction changed before publication"
                            : "global instruction precondition does not match",
                        current->reference, true);
  }
  return {};
}
#endif

} // namespace

auto resolve_user_global_instruction_path(
    const config::ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path,
                     instructions::UserGlobalInstructionError> {
  auto config_path = config::resolve_config_path(environment);
  if (!config_path) {
    return source_failure(config_path.error().code ==
                                  config::ConfigFileErrorCode::missing_home
                              ? SourceCode::missing_home
                              : SourceCode::invalid_root,
                          "global instruction path could not be resolved");
  }
  return config_path->parent_path() / target_directory / target_filename;
}

auto process_user_global_instruction_path()
    -> std::expected<std::filesystem::path,
                     instructions::UserGlobalInstructionError> {
  try {
    config::ConfigPathEnvironment environment;
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- Startup environment snapshot.
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"))
      environment.xdg_config_home = std::filesystem::path{xdg};
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- Startup environment snapshot.
    if (const auto* home = std::getenv("HOME"))
      environment.home = std::filesystem::path{home};
    return resolve_user_global_instruction_path(environment);
  } catch (...) {
    return source_failure(SourceCode::invalid_root,
                          "global instruction path could not be resolved");
  }
}

auto FilesystemUserGlobalInstructionSource::load(
    const instructions::UserGlobalInstructionLimits limits,
    const std::stop_token stop_token)
    -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                     instructions::UserGlobalInstructionError> {
  try {
    if (!valid_limits(limits) || !valid_path(m_path)) {
      return source_failure(
          SourceCode::invalid_request,
          "global instruction source configuration is invalid");
    }
    if (stop_token.stop_requested()) {
      return source_failure(SourceCode::cancelled,
                            "global instruction loading was cancelled");
    }
#ifdef _WIN32
    static_cast<void>(stop_token);
    return source_failure(SourceCode::io_failure,
                          "filesystem global instructions are unavailable");
#else
    auto root = open_directory_root(m_path.parent_path(), false);
    if (!root) return std::unexpected(source_error(std::move(root.error())));
    if (!*root) return std::optional<domain::UserGlobalInstructionDocument>{};
    auto document =
        load_at(**root, std::string{target_filename}, limits, stop_token);
    if (!document)
      return std::unexpected(source_error(std::move(document.error())));
    if (auto stable = root_unchanged(**root); !stable) {
      return std::unexpected(source_error(std::move(stable.error())));
    }
    return std::move(*document);
#endif
  } catch (...) {
    return source_failure(SourceCode::internal_failure,
                          "global instruction loading failed internally");
  }
}

// NOLINTBEGIN(readability-function-cognitive-complexity) -- Exact atomic
// publication and rollback.
auto FilesystemUserGlobalInstructionSource::write(
    instructions::UserGlobalInstructionWrite request,
    const std::stop_token stop_token)
    -> std::expected<instructions::UserGlobalInstructionWriteReceipt,
                     instructions::UserGlobalInstructionEditorError> {
  bool published{};
  try {
    auto candidate =
        instructions::prepare_user_global_instruction_write(request);
    if (!candidate) return std::unexpected(std::move(candidate.error()));
    if (!valid_path(m_path)) {
      return edit_failure(EditorCode::invalid_request,
                          "global instruction destination is invalid");
    }
    if (stop_token.stop_requested()) {
      return edit_failure(EditorCode::cancelled,
                          "global instruction write was cancelled");
    }
#ifdef _WIN32
    return edit_failure(EditorCode::io_failure,
                        "filesystem global instruction writes are unavailable");
#else
    auto opened = open_directory_root(m_path.parent_path(), true);
    if (!opened) return std::unexpected(std::move(opened.error()));
    if (!*opened) {
      return edit_failure(EditorCode::internal_failure,
                          "global instruction directory was not created");
    }
    auto root = std::move(**opened);
    auto lock = acquire_lock(root.directory.get(), stop_token);
    if (!lock) return std::unexpected(std::move(lock.error()));
    auto current =
        load_at(root, std::string{target_filename}, request.limits, stop_token);
    if (!current) return std::unexpected(std::move(current.error()));
    if (auto matched = precondition(*current, request.expected, false);
        !matched) {
      return std::unexpected(std::move(matched.error()));
    }
    auto prepared =
        PreparedFile::create(root.directory.get(), request.text, stop_token);
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    if (m_checkpoint) {
      auto checkpoint = m_checkpoint(
          UserGlobalInstructionFilesystemCheckpointStage::temporary_synced);
      if (!checkpoint) return std::unexpected(std::move(checkpoint.error()));
    }
    current =
        load_at(root, std::string{target_filename}, request.limits, stop_token);
    if (!current) return std::unexpected(std::move(current.error()));
    if (auto matched = precondition(*current, request.expected, true); !matched)
      return std::unexpected(std::move(matched.error()));
    if (auto stable = root_unchanged(root); !stable)
      return std::unexpected(std::move(stable.error()));
    if (stop_token.stop_requested()) {
      return edit_failure(
          EditorCode::cancelled,
          "global instruction write was cancelled before publication");
    }

    if (!request.expected) {
      if (::linkat(root.directory.get(), prepared->name().c_str(),
                   root.directory.get(), target_filename.c_str(), 0) != 0) {
        return edit_failure(
            errno == EEXIST   ? EditorCode::already_exists
            : errno == EACCES ? EditorCode::permission_denied
                              : EditorCode::io_failure,
            errno == EEXIST ? "global instruction collided during publication"
                            : "global instruction could not be published",
            {}, errno != EEXIST);
      }
      published = true;
    } else {
      if (m_checkpoint) {
        auto checkpoint = m_checkpoint(
            UserGlobalInstructionFilesystemCheckpointStage::replacement_ready);
        if (!checkpoint) return std::unexpected(std::move(checkpoint.error()));
      }
      if (exchange_entries(root.directory.get(), prepared->name(),
                           std::string{target_filename}) != 0) {
        return edit_failure(
            errno == EACCES ? EditorCode::permission_denied
                            : EditorCode::io_failure,
            errno == ENOTSUP || errno == ENOSYS
                ? "exact global instruction replacement is unavailable"
                : "global instruction replacement could not be published",
            {}, true);
      }
      published = true;
      auto displaced = load_at(root, prepared->name(), request.limits, {});
      const bool displaced_matches =
          displaced && *displaced &&
          (*displaced)->reference == *request.expected;
      if (!displaced_matches) {
        std::optional<domain::UserGlobalInstructionReference> observed;
        if (displaced && *displaced) observed = (*displaced)->reference;
        std::optional<EditorError> checkpoint_error;
        if (m_checkpoint) {
          auto checkpoint = m_checkpoint(
              UserGlobalInstructionFilesystemCheckpointStage::rollback_ready);
          if (!checkpoint) {
            checkpoint_error = std::move(checkpoint.error());
            checkpoint_error->may_have_applied = true;
          }
        }
        if (exchange_entries(root.directory.get(), prepared->name(),
                             std::string{target_filename}) != 0) {
          prepared->disarm();
          return edit_failure(EditorCode::concurrent_change,
                              "global instruction changed during publication "
                              "and rollback failed",
                              std::move(observed), true, true);
        }
        if (!prepared->path_is_prepared()) {
          prepared->disarm();
          return edit_failure(
              EditorCode::concurrent_change,
              "another global instruction change raced with rollback",
              std::move(observed), true, true);
        }
        if (::unlinkat(root.directory.get(), prepared->name().c_str(), 0) !=
            0) {
          return edit_failure(EditorCode::io_failure,
                              "global instruction rollback cleanup failed",
                              std::move(observed), true, true);
        }
        prepared->disarm();
        if (::fsync(root.directory.get()) != 0) {
          return edit_failure(
              EditorCode::durability_failure,
              "global instruction rollback could not be synchronized",
              std::move(observed), true, true);
        }
        published = false;
        if (checkpoint_error)
          return std::unexpected(std::move(*checkpoint_error));
        return edit_failure(
            EditorCode::concurrent_change,
            "global instruction changed during exact replacement",
            std::move(observed), true);
      }
    }

    std::optional<EditorError> post_error;
    if (::unlinkat(root.directory.get(), prepared->name().c_str(), 0) != 0) {
      post_error =
          EditorError{EditorCode::io_failure,
                      "global instruction temporary entry could not be removed",
                      {},
                      true,
                      true};
    } else {
      prepared->disarm();
    }
    if (m_checkpoint) {
      auto checkpoint = m_checkpoint(
          UserGlobalInstructionFilesystemCheckpointStage::published);
      if (!checkpoint && !post_error) {
        post_error = std::move(checkpoint.error());
        post_error->may_have_applied = true;
      }
    }
    if (::fsync(root.directory.get()) != 0 && !post_error) {
      post_error =
          EditorError{EditorCode::durability_failure,
                      "global instruction directory could not be synchronized",
                      {},
                      true,
                      true};
    }
    if (post_error) return std::unexpected(std::move(*post_error));
    auto resulting =
        load_at(root, std::string{target_filename}, request.limits, {});
    if (!resulting) {
      auto error = std::move(resulting.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    if (!*resulting || (*resulting)->reference != candidate->reference) {
      return edit_failure(
          EditorCode::concurrent_change,
          "published global instruction did not match its candidate",
          *resulting ? std::optional{(*resulting)->reference} : std::nullopt,
          true, true);
    }
    if (auto stable =
            root_unchanged(root, m_checkpoint ? &m_checkpoint : nullptr);
        !stable) {
      auto error = std::move(stable.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    instructions::UserGlobalInstructionWriteReceipt receipt{
        request.expected, (*resulting)->reference};
    if (auto valid =
            instructions::validate_user_global_instruction_write_receipt(
                request, receipt);
        !valid) {
      auto error = std::move(valid.error());
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    return receipt;
#endif
  } catch (...) {
    return edit_failure(EditorCode::internal_failure,
                        "global instruction write failed internally", {}, false,
                        published);
  }
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace aiforge::adapters
