#include "secure_artifact_export.hpp"

#include <cerrno>
#include <string>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aiforge::adapters::detail {
namespace {

[[nodiscard]] auto failure(cli::CommandFailureKind kind, std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

#ifndef _WIN32
class Descriptor final {
 public:
  explicit Descriptor(const int value = -1) : m_value{value} {}
  ~Descriptor() {
    if (m_value >= 0) static_cast<void>(::close(m_value));
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept
      : m_value{std::exchange(other.m_value, -1)} {}
  auto operator=(Descriptor&& other) noexcept -> Descriptor& {
    if (this == &other) return *this;
    if (m_value >= 0) static_cast<void>(::close(m_value));
    m_value = std::exchange(other.m_value, -1);
    return *this;
  }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  auto close() noexcept -> bool {
    const auto value = std::exchange(m_value, -1);
    return value < 0 || ::close(value) == 0;
  }

 private:
  int m_value;
};

[[nodiscard]] auto valid_component(const std::filesystem::path& component)
    -> bool {
  return !component.empty() && component != "." && component != ".." &&
         component != "/";
}

[[nodiscard]] auto open_parent(const std::filesystem::path& path)
    -> std::expected<Descriptor, cli::CommandFailure> {
  Descriptor current{
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX open API.
      ::open(path.is_absolute() ? "/" : ".",
             O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (current.get() < 0) {
    return failure(cli::CommandFailureKind::runtime,
                   "output directory could not be opened");
  }
  for (const auto& component : path.parent_path()) {
    if (component == "/" || component == ".") continue;
    if (!valid_component(component)) {
      return failure(cli::CommandFailureKind::usage,
                     "output path contains an unsafe component");
    }
    // clang-format off
    const auto next_value = ::openat(current.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW); // NOLINT(cppcoreguidelines-pro-type-vararg) -- POSIX component traversal API.
    // clang-format on
    Descriptor next{next_value};
    if (next.get() < 0) {
      return failure(cli::CommandFailureKind::usage,
                     "output directory is unavailable or unsafe");
    }
    current = std::move(next);
  }
  return current;
}

[[nodiscard]] auto same_file(const struct stat& left, const struct stat& right)
    -> bool {
  return S_ISREG(left.st_mode) && S_ISREG(right.st_mode) &&
         left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
#endif

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Secure stages.
auto secure_export_bytes(const std::span<const std::byte> content,
                         const std::filesystem::path& path,
                         const std::stop_token stop_token,
                         SecureExportTestHooks hooks)
    -> std::expected<void, cli::CommandFailure> {
#ifdef _WIN32
  static_cast<void>(content);
  static_cast<void>(path);
  static_cast<void>(stop_token);
  static_cast<void>(hooks);
  return failure(cli::CommandFailureKind::runtime,
                 "secure artifact export is unavailable on Windows");
#else
  if (path.empty() || !valid_component(path.filename())) {
    return failure(cli::CommandFailureKind::usage,
                   "output path must name a safe file");
  }
  auto parent = open_parent(path);
  if (!parent) return std::unexpected(std::move(parent.error()));
  const auto filename = path.filename();
#ifndef O_TMPFILE
  return failure(cli::CommandFailureKind::runtime,
                 "secure temporary output is unavailable");
#else
  Descriptor output{
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX openat API.
      ::openat(parent->get(), ".", O_TMPFILE | O_RDWR | O_CLOEXEC,
               S_IRUSR | S_IWUSR)};
  if (output.get() < 0) {
    return failure(cli::CommandFailureKind::runtime,
                   "secure temporary output could not be created");
  }

  const auto abandon =
      [&](const cli::CommandFailureKind kind,
          std::string message) -> std::expected<void, cli::CommandFailure> {
    if (hooks.before_cleanup) hooks.before_cleanup(output.get());
    if (!output.close()) {
      return failure(cli::CommandFailureKind::runtime,
                     "temporary output cleanup failed");
    }
    return failure(kind, std::move(message));
  };

  if (!hooks.write) {
    hooks.write = [](const int descriptor,
                     const std::span<const std::byte> remaining)
        -> std::expected<std::size_t, int> {
      const auto count =
          ::write(descriptor, remaining.data(), remaining.size());
      if (count < 0) return std::unexpected(errno);
      return static_cast<std::size_t>(count);
    };
  }
  std::size_t offset{};
  while (offset < content.size()) {
    if (stop_token.stop_requested()) {
      return abandon(cli::CommandFailureKind::cancelled, "export cancelled");
    }
    auto count = hooks.write(output.get(), content.subspan(offset));
    if (!count) {
      if (count.error() == EINTR) continue;
      return abandon(cli::CommandFailureKind::runtime,
                     "output file could not be written");
    }
    if (*count == 0 || *count > content.size() - offset) {
      return abandon(cli::CommandFailureKind::runtime,
                     "output file could not be written");
    }
    offset += *count;
  }
  if (stop_token.stop_requested()) {
    return abandon(cli::CommandFailureKind::cancelled, "export cancelled");
  }
  if (::fsync(output.get()) != 0) {
    return abandon(cli::CommandFailureKind::runtime,
                   "output file could not be synchronized");
  }
  struct stat owned{};
  if (::fstat(output.get(), &owned) != 0 || !S_ISREG(owned.st_mode)) {
    return abandon(cli::CommandFailureKind::runtime,
                   "temporary output identity could not be verified");
  }
  if (hooks.before_publish) {
    hooks.before_publish(parent->get(), filename.native());
  }
  if (stop_token.stop_requested()) {
    return abandon(cli::CommandFailureKind::cancelled, "export cancelled");
  }
  if (::linkat(output.get(), "", parent->get(), filename.c_str(),
               AT_EMPTY_PATH) != 0) {
    const auto exists = errno == EEXIST;
    return abandon(exists ? cli::CommandFailureKind::usage
                          : cli::CommandFailureKind::runtime,
                   exists ? "output path already exists"
                          : "output file could not be published");
  }

  if (hooks.after_publish) {
    hooks.after_publish(parent->get(), filename.native());
  }
  struct stat visible{};
  if (::fstatat(parent->get(), filename.c_str(), &visible,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !same_file(owned, visible) || ::fsync(parent->get()) != 0 ||
      ::fstatat(parent->get(), filename.c_str(), &visible,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !same_file(owned, visible)) {
    static_cast<void>(output.close());
    return failure(cli::CommandFailureKind::runtime,
                   "published output identity could not be verified");
  }
  static_cast<void>(output.close());
  return {};
#endif
#endif
}

} // namespace aiforge::adapters::detail
