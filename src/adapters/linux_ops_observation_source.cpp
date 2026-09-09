#include <aiforge/adapters/linux_ops_observation_source.hpp>

#include "linux_ops_observation_source_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <string_view>
#include <utility>

#include <aiforge/detail/utf8_text.hpp>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
using Clock = std::chrono::steady_clock;
constexpr std::size_t maximum_input = std::size_t{64} * 1024;
constexpr std::size_t maximum_memory_input = std::size_t{16} * 1024;
auto fail(Error error) -> std::unexpected<Error> {
  return std::unexpected(error);
}
auto valid_id(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 128 &&
         detail::is_safe_utf8_text(value);
}
auto timestamp() -> domain::EventTimestamp {
  return domain::EventTimestamp{
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())};
}
auto trim(std::string_view value) -> std::string_view {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}
auto line(std::string_view& input) -> std::string_view {
  const auto end = input.find('\n');
  const auto value = input.substr(0, end);
  input = end == std::string_view::npos ? std::string_view{}
                                        : input.substr(end + 1);
  return value;
}
auto token(std::string_view& input) -> std::string_view {
  input = trim(input);
  const auto end = input.find_first_of(" \t");
  const auto value = input.substr(0, end);
  input =
      end == std::string_view::npos ? std::string_view{} : input.substr(end);
  return value;
}
auto valid_input(std::string_view input, std::size_t maximum) -> bool {
  return !input.empty() && input.size() < maximum &&
         std::ranges::all_of(
             input,
             [](unsigned char byte) {
               return byte == '\n' || byte == '\t' ||
                      (byte >= 32 && byte < 127);
             });
}
template <typename Number>
auto number(std::string_view value) -> std::optional<Number> {
  if (value.empty()) return {};
  Number result{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    return {};
  return result;
}
auto decimal_seconds(std::string_view value) -> std::optional<std::uint64_t> {
  const auto dot = value.find('.');
  if (dot == std::string_view::npos) return {};
  const auto fraction = value.substr(dot + 1);
  if (fraction.empty() || fraction.size() > 9 ||
      !std::ranges::all_of(fraction,
                           [](char c) { return c >= '0' && c <= '9'; }))
    return {};
  return number<std::uint64_t>(value.substr(0, dot));
}
auto parse_uptime(const std::optional<std::string>& input)
    -> std::expected<std::optional<std::uint64_t>, Error> {
  if (!input) return std::nullopt;
  if (!valid_input(*input, 256)) return fail(Error::invalid_result);
  std::string_view remaining{*input};
  auto first_line = line(remaining);
  const auto uptime = decimal_seconds(token(first_line));
  const auto idle = decimal_seconds(token(first_line));
  if (!uptime || !idle || !trim(first_line).empty() || !remaining.empty())
    return fail(Error::invalid_result);
  return uptime;
}
auto zero_boot_offset(const std::optional<std::string>& input)
    -> std::expected<bool, Error> {
  if (!input) return false;
  if (!valid_input(*input, 256)) return fail(Error::invalid_result);
  std::string_view remaining{*input};
  bool seen_monotonic{};
  bool seen_boot{};
  bool zero{};
  while (!remaining.empty()) {
    auto value = line(remaining);
    const auto name = token(value);
    const auto seconds = number<std::int64_t>(token(value));
    const auto nanos = number<std::uint64_t>(token(value));
    if (!seconds || !nanos || *nanos >= 1000000000 || !trim(value).empty())
      return fail(Error::invalid_result);
    if (name == "boottime" && !seen_boot) {
      seen_boot = true;
      zero = *seconds == 0 && *nanos == 0;
    } else if (name == "monotonic" && !seen_monotonic) {
      seen_monotonic = true;
    } else {
      return fail(Error::invalid_result);
    }
  }
  if (!seen_boot || !seen_monotonic) return fail(Error::invalid_result);
  return zero;
}
auto parse_memory(const std::optional<std::string>& input)
    -> std::expected<std::optional<domain::LinuxMemoryObservation>, Error> {
  if (!input) return std::nullopt;
  if (!valid_input(*input, maximum_memory_input))
    return fail(Error::invalid_result);
  std::optional<std::uint64_t> total;
  std::optional<std::uint64_t> available;
  std::string_view remaining{*input};
  while (!remaining.empty()) {
    auto value = line(remaining);
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) return fail(Error::invalid_result);
    const auto key = trim(value.substr(0, colon));
    value.remove_prefix(colon + 1);
    if (key != "MemTotal" && key != "MemAvailable") continue;
    const auto amount = number<std::uint64_t>(token(value));
    const auto unit = token(value);
    auto& destination = key == "MemTotal" ? total : available;
    if (destination || !amount || unit != "kB" || !trim(value).empty() ||
        *amount > std::numeric_limits<std::uint64_t>::max() / 1024)
      return fail(Error::invalid_result);
    destination = *amount * 1024;
  }
  if (!total || !available) return std::nullopt;
  if (*total == 0 || *available > *total) return fail(Error::invalid_result);
  return domain::LinuxMemoryObservation{domain::OpsMemoryScope::kernel, *total,
                                        *available};
}

