#include "linux_systemd_bus.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <mutex>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
constexpr long maximum_message_bytes = 64L * 1024;
constexpr long maximum_queued_bytes = 64L * 1024;
constexpr const char* fixed_address = "unix:path=/run/dbus/system_bus_socket";
auto fail(Error error) -> std::unexpected<Error> {
  return std::unexpected(error);
}
auto system_error() -> Error {
  return errno == EACCES || errno == EPERM ? Error::permission_denied
                                           : Error::unavailable;
}
class Descriptor {
 public:
  explicit Descriptor(int descriptor = -1) : m_descriptor(descriptor) {}
  ~Descriptor() {
    if (m_descriptor >= 0) ::close(m_descriptor);
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept
      : m_descriptor(std::exchange(other.m_descriptor, -1)) {}
  auto operator=(Descriptor&& other) noexcept -> Descriptor& {
    if (this != &other) {
      if (m_descriptor >= 0) ::close(m_descriptor);
      m_descriptor = std::exchange(other.m_descriptor, -1);
    }
    return *this;
  }
  [[nodiscard]] auto get() const -> int { return m_descriptor; }

 private:
  int m_descriptor;
};
struct Namespace {
  Descriptor descriptor;
  std::uint64_t inode{};
};
auto open_namespace(int root, const std::string& path, int kind,
                    LinuxSystemdBudget& budget)
    -> std::expected<Namespace, Error> {
  if (auto ready = budget.check(); !ready) return fail(ready.error());
  // Fixed procfs namespace magic links, not caller-selected filesystem paths.
  const auto flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux syscall API.
  const auto opened = ::openat(root, path.c_str(), flags);
  Descriptor descriptor{opened};
  if (descriptor.get() < 0) return fail(system_error());
  struct stat metadata{};
  struct statfs filesystem{};
  if (::fstat(descriptor.get(), &metadata) != 0 ||
      ::fstatfs(descriptor.get(), &filesystem) != 0)
    return fail(system_error());
  if (filesystem.f_type != NSFS_MAGIC || metadata.st_ino == 0)
    return fail(Error::trust_failed);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux syscall API.
  const auto actual = ::ioctl(descriptor.get(), NS_GET_NSTYPE);
  if (actual != kind) return fail(Error::trust_failed);
  return Namespace{std::move(descriptor),
                   static_cast<std::uint64_t>(metadata.st_ino)};
}
struct Namespaces {
  Namespace pid, mount, user;
};
auto namespaces(int root, std::string_view process, LinuxSystemdBudget& budget)
    -> std::expected<Namespaces, Error> {
  const auto prefix = std::string{process} + "/ns/";
  auto pid = open_namespace(root, prefix + "pid", CLONE_NEWPID, budget);
  if (!pid) return fail(pid.error());
  auto mount = open_namespace(root, prefix + "mnt", CLONE_NEWNS, budget);
  if (!mount) return fail(mount.error());
  auto user = open_namespace(root, prefix + "user", CLONE_NEWUSER, budget);
  if (!user) return fail(user.error());
  return Namespaces{std::move(*pid), std::move(*mount), std::move(*user)};
}
auto boot_id(int root, LinuxSystemdBudget& budget)
    -> std::expected<std::string, Error> {
  if (auto ready = budget.check(); !ready) return fail(ready.error());
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux syscall API.
  const auto opened = ::openat(root, "sys/kernel/random/boot_id",
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  Descriptor descriptor{opened};
  if (descriptor.get() < 0) return fail(system_error());
  struct statfs filesystem{};
  if (::fstatfs(descriptor.get(), &filesystem) != 0 ||
      filesystem.f_type != PROC_SUPER_MAGIC)
    return fail(Error::trust_failed);
  std::array<char, 38> bytes{};
  const auto count = ::read(descriptor.get(), bytes.data(), bytes.size());
  if (count < 0) return fail(system_error());
  if (count != 37 || bytes[36] != '\n') return fail(Error::invalid_result);
  return std::string{bytes.data(), 36};
}
struct SocketIdentity {
  dev_t device{};
  ino_t inode{};
  uid_t owner{};
  auto operator==(const SocketIdentity&) const -> bool = default;
};
auto verify_directory(int descriptor) -> std::expected<void, Error> {
  struct stat metadata{};
  if (::fstat(descriptor, &metadata) != 0) return fail(system_error());
  if (!S_ISDIR(metadata.st_mode) || metadata.st_uid != 0 ||
      (metadata.st_mode & 0022) != 0)
    return fail(Error::trust_failed);
  return {};
}
auto socket_identity(LinuxSystemdBudget& budget)
    -> std::expected<SocketIdentity, Error> {
  if (auto ready = budget.check(); !ready) return fail(ready.error());
  constexpr auto flags =
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux syscall API.
  Descriptor directory{::open("/", flags)};
  if (directory.get() < 0) return fail(system_error());
  if (auto checked = verify_directory(directory.get()); !checked)
    return fail(checked.error());
  for (const auto* component : {"run", "dbus"}) {
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux syscall API.
    const auto opened = ::openat(directory.get(), component, flags);
    if (opened < 0) return fail(system_error());
    directory = Descriptor{opened};
    if (auto checked = verify_directory(directory.get()); !checked)
      return fail(checked.error());
  }
  struct stat metadata{};
  if (::fstatat(directory.get(), "system_bus_socket", &metadata,
                AT_SYMLINK_NOFOLLOW) != 0)
    return fail(system_error());
  if (!S_ISSOCK(metadata.st_mode) || metadata.st_ino == 0)
    return fail(Error::trust_failed);
  return SocketIdentity{metadata.st_dev, metadata.st_ino, metadata.st_uid};
}
struct ConnectionDeleter {
  void operator()(DBusConnection* connection) const noexcept {
    if (connection == nullptr) return;
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
  }
};
using Connection = std::unique_ptr<DBusConnection, ConnectionDeleter>;
void configure_limits(DBusConnection& connection) {
  dbus_connection_set_max_message_size(&connection, maximum_message_bytes);
  dbus_connection_set_max_received_size(&connection, maximum_queued_bytes);
  dbus_connection_set_max_message_unix_fds(&connection, 0);
  // Zero here disables reading even messages with no FDs (0 >= 0). The
  // per-message zero cap rejects FDs; one is the smallest usable queue gate.
  dbus_connection_set_max_received_unix_fds(&connection, 1);
  dbus_connection_set_exit_on_disconnect(&connection, 0);
}
class Wire final : public LinuxSystemdWire {
 public:
  Wire(Connection connection, SocketIdentity identity)
      : m_connection(std::move(connection)), m_identity(identity) {}
  [[nodiscard]] auto socket() const -> std::expected<int, Error> {
    int result{};
    if (dbus_connection_get_unix_fd(m_connection.get(), &result) == 0)
      return fail(Error::trust_failed);
    return result;
  }
  [[nodiscard]] auto identity() const -> const SocketIdentity& {
    return m_identity;
  }
  auto send(DBusMessage& message)
      -> std::expected<std::uint32_t, Error> override {
    dbus_uint32_t serial{};
    if (dbus_connection_send(m_connection.get(), &message, &serial) == 0)
      return fail(Error::resource_exhausted);
    return serial;
  }
  auto receive(std::chrono::milliseconds wait)
      -> std::expected<LinuxSystemdMessage, Error> override {
    // No dispatch callbacks: only closed request messages are sent. Unrelated
    // inbound messages are counted/discarded by the private protocol boundary.
    if (dbus_connection_read_write(m_connection.get(),
                                   static_cast<int>(wait.count())) == 0)
      return fail(Error::disconnected);
    return LinuxSystemdMessage{dbus_connection_pop_message(m_connection.get())};
  }

 private:
  Connection m_connection;
  SocketIdentity m_identity;
};
class Platform final : public LinuxSystemdPlatform {
 public:
  Platform(domain::LinuxOpsIdentity binding, Descriptor root, Namespaces pinned)
      : m_binding(std::move(binding)), m_root(std::move(root)),
        m_pinned(std::move(pinned)) {}
  auto verify(LinuxSystemdBudget& budget)
      -> std::expected<void, Error> override {
    auto current = namespaces(m_root.get(), "thread-self", budget);
    if (!current) return fail(current.error());
    const LinuxSystemdPeer identity{1, 0, current->pid.inode,
                                    current->mount.inode, current->user.inode};
    if (auto checked = validate_linux_systemd_peer(
            m_binding, m_pinned.user.inode, identity);
        !checked)
      return fail(Error::source_changed);
    auto manager = namespaces(m_root.get(), "1", budget);
    if (!manager) return fail(manager.error());
    const LinuxSystemdPeer init{1, 0, manager->pid.inode, manager->mount.inode,
                                manager->user.inode};
    if (auto checked =
            validate_linux_systemd_peer(m_binding, m_pinned.user.inode, init);
        !checked)
      return fail(Error::trust_failed);
    auto boot = boot_id(m_root.get(), budget);
    if (!boot) return fail(boot.error());
    if (*boot != m_binding.boot_id) return fail(Error::source_changed);
    return budget.check();
  }
  auto open(LinuxSystemdBudget& budget)
      -> std::expected<std::unique_ptr<LinuxSystemdWire>, Error> override {
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    int major{};
    int minor{};
    int micro{};
    dbus_get_version(&major, &minor, &micro);
    if (auto version = validate_linux_systemd_version(major, minor, micro);
        !version)
      return fail(version.error());
    auto before = socket_identity(budget);
    if (!before) return fail(before.error());
    static std::once_flag initialized;
    static bool available{};
    std::call_once(initialized, [] {
      dbus_connection_set_change_sigpipe(0);
      available = dbus_threads_init_default() != 0;
    });
    if (!available) return fail(Error::resource_exhausted);
    DBusError error = DBUS_ERROR_INIT;
    Connection connection{dbus_connection_open_private(fixed_address, &error)};
    if (!connection) {
      const bool denied =
          dbus_error_has_name(&error, DBUS_ERROR_ACCESS_DENIED) != 0;
      dbus_error_free(&error);
      return fail(denied ? Error::permission_denied : Error::unavailable);
    }
    dbus_error_free(&error);
    // These APIs run before auth/poll. Queue limits allow one message/read
    // buffer overshoot and do not count cumulative wire bytes.
    configure_limits(*connection);
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    int socket{};
    if (dbus_connection_get_unix_fd(connection.get(), &socket) == 0)
      return fail(Error::trust_failed);
    struct ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
        size != sizeof(peer) || peer.pid <= 0)
      return fail(Error::trust_failed);
    if (before->owner != 0 && before->owner != peer.uid)
      return fail(Error::trust_failed);
    auto peer_namespaces =
        namespaces(m_root.get(), std::to_string(peer.pid), budget);
    if (!peer_namespaces) return fail(peer_namespaces.error());
    const LinuxSystemdPeer proof{static_cast<std::uint32_t>(peer.pid), peer.uid,
                                 peer_namespaces->pid.inode,
                                 peer_namespaces->mount.inode,
                                 peer_namespaces->user.inode};
    if (auto checked =
            validate_linux_systemd_peer(m_binding, m_pinned.user.inode, proof);
        !checked)
      return fail(checked.error());
    auto after = socket_identity(budget);
    if (!after) return fail(after.error());
    if (*after != *before) return fail(Error::source_changed);
    if (auto checked = verify(budget); !checked) return fail(checked.error());
    return std::make_unique<Wire>(std::move(connection), *before);
  }
  auto verify_connection(LinuxSystemdWire& wire, LinuxSystemdBudget& budget)
      -> std::expected<void, Error> override {
    auto* connected = dynamic_cast<Wire*>(&wire);
    if (connected == nullptr) return fail(Error::internal_failure);
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    auto socket = connected->socket();
    if (!socket) return fail(socket.error());
    struct ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(*socket, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
        size != sizeof(peer) || peer.pid <= 0)
      return fail(Error::trust_failed);
    auto source = socket_identity(budget);
    if (!source) return fail(source.error());
    if (*source != connected->identity()) return fail(Error::source_changed);
    if (source->owner != 0 && source->owner != peer.uid)
      return fail(Error::trust_failed);
    auto peer_namespaces =
        namespaces(m_root.get(), std::to_string(peer.pid), budget);
    if (!peer_namespaces) return fail(peer_namespaces.error());
    return validate_linux_systemd_peer(m_binding, m_pinned.user.inode,
                                       {static_cast<std::uint32_t>(peer.pid),
                                        peer.uid, peer_namespaces->pid.inode,
                                        peer_namespaces->mount.inode,
                                        peer_namespaces->user.inode});
  }

 private:
  const domain::LinuxOpsIdentity m_binding;
  Descriptor m_root;
  Namespaces m_pinned;
};
} // namespace

