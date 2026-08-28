#include <aiforge/adapters/process_login.hpp>

#include <cerrno>
#include <istream>
#include <ostream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <utility>

#include <aiforge/credentials/credential.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(const cli::CommandFailureKind kind,
                           std::string message) -> cli::CommandFailure {
  return {kind, std::move(message)};
}

class EchoGuard final {
 public:
  explicit EchoGuard(const int descriptor) : m_descriptor(descriptor) {}
  EchoGuard(const EchoGuard&) = delete;
  auto operator=(const EchoGuard&) -> EchoGuard& = delete;
  ~EchoGuard() { static_cast<void>(restore()); }

  [[nodiscard]] auto disable() -> bool {
    if (m_descriptor < 0 || ::tcgetattr(m_descriptor, &m_original) != 0) {
      return false;
    }
    auto hidden = m_original;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (::tcsetattr(m_descriptor, TCSANOW, &hidden) != 0) return false;
    m_active = true;
    return true;
  }

  [[nodiscard]] auto restore() noexcept -> bool {
    if (m_active) {
      const bool restored =
          ::tcsetattr(m_descriptor, TCSANOW, &m_original) == 0;
      m_active = false;
      return restored;
    }
    return true;
  }

 private:
  int m_descriptor{-1};
  termios m_original{};
  bool m_active{};
};

} // namespace

auto ProcessLoginCommand::execute(cli::CommandEnvironment& environment,
                                  std::ostream& output, std::ostream& error)
    -> std::expected<void, cli::CommandFailure> {
  try {
    if (!environment.input_is_terminal || environment.input_descriptor < 0) {
      return std::unexpected(failure(
          cli::CommandFailureKind::usage,
          "login requires terminal input; credentials cannot be piped"));
    }
    if (environment.stop_token.stop_requested()) {
      return std::unexpected(
          failure(cli::CommandFailureKind::cancelled, "login cancelled"));
    }

    EchoGuard echo{environment.input_descriptor};
    if (!echo.disable()) {
      return std::unexpected(failure(cli::CommandFailureKind::runtime,
                                     "terminal echo could not be disabled"));
    }
    error << "Venice API key: " << std::flush;
    std::string value;
    value.reserve(256);
    bool ended_by_eof{};
    while (true) {
      const auto next = environment.input.get();
      if (next == std::char_traits<char>::eof()) {
        ended_by_eof = true;
        break;
      }
      if (next == '\n') break;
      if (value.size() >= credentials::maximum_credential_bytes) {
        const bool restored = echo.restore();
        error << '\n';
        if (!restored) {
          return std::unexpected(
              failure(cli::CommandFailureKind::runtime,
                      "terminal echo could not be restored"));
        }
        return std::unexpected(failure(cli::CommandFailureKind::usage,
                                       "the Venice credential exceeds 64 KiB"));
      }
      value.push_back(static_cast<char>(next));
    }
    const bool restored = echo.restore();
    error << '\n';
    if (!restored) {
      return std::unexpected(failure(cli::CommandFailureKind::runtime,
                                     "terminal echo could not be restored"));
    }
    if (ended_by_eof && value.empty()) {
      return std::unexpected(failure(cli::CommandFailureKind::cancelled,
                                     "credential entry cancelled"));
    }
    if (environment.stop_token.stop_requested()) {
      return std::unexpected(
          failure(cli::CommandFailureKind::cancelled, "login cancelled"));
    }
    auto secret = credentials::make_secret(std::move(value));
    if (!secret) {
      return std::unexpected(
          failure(cli::CommandFailureKind::usage, secret.error().message));
    }
    auto path = credentials::process_credential_path();
    if (!path) {
      return std::unexpected(
          failure(cli::CommandFailureKind::runtime, path.error().message));
    }
    credentials::FileCredentialStore store{std::move(*path)};
    auto stored = store.store(*secret);
    if (!stored) {
      return std::unexpected(
          failure(cli::CommandFailureKind::runtime, stored.error().message));
    }
    output << "Venice credential stored.\n";
    return {};
  } catch (...) {
    return std::unexpected(
        failure(cli::CommandFailureKind::runtime, "login failed internally"));
  }
}

} // namespace aiforge::adapters