#if defined(__linux__)
class Descriptor {
 public:
  explicit Descriptor(int value) : m_value(value) {}
  ~Descriptor() {
    if (m_value >= 0) {
      [[maybe_unused]] const auto closed = ::close(m_value);
    }
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept
      : m_value(std::exchange(other.m_value, -1)) {}
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }

 private:
  int m_value;
};
auto open_error() -> Error {
  return errno == EACCES || errno == EPERM ? Error::permission_denied
                                           : Error::unavailable;
}
auto read_contents(int descriptor, std::size_t limit,
                   LinuxOpsReadBudget& budget)
    -> std::expected<std::optional<std::string>, Error> {
  std::string result;
  std::array<char, 1024> buffer{};
  while (result.size() < limit) {
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    const auto capacity = std::min(
        {static_cast<std::uint64_t>(buffer.size()),
         static_cast<std::uint64_t>(limit - result.size()), budget.remaining});
    if (capacity == 0) return fail(Error::resource_exhausted);
    const auto count =
        ::read(descriptor, buffer.data(), static_cast<std::size_t>(capacity));
    if (count < 0) {
      if (errno == EINTR) continue;
      return fail(Error::unavailable);
    }
    if (count == 0) return std::optional{std::move(result)};
    if (auto consumed = budget.consume(static_cast<std::size_t>(count));
        !consumed)
      return std::unexpected(consumed.error());
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
  // Refuse an exact-cap input rather than reading one byte beyond the budget
  // merely to prove EOF on a procfs file with a synthetic stat size.
  return fail(Error::resource_exhausted);
}
auto proc_file(int root, const char* path, std::size_t limit,
               LinuxOpsReadBudget& budget, bool optional)
    -> std::expected<std::optional<std::string>, Error> {
  if (auto ready = budget.check(); !ready)
    return std::unexpected(ready.error());
  // Fixed adapter-owned procfs paths; O_NONBLOCK avoids waiting on a replaced
  // non-proc FIFO before descriptor identity is checked.
  constexpr auto flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux ABI.
  const auto opened = ::openat(root, path, flags);
  const Descriptor file{opened};
  if (file.get() < 0) {
    if (optional && (errno == ENOENT || errno == EACCES || errno == EPERM))
      return std::nullopt;
    return fail(open_error());
  }
  struct stat metadata{};
  struct statfs filesystem{};
  if (::fstat(file.get(), &metadata) != 0 ||
      ::fstatfs(file.get(), &filesystem) != 0)
    return fail(Error::unavailable);
  if (!S_ISREG(metadata.st_mode) || filesystem.f_type != PROC_SUPER_MAGIC)
    return optional
               ? std::expected<std::optional<std::string>, Error>{std::nullopt}
               : fail(Error::unavailable);
  return read_contents(file.get(), limit, budget);
}
struct NamespaceDescriptor {
  Descriptor descriptor;
  std::uint64_t inode;
};
auto open_namespace(int root, const char* path, int type,
                    LinuxOpsReadBudget& budget)
    -> std::expected<NamespaceDescriptor, Error> {
  if (auto ready = budget.check(); !ready)
    return std::unexpected(ready.error());
  // Follow only the fixed kernel namespace magic link beneath real procfs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux ABI.
  const auto opened = ::openat(root, path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  Descriptor descriptor{opened};
  if (descriptor.get() < 0) return fail(open_error());
  struct stat metadata{};
  struct statfs filesystem{};
  if (::fstat(descriptor.get(), &metadata) != 0 ||
      ::fstatfs(descriptor.get(), &filesystem) != 0 ||
      filesystem.f_type != NSFS_MAGIC || metadata.st_ino == 0)
    return fail(Error::unavailable);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux ABI.
  const auto namespace_type = ::ioctl(descriptor.get(), NS_GET_NSTYPE);
  if (namespace_type != type) return fail(Error::unavailable);
  return NamespaceDescriptor{std::move(descriptor),
                             static_cast<std::uint64_t>(metadata.st_ino)};
}
struct TimeNamespaces {
  NamespaceDescriptor current;
  NamespaceDescriptor children;
};
auto time_namespaces(int root, LinuxOpsReadBudget& budget)
    -> std::expected<std::optional<TimeNamespaces>, Error> {
  auto current =
      open_namespace(root, "thread-self/ns/time", CLONE_NEWTIME, budget);
  if (!current) {
    if (current.error() == Error::cancelled ||
        current.error() == Error::timed_out)
      return std::unexpected(current.error());
    return std::nullopt;
  }
  auto children = open_namespace(root, "thread-self/ns/time_for_children",
                                 CLONE_NEWTIME, budget);
  if (!children) {
    if (children.error() == Error::cancelled ||
        children.error() == Error::timed_out)
      return std::unexpected(children.error());
    return std::nullopt;
  }
  return TimeNamespaces{std::move(*current), std::move(*children)};
}
class ProcProbe final : public LinuxOpsProbe {
 public:
  ProcProbe(Descriptor root, NamespaceDescriptor pid, NamespaceDescriptor mount,
            domain::LinuxExecutionScope scope,
            std::optional<TimeNamespaces> time)
      : m_root(std::move(root)), m_pid(std::move(pid)),
        m_mount(std::move(mount)), m_scope(scope), m_time(std::move(time)) {}
  [[nodiscard]] static auto create()
      -> std::expected<std::shared_ptr<ProcProbe>, Error> {
    LinuxOpsReadBudget budget{
        maximum_input, Clock::now() + std::chrono::seconds{5}, {}};
    constexpr auto flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux ABI.
    const auto opened = ::open("/proc", flags);
    Descriptor root{opened};
    if (root.get() < 0) return fail(open_error());
    struct statfs filesystem{};
    if (::fstatfs(root.get(), &filesystem) != 0 ||
        filesystem.f_type != PROC_SUPER_MAGIC)
      return fail(Error::unavailable);
    auto pid =
        open_namespace(root.get(), "thread-self/ns/pid", CLONE_NEWPID, budget);
    if (!pid) return std::unexpected(pid.error());
    auto mount =
        open_namespace(root.get(), "thread-self/ns/mnt", CLONE_NEWNS, budget);
    if (!mount) return std::unexpected(mount.error());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- Linux ABI.
    const auto parent_fd = ::ioctl(pid->descriptor.get(), NS_GET_PARENT);
    const Descriptor parent{parent_fd};
    const auto scope = parent.get() >= 0
                           ? domain::LinuxExecutionScope::container
                           : domain::LinuxExecutionScope::unknown;
    auto time = time_namespaces(root.get(), budget);
    if (!time) return std::unexpected(time.error());
    return std::make_shared<ProcProbe>(std::move(root), std::move(*pid),
                                       std::move(*mount), scope,
                                       std::move(*time));
  }
  auto identity(LinuxOpsReadBudget& budget)
      -> std::expected<domain::LinuxOpsIdentity, Error> override {
    auto pid = open_namespace(m_root.get(), "thread-self/ns/pid", CLONE_NEWPID,
                              budget);
    if (!pid) return std::unexpected(pid.error());
    auto mount =
        open_namespace(m_root.get(), "thread-self/ns/mnt", CLONE_NEWNS, budget);
    if (!mount) return std::unexpected(mount.error());
    if (pid->inode != m_pid.inode || mount->inode != m_mount.inode)
      return fail(Error::source_changed);
    auto boot =
        proc_file(m_root.get(), "sys/kernel/random/boot_id", 64, budget, false);
    if (!boot) return std::unexpected(boot.error());
    if (!*boot || (**boot).size() != 37 || (**boot).back() != '\n')
      return fail(Error::invalid_result);
    (**boot).pop_back();
    return domain::LinuxOpsIdentity{m_scope, std::move(**boot), pid->inode,
                                    mount->inode};
  }
  auto read(LinuxOpsInputFile file, LinuxOpsReadBudget& budget)
      -> std::expected<std::optional<std::string>, Error> override {
    switch (file) {
      case LinuxOpsInputFile::uptime:
        return proc_file(m_root.get(), "uptime", 256, budget, true);
      case LinuxOpsInputFile::memory:
        return proc_file(m_root.get(), "meminfo", maximum_memory_input, budget,
                         true);
      case LinuxOpsInputFile::time_offsets:
        return proc_file(m_root.get(), "thread-self/timens_offsets", 256,
                         budget, true);
    }
    return fail(Error::unsupported);
  }

  auto time_identity(LinuxOpsReadBudget& budget)
      -> std::expected<std::optional<LinuxOpsTimeIdentity>, Error> override {
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    if (!m_time) return std::nullopt;
    auto current = time_namespaces(m_root.get(), budget);
    if (!current) return std::unexpected(current.error());
    if (!*current || (**current).current.inode != m_time->current.inode ||
        (**current).children.inode != m_time->children.inode)
      return std::nullopt;
    return LinuxOpsTimeIdentity{m_time->current.inode, m_time->children.inode};
  }

 private:
  Descriptor m_root;
  NamespaceDescriptor m_pid;
  NamespaceDescriptor m_mount;
  domain::LinuxExecutionScope m_scope;
  std::optional<TimeNamespaces> m_time;
};
#endif
} // namespace

auto LinuxOpsReadBudget::check() const -> std::expected<void, Error> {
  if (stop.stop_requested()) return fail(Error::cancelled);
  if (Clock::now() >= deadline) return fail(Error::timed_out);
  return {};
}
auto LinuxOpsReadBudget::consume(std::size_t bytes)
    -> std::expected<void, Error> {
  if (auto ready = check(); !ready) return ready;
  if (bytes > remaining) return fail(Error::resource_exhausted);
  remaining -= bytes;
  return {};
}
struct LinuxOpsObservationSource::Impl {
  domain::OpsTargetBinding binding;
  std::shared_ptr<LinuxOpsProbe> probe;
  [[nodiscard]] auto validate_identity(LinuxOpsReadBudget& budget)
      -> std::expected<void, Error> {
    auto current = probe->identity(budget);
    if (!current) return std::unexpected(current.error());
    if (*current != std::get<domain::LinuxOpsIdentity>(binding.identity))
      return fail(Error::source_changed);
    return {};
  }
  [[nodiscard]] auto read_uptime(LinuxOpsReadBudget& budget)
      -> std::expected<std::optional<std::uint64_t>, Error> {
    auto time_before = probe->time_identity(budget);
    if (!time_before) return std::unexpected(time_before.error());
    auto uptime_input = probe->read(LinuxOpsInputFile::uptime, budget);
    if (!uptime_input) return std::unexpected(uptime_input.error());
    auto uptime = parse_uptime(*uptime_input);
    if (!uptime) return std::unexpected(uptime.error());
    auto offset_input = probe->read(LinuxOpsInputFile::time_offsets, budget);
    if (!offset_input) return std::unexpected(offset_input.error());
    auto zero_offset = zero_boot_offset(*offset_input);
    if (!zero_offset) return std::unexpected(zero_offset.error());
    auto time_after = probe->time_identity(budget);
    if (!time_after) return std::unexpected(time_after.error());
    const bool stable_time =
        *time_before && *time_after && **time_before == **time_after &&
        (**time_before).current != 0 &&
        (**time_before).current == (**time_before).children;
    if (!*zero_offset || !stable_time) uptime->reset();
    return uptime;
  }
};
LinuxOpsObservationSource::LinuxOpsObservationSource(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}
LinuxOpsObservationSource::~LinuxOpsObservationSource() = default;
auto LinuxOpsObservationSource::guarantees_bound_read_only_observations()
    const noexcept -> bool {
  return true;
}
auto LinuxOpsObservationSource::target_binding() const noexcept
    -> const domain::OpsTargetBinding& {
  return m_impl->binding;
}
auto LinuxOpsObservationSourceAccess::create(
    domain::OpsTargetId target, domain::OpsConfigurationRevision revision,
    std::shared_ptr<LinuxOpsProbe> probe)
    -> std::expected<std::shared_ptr<LinuxOpsObservationSource>, Error> {
  try {
    if (!valid_id(target.value()) || !valid_id(revision.value()))
      return fail(Error::invalid_result);
    if (!probe) return fail(Error::unavailable);
    LinuxOpsReadBudget budget{
        maximum_input, Clock::now() + std::chrono::seconds{5}, {}};
    auto identity = probe->identity(budget);
    if (!identity) return std::unexpected(identity.error());
    domain::OpsTargetBinding binding{std::move(target), std::move(revision),
                                     std::move(*identity)};
    if (!domain::validate_ops_target_binding(binding))
      return fail(Error::invalid_result);
    return std::shared_ptr<LinuxOpsObservationSource>{
        new LinuxOpsObservationSource{
            std::make_unique<LinuxOpsObservationSource::Impl>(
                std::move(binding), std::move(probe))}};
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
auto LinuxOpsObservationSource::create(
    domain::OpsTargetId target, domain::OpsConfigurationRevision revision)
    -> std::expected<std::shared_ptr<LinuxOpsObservationSource>, Error> {
  try {
    if (!valid_id(target.value()) || !valid_id(revision.value()))
      return fail(Error::invalid_result);
#if defined(__linux__)
    auto probe = ProcProbe::create();
    if (!probe) return std::unexpected(probe.error());
    return LinuxOpsObservationSourceAccess::create(
        std::move(target), std::move(revision), std::move(*probe));
#else
    static_cast<void>(target);
    static_cast<void>(revision);
    return fail(Error::unsupported);
#endif
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
auto LinuxOpsObservationSource::observe(
    const domain::OpsObservationRequest& request, std::stop_token stop)
    -> std::expected<domain::OpsObservation, Error> {
  try {
    if (request.operation != domain::OpsObservationOperation::linux_health)
      return fail(Error::unsupported);
    if (!domain::validate_recorded_ops_request(request))
      return fail(Error::invalid_result);
    if (request.target != m_impl->binding) return fail(Error::source_changed);
    LinuxOpsReadBudget budget{
        std::min<std::uint64_t>(request.limits.maximum_bytes, maximum_input),
        Clock::now() + request.limits.timeout, stop};
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    const auto started = timestamp();
    if (auto current = m_impl->validate_identity(budget); !current)
      return std::unexpected(current.error());
    auto memory_input = m_impl->probe->read(LinuxOpsInputFile::memory, budget);
    if (!memory_input) return std::unexpected(memory_input.error());
    auto memory = parse_memory(*memory_input);
    if (!memory) return std::unexpected(memory.error());
    auto uptime = m_impl->read_uptime(budget);
    if (!uptime) return std::unexpected(uptime.error());
    if (auto current = m_impl->validate_identity(budget); !current)
      return std::unexpected(current.error());
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    const bool complete = memory->has_value() && uptime->has_value();
    domain::OpsObservation result{
        request,
        started,
        timestamp(),
        complete ? domain::OpsObservationCompleteness::complete
                 : domain::OpsObservationCompleteness::partial,
        complete ? std::optional<std::uint64_t>{0} : std::nullopt,
        0,
        {},
        domain::LinuxHealthObservation{
            domain::OpsHealthState::unknown, *uptime, *memory, {}, {}}};
    if (!domain::validate_recorded_ops_observation(result))
      return fail(Error::invalid_result);
    return result;
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
