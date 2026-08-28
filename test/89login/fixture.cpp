#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-login-XXXXXX")
            .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto* created = ::mkdtemp(writable.data());
    if (created != nullptr) m_path = created;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

struct Child {
  pid_t pid{-1};
  int master{-1};
  int terminal{-1};
  std::string output;
};

enum class ChildMode {
  login,
  offline_chat,
};

auto close_child(Child& child) -> void {
  if (child.pid > 0) {
    int status{};
    if (::waitpid(child.pid, &status, WNOHANG) == 0) {
      static_cast<void>(::kill(child.pid, SIGKILL));
      static_cast<void>(::waitpid(child.pid, &status, 0));
    }
  }
  if (child.master >= 0) static_cast<void>(::close(child.master));
  if (child.terminal >= 0) static_cast<void>(::close(child.terminal));
  child.pid = -1;
  child.master = -1;
  child.terminal = -1;
}

[[nodiscard]] auto visible_terminal_text(const std::string_view output)
    -> std::string {
  enum class State { text, escape, control_sequence, string, string_escape };
  auto state = State::text;
  std::string visible;
  visible.reserve(output.size());
  for (const auto byte : output) {
    const auto character = static_cast<unsigned char>(byte);
    switch (state) {
      case State::text:
        if (character == 0x1b) {
          state = State::escape;
        } else if (character >= 0x20 && character != 0x7f) {
          visible.push_back(byte);
        }
        break;
      case State::escape:
        if (byte == '[') {
          state = State::control_sequence;
        } else if (byte == ']' || byte == 'P' || byte == '^' || byte == '_' ||
                   byte == 'X') {
          state = State::string;
        } else {
          state = State::text;
        }
        break;
      case State::control_sequence:
        if (character >= 0x40 && character <= 0x7e) state = State::text;
        break;
      case State::string:
        if (character == 0x07) {
          state = State::text;
        } else if (character == 0x1b) {
          state = State::string_escape;
        }
        break;
      case State::string_escape:
        state = byte == '\\' ? State::text : State::string;
        break;
    }
  }
  return visible;
}

[[nodiscard]] auto spawn(const std::string& executable,
                         const std::filesystem::path& config_home,
                         const ChildMode mode = ChildMode::login) -> Child {
  int master{-1};
  int slave{-1};
  if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return {};
  const auto pid = ::fork();
  if (pid < 0) {
    static_cast<void>(::close(master));
    static_cast<void>(::close(slave));
    return {};
  }
  if (pid == 0) {
    static_cast<void>(::setsid());
    static_cast<void>(::dup2(slave, STDIN_FILENO));
    static_cast<void>(::dup2(slave, STDOUT_FILENO));
    static_cast<void>(::dup2(slave, STDERR_FILENO));
    static_cast<void>(::close(master));
    if (slave > STDERR_FILENO) static_cast<void>(::close(slave));
    static_cast<void>(::setenv("XDG_CONFIG_HOME", config_home.c_str(), 1));
    static_cast<void>(::unsetenv("VENICE_API_KEY"));
    if (mode == ChildMode::offline_chat) {
      static_cast<void>(::setenv("AIFORGE_MODEL", "offline-model", 1));
      const auto cache_home = config_home / "cache";
      static_cast<void>(::setenv("XDG_CACHE_HOME", cache_home.c_str(), 1));
      ::execl(executable.c_str(), executable.c_str(), "--ephemeral", nullptr);
    } else {
      ::execl(executable.c_str(), executable.c_str(), "login", nullptr);
    }
    ::_exit(127);
  }
  const auto flags = ::fcntl(master, F_GETFL, 0);
  if (flags >= 0)
    static_cast<void>(::fcntl(master, F_SETFL, flags | O_NONBLOCK));
  return {pid, master, slave, {}};
}

