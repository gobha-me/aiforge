#include <aiforge/adapters/agent_transport.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {
auto failure(surfaces::AgentErrorCode code, std::string message)
    -> std::unexpected<surfaces::AgentError> {
  return std::unexpected(surfaces::AgentError{code, std::move(message)});
}
#ifndef _WIN32
class NonblockingDescriptor final {
 public:
  explicit NonblockingDescriptor(const int descriptor, const bool input)
      : m_descriptor(descriptor) {
    if (descriptor < 0) return;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX flags query.
    m_flags = ::fcntl(descriptor, F_GETFL);
    if (m_flags < 0) return;
    const auto mode = m_flags & O_ACCMODE;
    if ((input && mode == O_WRONLY) || (!input && mode == O_RDONLY)) return;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX integer flags.
    m_ready = ::fcntl(descriptor, F_SETFL, m_flags | O_NONBLOCK) == 0;
  }
  ~NonblockingDescriptor() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Restore POSIX flags.
    if (m_ready) static_cast<void>(::fcntl(m_descriptor, F_SETFL, m_flags));
  }
  NonblockingDescriptor(const NonblockingDescriptor&) = delete;
  auto operator=(const NonblockingDescriptor&)
      -> NonblockingDescriptor& = delete;
  [[nodiscard]] auto ready() const -> bool { return m_ready; }
  [[nodiscard]] auto descriptor() const -> int { return m_descriptor; }

 private:
  int m_descriptor;
  int m_flags{-1};
  bool m_ready{};
};

auto read_chunk(const int input, std::array<char, 4096>& buffer,
                const std::stop_token stop)
    -> std::expected<std::size_t, surfaces::AgentError> {
  for (;;) {
    if (stop.stop_requested())
      return failure(surfaces::AgentErrorCode::cancelled,
                     "agent input cancelled");
    pollfd descriptor{input, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, 20);
    if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
    if (ready < 0 || (descriptor.revents & POLLNVAL) != 0) break;
    const auto count = ::read(input, buffer.data(), buffer.size());
    if (count >= 0) return static_cast<std::size_t>(count);
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
  }
  return failure(surfaces::AgentErrorCode::invalid_request,
                 "agent input could not be read");
}

auto write_all(const int output, std::string_view record,
               const std::stop_token stop,
               const std::chrono::milliseconds timeout)
    -> std::expected<void, surfaces::AgentError> {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!record.empty()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return failure(surfaces::AgentErrorCode::output_failed,
                     "agent output deadline exceeded");
    // Attempt an immediately writable cancellation/terminal record even after
    // stop. A blocked descriptor must never postpone cancellation accounting.
    const auto written = ::write(output, record.data(), record.size());
    if (written > 0) {
      record.remove_prefix(static_cast<std::size_t>(written));
    } else if (written < 0 && errno != EINTR && errno != EAGAIN &&
               errno != EWOULDBLOCK) {
      return failure(surfaces::AgentErrorCode::output_failed,
                     "agent output closed or failed");
    }
    if (std::chrono::steady_clock::now() >= deadline)
      return failure(surfaces::AgentErrorCode::output_failed,
                     "agent output deadline exceeded");
    if (record.empty()) return {};
    if (stop.stop_requested())
      return failure(surfaces::AgentErrorCode::cancelled,
                     "agent output cancelled");
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())
            .count();
    const auto wait_ms =
        static_cast<int>(std::clamp<std::int64_t>(remaining, 1, 10));
    pollfd descriptor{output, POLLOUT, 0};
    const auto ready = ::poll(&descriptor, 1, wait_ms);
    if ((ready < 0 && errno != EINTR) ||
        (descriptor.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0)
      return failure(surfaces::AgentErrorCode::output_failed,
                     "agent output closed or failed");
  }
  return {};
}
#endif
} // namespace

