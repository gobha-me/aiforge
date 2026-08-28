#include <aiforge/adapters/process_draft_editor.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

using Error = surfaces::DraftEditorError;
using Code = surfaces::DraftEditorErrorCode;

[[nodiscard]] auto error(const Code code, std::string message)
    -> std::unexpected<Error> {
  return std::unexpected(Error{code, std::move(message)});
}

[[nodiscard]] auto valid_token(const std::string_view value) -> bool {
  return !value.empty() && value.size() <= 4096 &&
         std::ranges::none_of(value, [](const unsigned char character) {
           return std::isspace(character) != 0 || character < 0x20U ||
                  character == 0x7FU;
         });
}

#ifndef _WIN32
[[nodiscard]] auto canonical_executable(const std::filesystem::path& path)
    -> std::optional<std::string> {
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(path, ec);
  if (ec || !canonical.is_absolute()) return std::nullopt;
  const auto status = std::filesystem::status(canonical, ec);
  if (ec || !std::filesystem::is_regular_file(status) ||
      ::access(canonical.c_str(), X_OK) != 0) {
    return std::nullopt;
  }
  return canonical.string();
}

[[nodiscard]] auto resolve_editor() -> std::expected<std::string, Error> {
  const char* raw = std::getenv("VISUAL");
  if (raw == nullptr || *raw == '\0') raw = std::getenv("EDITOR");
  if (raw == nullptr || *raw == '\0') {
    return error(Code::not_configured,
                 "set VISUAL or EDITOR to one editor executable");
  }
  const std::string value{raw};
  if (!valid_token(value)) {
    return error(Code::invalid_configuration,
                 "VISUAL or EDITOR must name one executable without arguments");
  }
  const std::filesystem::path path{value};
  if (path.is_absolute()) {
    auto resolved = canonical_executable(path);
    if (!resolved) {
      return error(Code::unavailable,
                   "configured editor executable is unavailable");
    }
    return *resolved;
  }
  if (path.has_parent_path()) {
    return error(Code::invalid_configuration,
                 "relative editor paths are not allowed");
  }

  const char* raw_path = std::getenv("PATH");
  if (raw_path == nullptr) {
    return error(Code::unavailable,
                 "configured editor executable could not be resolved");
  }
  const std::string search{raw_path};
  std::size_t start{};
  while (start <= search.size()) {
    const auto end = search.find(':', start);
    const auto part = std::string_view{search}.substr(
        start, end == std::string::npos ? end : end - start);
    const std::filesystem::path directory{part};
    if (!part.empty() && directory.is_absolute()) {
      if (auto resolved = canonical_executable(directory / path)) {
        return *resolved;
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return error(Code::unavailable,
               "configured editor executable could not be resolved");
}

auto write_all(const int descriptor, const std::string_view value) -> bool {
  std::size_t offset{};
  while (offset < value.size()) {
    const auto count =
        ::write(descriptor, value.data() + offset, value.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

[[nodiscard]] auto normalized_text(std::string value)
    -> std::optional<std::string> {
  std::string normalized;
  normalized.reserve(value.size());
  for (std::size_t index{}; index < value.size(); ++index) {
    if (value[index] == '\r') {
      if (index + 1 < value.size() && value[index + 1] == '\n') ++index;
      normalized.push_back('\n');
    } else {
      normalized.push_back(value[index]);
    }
  }

  std::size_t index{};
  while (index < normalized.size()) {
    const auto first = static_cast<unsigned char>(normalized[index]);
    if (first == 0 || (first < 0x20U && first != '\n' && first != '\t') ||
        first == 0x7FU) {
      return std::nullopt;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return std::nullopt;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return std::nullopt;
    }
    if (length > normalized.size() - index) return std::nullopt;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(normalized[index + offset]);
      if ((next & 0xC0U) != 0x80U) return std::nullopt;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return std::nullopt;
    }
    index += length;
  }
  return normalized;
}

[[nodiscard]] auto selected_environment() -> std::vector<std::string> {
  static constexpr std::array names{
      "HOME",          "USER",   "LOGNAME",         "TERM",
      "COLORTERM",     "LANG",   "LC_ALL",          "LC_CTYPE",
      "PATH",          "TMPDIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
      "XDG_STATE_HOME"};
  std::vector<std::string> result;
  for (const auto* name : names) {
    if (const char* value = std::getenv(name); value != nullptr) {
      result.push_back(std::string{name} + '=' + value);
    }
  }
  return result;
}

struct TemporaryDraft {
  std::filesystem::path directory;
  std::filesystem::path file;
};

[[nodiscard]] auto secure_runtime_directory(const char* value)
    -> std::optional<std::filesystem::path> {
  if (value == nullptr) return std::nullopt;
  const std::filesystem::path candidate{value};
  if (!candidate.is_absolute()) return std::nullopt;
  struct stat status{};
  if (::lstat(candidate.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != ::getuid() ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return std::nullopt;
  }
  return candidate;
}

[[nodiscard]] auto make_temporary(const std::string_view draft)
    -> std::expected<TemporaryDraft, Error> {
  auto base = std::filesystem::path{"/tmp"};
  if (auto runtime = secure_runtime_directory(std::getenv("XDG_RUNTIME_DIR")))
    base = std::move(*runtime);
  auto pattern = (base / "aiforge-editor-XXXXXX").string();
  std::vector<char> storage(pattern.begin(), pattern.end());
  storage.push_back('\0');
  char* made = ::mkdtemp(storage.data());
  if (made == nullptr) {
    return error(errno == EACCES ? Code::permission_denied
                                 : Code::resource_exhausted,
                 "secure editor temporary directory could not be created");
  }
  TemporaryDraft result{std::filesystem::path{made}, {}};
  if (::chmod(result.directory.c_str(), S_IRWXU) != 0) {
    static_cast<void>(::rmdir(result.directory.c_str()));
    return error(Code::permission_denied,
                 "editor temporary directory could not be secured");
  }
  result.file = result.directory / "draft.txt";
  const int descriptor =
      ::open(result.file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
             S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    static_cast<void>(::rmdir(result.directory.c_str()));
    return error(Code::permission_denied,
                 "editor draft file could not be created");
  }
  const bool written = write_all(descriptor, draft) && ::fsync(descriptor) == 0;
  const bool closed = ::close(descriptor) == 0;
  if (!written || !closed) {
    static_cast<void>(::unlink(result.file.c_str()));
    static_cast<void>(::rmdir(result.directory.c_str()));
    return error(Code::resource_exhausted,
                 "editor draft file could not be written");
  }
  return result;
}

auto cleanup(const TemporaryDraft& temporary) -> bool {
  const bool file_removed = ::unlink(temporary.file.c_str()) == 0;
  const bool directory_removed = ::rmdir(temporary.directory.c_str()) == 0;
  return file_removed && directory_removed;
}

[[nodiscard]] auto read_result(const TemporaryDraft& temporary,
                               const std::size_t maximum)
    -> std::expected<std::string, Error> {
  const int descriptor = ::open(temporary.file.c_str(), O_RDONLY | O_NOFOLLOW);
  if (descriptor < 0) {
    return error(Code::invalid_result, "edited draft could not be opened");
  }
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != ::getuid() ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximum) {
    static_cast<void>(::close(descriptor));
    return error(Code::invalid_result,
                 "edited draft is insecure or exceeds its size limit");
  }
  std::string value;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      static_cast<void>(::close(descriptor));
      return error(Code::invalid_result, "edited draft could not be read");
    }
    if (count == 0) break;
    const auto size = static_cast<std::size_t>(count);
    if (size > maximum - std::min(maximum, value.size())) {
      static_cast<void>(::close(descriptor));
      return error(Code::invalid_result, "edited draft exceeds its size limit");
    }
    value.append(buffer.data(), size);
  }
  if (::close(descriptor) != 0) {
    return error(Code::invalid_result, "edited draft could not be closed");
  }
  auto normalized = normalized_text(std::move(value));
  if (!normalized) {
    return error(Code::invalid_result,
                 "edited draft is not valid bounded UTF-8 text");
  }
  return std::move(*normalized);
}

[[nodiscard]] auto run_editor(const std::string& executable,
                              const TemporaryDraft& temporary,
                              const ProcessDraftEditorLimits& limits,
                              const std::stop_token stop_token)
    -> std::expected<void, Error> {
  auto environment = selected_environment();
  std::vector<char*> environment_pointers;
  environment_pointers.reserve(environment.size() + 1);
  for (auto& value : environment)
    environment_pointers.push_back(value.data());
  environment_pointers.push_back(nullptr);
  std::array<char*, 3> arguments{const_cast<char*>(executable.c_str()),
                                 const_cast<char*>(temporary.file.c_str()),
                                 nullptr};

  const pid_t child = ::fork();
  if (child < 0) {
    return error(Code::process_failed, "editor process could not be started");
  }
  if (child == 0) {
    static_cast<void>(::setpgid(0, 0));
    ::execve(executable.c_str(), arguments.data(), environment_pointers.data());
    ::_exit(127);
  }
  static_cast<void>(::setpgid(child, child));

  const bool terminal = ::isatty(STDIN_FILENO) != 0;
  const pid_t parent_group = terminal ? ::tcgetpgrp(STDIN_FILENO) : -1;
  struct sigaction ignored{};
  struct sigaction prior{};
  ignored.sa_handler = SIG_IGN;
  static_cast<void>(::sigemptyset(&ignored.sa_mask));
  if (terminal) {
    static_cast<void>(::sigaction(SIGTTOU, &ignored, &prior));
    static_cast<void>(::tcsetpgrp(STDIN_FILENO, child));
  }

  int status{};
  const auto started = std::chrono::steady_clock::now();
  bool cancelled{};
  bool timed_out{};
  for (;;) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) {
      static_cast<void>(::kill(-child, SIGKILL));
      static_cast<void>(::waitpid(child, &status, 0));
      if (terminal) {
        static_cast<void>(::tcsetpgrp(STDIN_FILENO, parent_group));
        static_cast<void>(::sigaction(SIGTTOU, &prior, nullptr));
      }
      return error(Code::process_failed,
                   "editor process could not be observed");
    }
    cancelled = stop_token.stop_requested();
    timed_out = std::chrono::steady_clock::now() - started >= limits.timeout;
    if (cancelled || timed_out) {
      static_cast<void>(::kill(-child, SIGTERM));
      const auto deadline =
          std::chrono::steady_clock::now() + limits.termination_grace;
      while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(child, &status, WNOHANG) == child) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
      }
      if (::waitpid(child, &status, WNOHANG) == 0) {
        static_cast<void>(::kill(-child, SIGKILL));
        static_cast<void>(::waitpid(child, &status, 0));
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  if (terminal) {
    static_cast<void>(::tcsetpgrp(STDIN_FILENO, parent_group));
    static_cast<void>(::sigaction(SIGTTOU, &prior, nullptr));
  }
  if (cancelled) return error(Code::cancelled, "editor cancelled");
  if (timed_out) return error(Code::process_failed, "editor timed out");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return error(Code::process_failed, "editor exited unsuccessfully");
  }
  return {};
}
#endif

} // namespace

