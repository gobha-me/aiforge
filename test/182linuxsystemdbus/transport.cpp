// Actual libdbus authentication/read regression against a task-owned Unix
// fixture. No system/session bus, service, user keyring or infrastructure read.
#include "../../src/adapters/linux_systemd_bus.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
using Message = aiforge::adapters::LinuxSystemdMessage;
using Clock = std::chrono::steady_clock;
class Socket {
 public:
  explicit Socket(int value = -1) : m_value(value) {}
  ~Socket() {
    if (m_value >= 0) ::close(m_value);
  }
  Socket(const Socket&) = delete;
  auto operator=(const Socket&) -> Socket& = delete;
  [[nodiscard]] auto get() const -> int { return m_value; }

 private:
  int m_value;
};
class LocalBusFixture {
 public:
  LocalBusFixture()
      : m_listener(
            ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) {
    std::array<char, 40> pattern{};
    const std::string base{"/tmp/aiforge-dbus-fixture-XXXXXX"};
    std::copy(base.begin(), base.end(), pattern.begin());
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_directory = created;
    m_path = m_directory + "/bus";
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    REQUIRE(m_path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
    REQUIRE(m_listener.get() >= 0);
    REQUIRE(::bind(m_listener.get(),
                   reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) == 0);
    REQUIRE(::listen(m_listener.get(), 1) == 0);
    m_thread = std::jthread([this](std::stop_token stop) { serve(stop); });
  }
  ~LocalBusFixture() {
    m_thread.request_stop();
    if (m_thread.joinable()) m_thread.join();
    std::error_code ignored;
    std::filesystem::remove(m_path, ignored);
    std::filesystem::remove(m_directory, ignored);
  }
  [[nodiscard]] auto address() const -> std::string {
    return "unix:path=" + m_path;
  }
  std::atomic<bool> replied{};

 private:
  static auto ready(int descriptor, short events, std::stop_token stop,
                    Clock::time_point deadline) -> bool {
    while (!stop.stop_requested() && Clock::now() < deadline) {
      struct pollfd poll{descriptor, events, 0};
      const auto result = ::poll(&poll, 1, 20);
      if (result > 0) return (poll.revents & events) != 0;
      if (result < 0 && errno != EINTR) return false;
    }
    return false;
  }
  static auto write_all(int descriptor, std::string_view text,
                        std::stop_token stop, Clock::time_point deadline)
      -> bool {
    while (!text.empty()) {
      if (!ready(descriptor, POLLOUT, stop, deadline)) return false;
      const auto count =
          ::send(descriptor, text.data(), text.size(), MSG_NOSIGNAL);
      if (count > 0)
        text.remove_prefix(static_cast<std::size_t>(count));
      else if (count == 0 || (errno != EINTR && errno != EAGAIN))
        return false;
    }
    return true;
  }
  static auto line(int descriptor, std::stop_token stop,
                   Clock::time_point deadline) -> std::string {
    std::string result;
    while (result.size() < 512) {
      if (!ready(descriptor, POLLIN, stop, deadline)) return {};
      char byte{};
      const auto count = ::read(descriptor, &byte, 1);
      if (count == 1) {
        if (byte != '\0') result.push_back(byte);
        if (result.ends_with("\r\n")) return result;
      } else if (count == 0 || (errno != EINTR && errno != EAGAIN))
        return {};
    }
    return {};
  }
  void serve(std::stop_token stop) {
    try {
      const auto deadline = Clock::now() + std::chrono::seconds{3};
      if (!ready(m_listener.get(), POLLIN, stop, deadline)) return;
      const Socket client{::accept4(m_listener.get(), nullptr, nullptr,
                                    SOCK_CLOEXEC | SOCK_NONBLOCK)};
      if (client.get() < 0) return;
      if (!line(client.get(), stop, deadline).starts_with("AUTH EXTERNAL "))
        return;
      if (!write_all(client.get(), "OK 0123456789abcdef0123456789abcdef\r\n",
                     stop, deadline))
        return;
      auto next = line(client.get(), stop, deadline);
      if (next == "NEGOTIATE_UNIX_FD\r\n") {
        if (!write_all(client.get(), "ERROR\r\n", stop, deadline)) return;
        next = line(client.get(), stop, deadline);
      }
      if (next != "BEGIN\r\n") return;
      std::array<char, 4096> bytes{};
      std::size_t used{};
      int needed{};
      while (needed <= 0 || used < static_cast<std::size_t>(needed)) {
        if (used == bytes.size() ||
            !ready(client.get(), POLLIN, stop, deadline))
          return;
        const auto count =
            ::read(client.get(), bytes.data() + used, bytes.size() - used);
        if (count <= 0) return;
        used += static_cast<std::size_t>(count);
        needed = dbus_message_demarshal_bytes_needed(bytes.data(),
                                                     static_cast<int>(used));
        if (needed < 0 || needed > static_cast<int>(bytes.size())) return;
      }
      Message request{dbus_message_demarshal(bytes.data(), needed, nullptr)};
      if (!request ||
          std::string_view{dbus_message_get_member(request.get())} != "Hello")
        return;
      Message response{dbus_message_new_method_return(request.get())};
      if (!response ||
          !dbus_message_set_sender(response.get(), "org.freedesktop.DBus"))
        return;
      dbus_message_set_serial(response.get(), 1);
      const char* unique = ":1.2";
      if (!dbus_message_append_args(response.get(), DBUS_TYPE_STRING, &unique,
                                    DBUS_TYPE_INVALID))
        return;
      char* encoded{};
      int length{};
      if (!dbus_message_marshal(response.get(), &encoded, &length)) return;
      const bool sent =
          write_all(client.get(),
                    std::string_view{encoded, static_cast<std::size_t>(length)},
                    stop, deadline);
      dbus_free(encoded);
      replied = sent;
      // Keep the peer alive until the test disposes its connection. All waits
      // remain finite and observe stop; no process or join-on-blocked-read.
      while (!stop.stop_requested() && Clock::now() < deadline) {
        struct pollfd poll{client.get(), POLLIN, 0};
        if (::poll(&poll, 1, 20) > 0 && (poll.revents & (POLLHUP | POLLERR)))
          return;
      }
    } catch (...) {
      replied = false;
    }
  }
  Socket m_listener;
  std::string m_directory, m_path;
  std::jthread m_thread;
};
} // namespace