[[nodiscard]] auto read_until(Child& child, const std::string_view marker,
                              const std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{child.master, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, 50);
    if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
      char buffer[512];
      const auto count = ::read(child.master, buffer, sizeof(buffer));
      if (count > 0)
        child.output.append(buffer, static_cast<std::size_t>(count));
    }
    if (visible_terminal_text(child.output).find(marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto finish(Child& child, const std::chrono::milliseconds timeout)
    -> int {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status{};
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{child.master, POLLIN, 0};
    if (::poll(&descriptor, 1, 20) > 0 && (descriptor.revents & POLLIN) != 0) {
      char buffer[512];
      const auto count = ::read(child.master, buffer, sizeof(buffer));
      if (count > 0)
        child.output.append(buffer, static_cast<std::size_t>(count));
    }
    const auto waited = ::waitpid(child.pid, &status, WNOHANG);
    if (waited == child.pid) {
      while (true) {
        char buffer[512];
        const auto count = ::read(child.master, buffer, sizeof(buffer));
        if (count <= 0) break;
        child.output.append(buffer, static_cast<std::size_t>(count));
      }
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      return 128 + WTERMSIG(status);
    }
    std::this_thread::sleep_for(10ms);
  }
  static_cast<void>(::kill(child.pid, SIGKILL));
  static_cast<void>(::waitpid(child.pid, &status, 0));
  return -1;
}

[[nodiscard]] auto echo_enabled(const Child& child) -> bool {
  termios state{};
  return ::tcgetattr(child.terminal, &state) == 0 &&
         (state.c_lflag & ECHO) != 0;
}

[[nodiscard]] auto file_mode(const std::filesystem::path& path) -> mode_t {
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0 ? info.st_mode & 0777 : 0;
}

auto fail(const std::string_view message, Child* child = nullptr) -> int {
  std::cerr << message << '\n';
  if (child != nullptr) std::cerr << child->output << '\n';
  return 1;
}

[[nodiscard]] auto write_offline_catalog(
    const std::filesystem::path& config_home) -> bool {
  const auto directory = config_home / "cache" / "aiforge";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::ofstream output{directory / "model-catalog.json", std::ios::binary};
  if (!output) return false;
  output
      << R"({"schema_version":2,"fetched_at_ms":)" << now
      << R"(,"source_id":"test.models","source_revision":null,"entries":[{"id":"offline-model","type":"text","name":null,"context_window_tokens":8192,"maximum_output_tokens":1024,"offline":false,"traits":[],"capabilities":[],"pricing":null}]})";
  output.close();
  return output.good() &&
         ::chmod((directory / "model-catalog.json").c_str(), 0600) == 0;
}

} // namespace

auto main(const int argc, char* argv[]) -> int {
  if (argc != 2) return fail("expected the aiforge executable path");
  TemporaryDirectory temporary;
  if (temporary.path().empty())
    return fail("could not create a temporary directory");
  const std::string executable{argv[1]};
  const std::string secret{"pty-secret-do-not-echo"};

  auto success = spawn(executable, temporary.path() / "success");
  if (success.pid < 0 || !read_until(success, "Venice API key: ", 5s)) {
    close_child(success);
    return fail("login prompt did not appear", &success);
  }
  const auto input = secret + "\n";
  if (::write(success.master, input.data(), input.size()) !=
      static_cast<ssize_t>(input.size())) {
    close_child(success);
    return fail("could not write the login credential", &success);
  }
  const auto success_status = finish(success, 5s);
  if (success_status != 0 || success.output.find(secret) != std::string::npos ||
      success.output.find("Venice credential stored.") == std::string::npos ||
      !echo_enabled(success)) {
    close_child(success);
    return fail("successful login leaked input or failed to restore echo",
                &success);
  }
  const auto credential_path =
      temporary.path() / "success" / "aiforge" / "credentials";
  std::ifstream credential{credential_path, std::ios::binary};
  const std::string stored{std::istreambuf_iterator<char>{credential},
                           std::istreambuf_iterator<char>{}};
  if (stored != secret + "\n" || file_mode(credential_path) != 0600 ||
      file_mode(credential_path.parent_path()) != 0700) {
    close_child(success);
    return fail("login did not publish a restrictive credential file",
                &success);
  }
  close_child(success);

  auto cancelled = spawn(executable, temporary.path() / "cancelled");
  if (cancelled.pid < 0 || !read_until(cancelled, "Venice API key: ", 5s)) {
    close_child(cancelled);
    return fail("cancelled login prompt did not appear", &cancelled);
  }
  const char eof = 4;
  if (::write(cancelled.master, &eof, 1) != 1) {
    close_child(cancelled);
    return fail("could not send login EOF", &cancelled);
  }
  const auto cancelled_status = finish(cancelled, 5s);
  const auto cancelled_path =
      temporary.path() / "cancelled" / "aiforge" / "credentials";
  if (cancelled_status != 130 || !echo_enabled(cancelled) ||
      std::filesystem::exists(cancelled_path)) {
    close_child(cancelled);
    return fail("cancelled login did not restore echo safely", &cancelled);
  }
  close_child(cancelled);

  const auto offline_home = temporary.path() / "offline";
  if (!write_offline_catalog(offline_home)) {
    return fail("could not prepare the offline model catalog");
  }
  auto offline = spawn(executable, offline_home, ChildMode::offline_chat);
  if (offline.pid < 0 || !read_until(offline, "Enter submit", 5s)) {
    close_child(offline);
    return fail("credential-free interactive chat did not open", &offline);
  }
  const std::string prompt{"credential check\r"};
  if (::write(offline.master, prompt.data(), prompt.size()) !=
          static_cast<ssize_t>(prompt.size()) ||
      !read_until(offline, "backend credential is not configured", 5s)) {
    close_child(offline);
    return fail(
        "credential-free interactive submission was not rejected safely",
        &offline);
  }
  const char exit_key = 4;
  if (::write(offline.master, &exit_key, 1) != 1) {
    close_child(offline);
    return fail("could not exit credential-free interactive chat", &offline);
  }
  const auto offline_status = finish(offline, 5s);
  if (offline_status != 0) {
    close_child(offline);
    return fail("credential-free interactive chat failed during startup",
                &offline);
  }
  close_child(offline);
  return 0;
}
