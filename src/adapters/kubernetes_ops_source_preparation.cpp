#include <aiforge/adapters/kubernetes_ops_source_preparation.hpp>

#include "kubernetes_ops_observation_source.hpp"
#include "static_kubernetes_config.hpp"
#include "static_kubernetes_config_parser.hpp"

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#if defined(__linux__)
#include <fcntl.h>
#endif
#include <new>
#include <string_view>
#if defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <utility>
#include <vector>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
#if defined(__linux__)
using Clock = std::chrono::steady_clock;

class Descriptor final {
 public:
  explicit Descriptor(int value = -1) noexcept : m_value(value) {}
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept
      : m_value(std::exchange(other.m_value, -1)) {}
  auto operator=(Descriptor&& other) noexcept -> Descriptor& {
    if (this != &other) {
      reset();
      m_value = std::exchange(other.m_value, -1);
    }
    return *this;
  }
  ~Descriptor() { reset(); }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  [[nodiscard]] explicit operator bool() const noexcept { return m_value >= 0; }

 private:
  auto reset() noexcept -> void {
    if (m_value >= 0) static_cast<void>(::close(m_value));
    m_value = -1;
  }
  int m_value{-1};
};

auto budget(const runtime::OpsSourcePreparationRequest& request,
            std::stop_token stop) -> std::expected<void, Error> {
  if (stop.stop_requested()) return std::unexpected(Error::cancelled);
  if (Clock::now() >= request.deadline)
    return std::unexpected(Error::timed_out);
  return {};
}
#endif

auto safe_text(std::string_view value, std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         detail::is_safe_utf8_text(value) &&
         std::ranges::none_of(value, [](unsigned char byte) {
           return byte < 32 || byte == 127;
         });
}
auto namespace_text(std::string_view value) -> bool {
  const auto alphanumeric = [](char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
  };
  return safe_text(value, 63) && alphanumeric(value.front()) &&
         alphanumeric(value.back()) &&
         std::ranges::all_of(value, [&](char byte) {
           return alphanumeric(byte) || byte == '-';
         });
}

#if defined(__linux__)
auto open_error(int value) -> Error {
  if (value == EACCES || value == EPERM) return Error::permission_denied;
  if (value == ELOOP || value == ENOTDIR) return Error::source_changed;
  return Error::unavailable;
}

