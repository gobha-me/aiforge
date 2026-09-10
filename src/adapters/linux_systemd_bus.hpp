#pragma once

// Private to adapters: no arbitrary bus address, interface, member or FD enters
// the public observation port. This foundation does not advertise services yet.
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

#include <aiforge/runtime/ops_observation_source.hpp>
#include <dbus/dbus.h>

namespace aiforge::adapters {
struct LinuxSystemdMessageDeleter {
  void operator()(DBusMessage* message) const noexcept;
};
using LinuxSystemdMessage =
    std::unique_ptr<DBusMessage, LinuxSystemdMessageDeleter>;
struct LinuxSystemdBudget {
  std::chrono::steady_clock::time_point deadline;
  std::stop_token stop;
  std::uint32_t remaining_calls{1024};
  [[nodiscard]] auto check() const
      -> std::expected<void, runtime::OpsObservationSourceError>;
};
// Exactly the read subset needed by the service decoder. Unit names are
// grammar-checked .service identities, never paths or arbitrary arguments.
// The later service decoder must verify GetUnit's object path and canonical Id
// before requesting properties by that canonical unit name. Raw DBus replies
// remain private here; this transport does not validate service payload
// schemas.
enum class LinuxSystemdRead {
  list_services,
  find_unit,
  unit_id,
  active_state,
  invocation_id,
  service_result,
  execution_code,
  execution_status,
  restart_count
};
struct LinuxSystemdPeer {
  std::uint32_t process_id{};
  std::uint32_t user_id{};
  std::uint64_t pid_namespace{};
  std::uint64_t mount_namespace{};
  std::uint64_t user_namespace{};
};
[[nodiscard]] auto validate_linux_systemd_version(int major, int minor,
                                                  int micro)
    -> std::expected<void, runtime::OpsObservationSourceError>;
[[nodiscard]] auto validate_linux_systemd_peer(
    const domain::LinuxOpsIdentity& binding, std::uint64_t user_namespace,
    const LinuxSystemdPeer& peer)
    -> std::expected<void, runtime::OpsObservationSourceError>;

// Deterministic private wire seam. Production uses libdbus on its one owning
// worker; fixtures construct messages in memory and never connect to host
// buses.
class LinuxSystemdWire {
 public:
  virtual ~LinuxSystemdWire() = default;
  [[nodiscard]] virtual auto send(DBusMessage& message)
      -> std::expected<std::uint32_t, runtime::OpsObservationSourceError> = 0;
  [[nodiscard]] virtual auto receive(std::chrono::milliseconds wait)
      -> std::expected<LinuxSystemdMessage,
                       runtime::OpsObservationSourceError> = 0;
};
class LinuxSystemdPlatform {
 public:
  virtual ~LinuxSystemdPlatform() = default;
  [[nodiscard]] virtual auto verify(LinuxSystemdBudget& budget)
      -> std::expected<void, runtime::OpsObservationSourceError> = 0;
  // Must set transport limits and verify the peer before returning. Does not
  // authenticate/read/query the bus; the handshake follows on this worker.
  [[nodiscard]] virtual auto open(LinuxSystemdBudget& budget)
      -> std::expected<std::unique_ptr<LinuxSystemdWire>,
                       runtime::OpsObservationSourceError> = 0;
  [[nodiscard]] virtual auto verify_connection(LinuxSystemdWire& wire,
                                               LinuxSystemdBudget& budget)
      -> std::expected<void, runtime::OpsObservationSourceError> = 0;
};
class LinuxSystemdConnection {
 public:
  ~LinuxSystemdConnection();
  [[nodiscard]] auto read(LinuxSystemdRead operation, std::string_view unit,
                          LinuxSystemdBudget& budget)
      -> std::expected<LinuxSystemdMessage, runtime::OpsObservationSourceError>;
  // Required before delivering decoded service evidence. No reconnect/retry.
  [[nodiscard]] auto verify(LinuxSystemdBudget& budget)
      -> std::expected<void, runtime::OpsObservationSourceError>;

 private:
  friend class LinuxSystemdBus;
  struct Impl;
  explicit LinuxSystemdConnection(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
class LinuxSystemdBus {
 public:
  [[nodiscard]] static auto create(domain::OpsTargetBinding binding)
      -> std::expected<std::shared_ptr<LinuxSystemdBus>,
                       runtime::OpsObservationSourceError>;
  [[nodiscard]] auto open(LinuxSystemdBudget& budget) const
      -> std::expected<std::unique_ptr<LinuxSystemdConnection>,
                       runtime::OpsObservationSourceError>;

 private:
  friend struct LinuxSystemdBusAccess;
  explicit LinuxSystemdBus(std::shared_ptr<LinuxSystemdPlatform> platform);
  std::shared_ptr<LinuxSystemdPlatform> m_platform;
};
struct LinuxSystemdBusAccess {
  [[nodiscard]] static auto create(
      std::shared_ptr<LinuxSystemdPlatform> platform)
      -> std::expected<std::shared_ptr<LinuxSystemdBus>,
                       runtime::OpsObservationSourceError>;
  // Takes ownership of a private connection to a test-owned Unix fixture.
  // Exercises production limits/send/read without selecting the host endpoint.
  [[nodiscard]] static auto fixture_wire(DBusConnection* connection)
      -> std::expected<std::unique_ptr<LinuxSystemdWire>,
                       runtime::OpsObservationSourceError>;
};
} // namespace aiforge::adapters