TEST_CASE("Production libdbus caps allow an authenticated zero-FD reply",
          "[linux-systemd][unix-fixture]") {
  REQUIRE(dbus_threads_init_default());
  dbus_connection_set_change_sigpipe(false);
  LocalBusFixture fixture;
  DBusError error = DBUS_ERROR_INIT;
  auto* connection =
      dbus_connection_open_private(fixture.address().c_str(), &error);
  dbus_error_free(&error);
  REQUIRE(connection != nullptr);
  REQUIRE_FALSE(dbus_connection_get_is_authenticated(connection));
  auto wire =
      aiforge::adapters::LinuxSystemdBusAccess::fixture_wire(connection);
  REQUIRE(wire);
  REQUIRE(dbus_connection_get_max_message_unix_fds(connection) == 0);
  REQUIRE(dbus_connection_get_max_received_unix_fds(connection) == 1);
  REQUIRE(dbus_connection_get_max_message_size(connection) == 64 * 1024);
  REQUIRE(dbus_connection_get_max_received_size(connection) == 64 * 1024);
  REQUIRE_FALSE(dbus_connection_get_is_authenticated(connection));
  Message hello{dbus_message_new_method_call("org.freedesktop.DBus",
                                             "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus", "Hello")};
  REQUIRE(hello);
  auto serial = (*wire)->send(*hello);
  REQUIRE(serial);
  const auto deadline = Clock::now() + std::chrono::seconds{2};
  Message reply;
  while (!reply && Clock::now() < deadline) {
    auto received = (*wire)->receive(std::chrono::milliseconds{20});
    REQUIRE(received);
    reply = std::move(*received);
  }
  REQUIRE(reply);
  REQUIRE(fixture.replied);
  REQUIRE(dbus_message_get_reply_serial(reply.get()) == *serial);
  REQUIRE(dbus_message_has_signature(reply.get(), "s"));
  REQUIRE_FALSE(dbus_message_contains_unix_fds(reply.get()));
  REQUIRE(dbus_connection_get_is_authenticated(connection));
}