auto LinuxSystemdBusAccess::fixture_wire(DBusConnection* connection)
    -> std::expected<std::unique_ptr<LinuxSystemdWire>, Error> {
  Connection owned{connection};
  try {
    if (!owned) return fail(Error::unavailable);
    configure_limits(*owned);
    return std::make_unique<Wire>(std::move(owned), SocketIdentity{});
  } catch (...) {
    return fail(Error::internal_failure);
  }
}

auto LinuxSystemdBus::create(domain::OpsTargetBinding binding)
    -> std::expected<std::shared_ptr<LinuxSystemdBus>, Error> {
  try {
    if (!domain::validate_ops_target_binding(binding))
      return fail(Error::invalid_result);
    const auto* identity =
        std::get_if<domain::LinuxOpsIdentity>(&binding.identity);
    if (identity == nullptr ||
        identity->scope == domain::LinuxExecutionScope::host)
      return fail(Error::unsupported);
    LinuxSystemdBudget budget{
        std::chrono::steady_clock::now() + std::chrono::seconds{5}, {}};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux syscall API.
    Descriptor root{::open("/proc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                        O_CLOEXEC | O_NONBLOCK)};
    if (root.get() < 0) return fail(system_error());
    struct statfs filesystem{};
    if (::fstatfs(root.get(), &filesystem) != 0 ||
        filesystem.f_type != PROC_SUPER_MAGIC)
      return fail(Error::trust_failed);
    auto pinned = namespaces(root.get(), "thread-self", budget);
    if (!pinned) return fail(pinned.error());
    const LinuxSystemdPeer current{1, 0, pinned->pid.inode, pinned->mount.inode,
                                   pinned->user.inode};
    if (auto checked =
            validate_linux_systemd_peer(*identity, pinned->user.inode, current);
        !checked)
      return fail(Error::source_changed);
    auto platform = std::make_shared<Platform>(*identity, std::move(root),
                                               std::move(*pinned));
    if (auto verified = platform->verify(budget); !verified)
      return fail(verified.error());
    return LinuxSystemdBusAccess::create(std::move(platform));
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