auto ProcessDraftEditor::edit(const std::string_view draft,
                              const std::stop_token stop_token)
    -> std::expected<std::string, surfaces::DraftEditorError> {
  try {
    if (m_limits.maximum_draft_bytes == 0 ||
        m_limits.timeout <= std::chrono::milliseconds::zero() ||
        m_limits.termination_grace <= std::chrono::milliseconds::zero()) {
      return error(Code::internal_failure, "editor limits are invalid");
    }
    if (draft.size() > m_limits.maximum_draft_bytes) {
      return error(Code::resource_exhausted,
                   "draft exceeds the editor size limit");
    }
    if (stop_token.stop_requested()) {
      return error(Code::cancelled, "editor cancelled");
    }
#ifdef _WIN32
    static_cast<void>(draft);
    return error(Code::unavailable,
                 "external editor escape is unavailable on this platform");
#else
    auto executable = resolve_editor();
    if (!executable) return std::unexpected(std::move(executable.error()));
    auto temporary = make_temporary(draft);
    if (!temporary) return std::unexpected(std::move(temporary.error()));
    auto launched = run_editor(*executable, *temporary, m_limits, stop_token);
    if (!launched) {
      static_cast<void>(cleanup(*temporary));
      return std::unexpected(std::move(launched.error()));
    }
    auto result = read_result(*temporary, m_limits.maximum_draft_bytes);
    if (!result) {
      static_cast<void>(cleanup(*temporary));
      return std::unexpected(std::move(result.error()));
    }
    if (!cleanup(*temporary)) {
      return error(Code::cleanup_failed,
                   "editor temporary files could not be removed");
    }
    return std::move(*result);
#endif
  } catch (...) {
    return error(Code::internal_failure, "editor escape failed internally");
  }
}

} // namespace aiforge::adapters