struct AgentTransport::Impl {
#ifndef _WIN32
  NonblockingDescriptor input;
  NonblockingDescriptor output;
  Impl(int input_fd, int output_fd, std::stop_token token,
       std::chrono::milliseconds timeout)
      : input(input_fd, true), output(output_fd, false), stop(std::move(token)),
        output_timeout(timeout) {}
#endif
  std::stop_token stop;
  std::chrono::milliseconds output_timeout;
  bool read{};
  bool failed{};
};

AgentTransport::AgentTransport(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}
AgentTransport::~AgentTransport() = default;

auto AgentTransport::open(const int input_descriptor,
                          const int output_descriptor,
                          const std::stop_token stop_token,
                          const std::chrono::milliseconds output_timeout)
    -> std::expected<std::unique_ptr<AgentTransport>, surfaces::AgentError> {
  try {
#ifndef _WIN32
    if (input_descriptor == output_descriptor ||
        (output_timeout <= std::chrono::milliseconds::zero() ||
         output_timeout > std::chrono::seconds{2}))
      return failure(surfaces::AgentErrorCode::invalid_request,
                     "agent descriptor configuration is invalid");
    auto impl = std::make_unique<Impl>(input_descriptor, output_descriptor,
                                       stop_token, output_timeout);
    if (!impl->input.ready() || !impl->output.ready())
      return failure(
          surfaces::AgentErrorCode::unavailable,
          "agent requires readable stdin and writable stdout descriptors");
    return std::unique_ptr<AgentTransport>{new AgentTransport{std::move(impl)}};
#else
    static_cast<void>(input_descriptor);
    static_cast<void>(output_descriptor);
    static_cast<void>(stop_token);
    static_cast<void>(output_timeout);
    return failure(
        surfaces::AgentErrorCode::unavailable,
        "agent descriptor transport is unavailable on this platform");
#endif
  } catch (...) {
    return failure(surfaces::AgentErrorCode::internal_failure,
                   "agent transport initialization failed");
  }
}

auto AgentTransport::read_request()
    -> std::expected<std::string, surfaces::AgentError> {
  try {
    if (m_impl->read || m_impl->failed)
      return failure(surfaces::AgentErrorCode::invalid_request,
                     "agent input was already consumed");
    m_impl->read = true;
#ifndef _WIN32
    std::string input;
    std::array<char, 4096> buffer{};
    for (;;) {
      auto count = read_chunk(m_impl->input.descriptor(), buffer, m_impl->stop);
      if (m_impl->stop.stop_requested())
        return failure(surfaces::AgentErrorCode::cancelled,
                       "agent input cancelled");
      if (!count) return std::unexpected(std::move(count.error()));
      if (*count > surfaces::agent_maximum_input_bytes - input.size())
        return failure(surfaces::AgentErrorCode::resource_exhausted,
                       "agent input exceeds its byte limit");
      if (*count == 0) return input;
      input.append(buffer.data(), *count);
    }
#else
    return failure(surfaces::AgentErrorCode::unavailable,
                   "agent input is unavailable");
#endif
  } catch (...) {
    return failure(surfaces::AgentErrorCode::internal_failure,
                   "agent input failed internally");
  }
}

auto AgentTransport::write_record(const std::string_view record)
    -> std::expected<void, surfaces::AgentError> {
  try {
    if (m_impl->failed || record.empty() ||
        record.size() > surfaces::agent_maximum_record_bytes ||
        record.back() != '\n' ||
        record.substr(0, record.size() - 1).find('\n') !=
            std::string_view::npos) {
      m_impl->failed = true;
      return failure(surfaces::AgentErrorCode::output_failed,
                     "agent output record is invalid or the sink has failed");
    }
    m_impl->failed = true;
#ifndef _WIN32
    auto result = write_all(m_impl->output.descriptor(), record, m_impl->stop,
                            m_impl->output_timeout);
    if (result) m_impl->failed = false;
    return result;
#else
    return failure(surfaces::AgentErrorCode::unavailable,
                   "agent output is unavailable");
#endif
  } catch (...) {
    return failure(surfaces::AgentErrorCode::output_failed,
                   "agent output failed internally");
  }
}
} // namespace aiforge::adapters