auto same_file(const struct stat& left, const struct stat& right) noexcept
    -> bool {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_uid == right.st_uid && left.st_mode == right.st_mode &&
         left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

struct SourceSnapshot {
  Descriptor parent;
  Descriptor file;
  std::vector<std::string> path;
  std::string bytes;
  struct stat identity{};

  [[nodiscard]] auto unchanged() const noexcept -> bool {
    struct stat descriptor_state{};
    struct stat path_state{};
    if (::fstat(file.get(), &descriptor_state) != 0 ||
        !same_file(identity, descriptor_state))
      return false;
    // Re-open every component from the filesystem root. Retaining only the
    // leaf parent would miss an ancestor that was renamed and replaced while
    // preparation held its old descriptor.
    Descriptor current{
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX syscall.
        ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!current) return false;
    for (std::size_t index{}; index + 1 < path.size(); ++index) {
      Descriptor next{
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX syscall.
          ::openat(current.get(), path[index].c_str(),
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
      if (!next) return false;
      current = std::move(next);
    }
    return ::fstatat(current.get(), path.back().c_str(), &path_state,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(path_state.st_mode) && same_file(identity, path_state);
  }
};
#endif

auto components(std::string_view path)
    -> std::expected<std::vector<std::string>, Error> {
  if (path.empty() || path.front() != '/' || path.back() == '/')
    return std::unexpected(Error::invalid_result);
  std::vector<std::string> result;
  std::size_t start{1};
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto value = path.substr(start, end - start);
    if (value.empty() || value == "." || value == "..")
      return std::unexpected(Error::invalid_result);
    result.emplace_back(value);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  if (result.empty()) return std::unexpected(Error::invalid_result);
  return result;
}

#if defined(__linux__)
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Audit stages.
auto snapshot_source(const config::StaticKubernetesTargetConfig& configured,
                     const runtime::OpsSourcePreparationRequest& request,
                     std::stop_token stop)
    -> std::expected<SourceSnapshot, Error> {
  if (auto active = budget(request, stop); !active)
    return std::unexpected(active.error());
  auto names = components(configured.config_file);
  if (!names) return std::unexpected(names.error());

  Descriptor parent{
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX syscall.
      ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (!parent) return std::unexpected(open_error(errno));
  for (std::size_t index{}; index + 1 < names->size(); ++index) {
    if (auto active = budget(request, stop); !active)
      return std::unexpected(active.error());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX openat API.
    Descriptor next{::openat(parent.get(), (*names)[index].c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!next) return std::unexpected(open_error(errno));
    parent = std::move(next);
  }

  const auto& name = names->back();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX openat API.
  Descriptor file{::openat(parent.get(), name.c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (!file) return std::unexpected(open_error(errno));
  struct stat before{};
  if (::fstat(file.get(), &before) != 0)
    return std::unexpected(open_error(errno));
  if (!S_ISREG(before.st_mode)) return std::unexpected(Error::invalid_result);
  if (before.st_uid != ::geteuid() || (before.st_mode & S_IRUSR) == 0 ||
      (before.st_mode & 0077) != 0)
    return std::unexpected(Error::permission_denied);
  if (before.st_size < 0 ||
      std::cmp_greater(before.st_size, static_kubernetes_detail::input_limit))
    return std::unexpected(Error::resource_exhausted);

  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(before.st_size));
  std::array<char, 8192> buffer{};
  while (true) {
    if (auto active = budget(request, stop); !active)
      return std::unexpected(active.error());
    const auto count = ::read(file.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(open_error(errno));
    }
    if (auto active = budget(request, stop); !active)
      return std::unexpected(active.error());
    if (count == 0) break;
    const auto size = static_cast<std::size_t>(count);
    if (size > static_kubernetes_detail::input_limit - bytes.size())
      return std::unexpected(Error::resource_exhausted);
    bytes.append(buffer.data(), size);
  }
  SourceSnapshot result{std::move(parent), std::move(file), std::move(*names),
                        std::move(bytes), before};
  if (!result.unchanged()) return std::unexpected(Error::source_changed);
  return result;
}
#endif

#if defined(__linux__)
auto parser_error(StaticKubernetesConfigFailure value) -> Error {
  switch (value) {
    case StaticKubernetesConfigFailure::unsupported: return Error::unsupported;
    case StaticKubernetesConfigFailure::resource_exhausted:
      return Error::resource_exhausted;
    case StaticKubernetesConfigFailure::cancelled: return Error::cancelled;
    case StaticKubernetesConfigFailure::internal_failure:
      return Error::internal_failure;
    case StaticKubernetesConfigFailure::invalid_input:
      return Error::invalid_result;
  }
  return Error::invalid_result;
}
#endif
} // namespace

KubernetesOpsSourcePreparationFactory::KubernetesOpsSourcePreparationFactory(
    runtime::OpsSourcePreparationIdentity identity,
    config::StaticKubernetesTargetConfig configuration)
    : m_identity(std::move(identity)),
      m_configuration(std::move(configuration)) {
}

auto KubernetesOpsSourcePreparationFactory::create(
    domain::OpsTargetId target, domain::OpsConfigurationRevision revision,
    config::StaticKubernetesTargetConfig configuration) noexcept
    -> std::expected<std::shared_ptr<KubernetesOpsSourcePreparationFactory>,
                     Error> {
  try {
    runtime::OpsSourcePreparationIdentity identity{
        std::move(target), std::move(revision),
        domain::OpsTargetKind::kubernetes};
    if (!runtime::validate_ops_source_preparation_identity(identity) ||
        !safe_text(configuration.config_file, 4096) ||
        !safe_text(configuration.context, 256) ||
        !namespace_text(configuration.name_space) ||
        !components(configuration.config_file))
      return std::unexpected(Error::invalid_result);
    return std::shared_ptr<KubernetesOpsSourcePreparationFactory>{
        new KubernetesOpsSourcePreparationFactory{std::move(identity),
                                                  std::move(configuration)}};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}

auto KubernetesOpsSourcePreparationFactory::prepare(
    const runtime::OpsSourcePreparationRequest& request, std::stop_token stop)
    -> std::expected<runtime::PreparedOpsSource, Error> {
#if !defined(__linux__)
  static_cast<void>(request);
  static_cast<void>(stop);
  return std::unexpected(Error::unsupported);
#else
  try {
    if (auto active = budget(request, stop); !active)
      return std::unexpected(active.error());
    if (!runtime::validate_ops_source_preparation_request(request) ||
        request.token.selection != m_identity)
      return std::unexpected(Error::invalid_result);
    auto snapshot = snapshot_source(m_configuration, request, stop);
    if (!snapshot) return std::unexpected(snapshot.error());
    auto parsed = StaticKubernetesConfig::parse(
        snapshot->bytes, StaticKubernetesSyntax::yaml, m_configuration.context,
        m_configuration.name_space, stop);
    if (!parsed) return std::unexpected(parser_error(parsed.error()));
    if (auto active = budget(request, stop); !active)
      return std::unexpected(active.error());
    if (!snapshot->unchanged()) return std::unexpected(Error::source_changed);
    domain::OpsTargetBinding binding{m_identity.target_id,
                                     m_identity.configuration_revision,
                                     parsed->identity()};
    auto source =
        KubernetesOpsObservationSource::create(binding, std::move(*parsed));
    if (!source) return std::unexpected(source.error());
    if (auto active = budget(request, stop); !active)
      return std::unexpected(active.error());
    if (!snapshot->unchanged()) return std::unexpected(Error::source_changed);
    if (auto active = budget(request, stop); !active)
      return std::unexpected(active.error());
    return runtime::PreparedOpsSource{std::move(*source)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
#endif
}
} // namespace aiforge::adapters
