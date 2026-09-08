#include <aiforge/adapters/local_file_source.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aiforge::adapters {
namespace {
using Error = domain::LocalSourceError;
using Code = domain::LocalSourceErrorCode;
using Status = std::expected<void, Error>;
auto failure(Code code, std::string message) -> std::unexpected<Error> {
  return std::unexpected(Error{code, std::move(message)});
}
auto filesystem_error() -> std::unexpected<Error> {
  switch (errno) {
    case ENOENT:
      return failure(Code::unavailable, "local source is unavailable");
    case EACCES:
    case EPERM:
      return failure(Code::permission_denied, "local source is unreadable");
    case ELOOP:
    case ENOTDIR:
      return failure(Code::unsupported_entry,
                     "local source type is unsupported");
    case EMFILE:
    case ENFILE:
    case ENOMEM:
      return failure(Code::resource_exhausted,
                     "local source resources are exhausted");
    default:
      return failure(Code::io_failure, "local source observation failed");
  }
}

class Descriptor final {
 public:
  explicit Descriptor(int value = -1) : m_value(value) {}
  ~Descriptor() {
    if (m_value >= 0) static_cast<void>(::close(m_value));
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept
      : m_value(std::exchange(other.m_value, -1)) {}
  auto operator=(Descriptor&& other) noexcept -> Descriptor& {
    if (this != &other) {
      if (m_value >= 0) static_cast<void>(::close(m_value));
      m_value = std::exchange(other.m_value, -1);
    }
    return *this;
  }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  auto release() noexcept -> int { return std::exchange(m_value, -1); }
  explicit operator bool() const noexcept { return m_value >= 0; }

 private:
  int m_value;
};

auto open_at(int parent, const char* name, int flags) -> Descriptor {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX openat.
  return Descriptor{::openat(parent, name, flags)};
}
constexpr int directory_flags = O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;

struct Budget {
  std::chrono::steady_clock::time_point started{
      std::chrono::steady_clock::now()};
  std::chrono::milliseconds timeout;
  std::stop_token stop;
  const std::atomic<bool>* revoked{};
  [[nodiscard]] auto check() const -> Status {
    if (stop.stop_requested())
      return failure(Code::cancelled, "local source operation cancelled");
    if (revoked != nullptr && revoked->load())
      return failure(Code::stale_lease, "local folder lease is revoked");
    if (std::chrono::steady_clock::now() - started >= timeout)
      return failure(Code::timed_out, "local source operation timed out");
    return {};
  }
};

struct Metadata {
  std::uint64_t inode{};
  std::uint64_t mount{};
  std::uint32_t device_major{};
  std::uint32_t device_minor{};
  std::uint32_t mode{};
  std::uint64_t size{};
  std::int64_t modified_seconds{};
  std::uint32_t modified_nanoseconds{};
  std::int64_t changed_seconds{};
  std::uint32_t changed_nanoseconds{};
  auto operator==(const Metadata&) const -> bool = default;
};
auto metadata(int descriptor, const char* name = "")
    -> std::expected<Metadata, Error> {
#if defined(__linux__) && defined(STATX_MNT_ID)
  struct statx value{};
  const int flags = AT_SYMLINK_NOFOLLOW | (*name == '\0' ? AT_EMPTY_PATH : 0);
  constexpr unsigned mask = STATX_TYPE | STATX_MODE | STATX_INO | STATX_SIZE |
                            STATX_MTIME | STATX_CTIME | STATX_MNT_ID;
  if (::statx(descriptor, name, flags, mask, &value) != 0) {
    if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)
      return failure(Code::unavailable,
                     "local physical identity is unavailable");
    return filesystem_error();
  }
  if ((value.stx_mask & mask) != mask)
    return failure(Code::unavailable, "local physical identity is unavailable");
  return Metadata{value.stx_ino,          value.stx_mnt_id,
                  value.stx_dev_major,    value.stx_dev_minor,
                  value.stx_mode,         value.stx_size,
                  value.stx_mtime.tv_sec, value.stx_mtime.tv_nsec,
                  value.stx_ctime.tv_sec, value.stx_ctime.tv_nsec};
#else
  static_cast<void>(descriptor);
  static_cast<void>(name);
  return failure(Code::unavailable, "local physical identity is unsupported");
#endif
}
auto physical_equal(const Metadata& left, const Metadata& right) -> bool {
  return left.inode == right.inode && left.mount == right.mount &&
         left.device_major == right.device_major &&
         left.device_minor == right.device_minor &&
         (left.mode & S_IFMT) == (right.mode & S_IFMT);
}
auto directory(const Metadata& value) -> bool {
  return S_ISDIR(value.mode);
}
auto regular(const Metadata& value) -> bool {
  return S_ISREG(value.mode);
}

struct NamespaceIdentity {
  std::uint64_t device{};
  std::uint64_t inode{};
  auto operator==(const NamespaceIdentity&) const -> bool = default;
};
auto namespace_identity(int descriptor)
    -> std::expected<NamespaceIdentity, Error> {
  struct stat value{};
  if (::fstat(descriptor, &value) != 0)
    return failure(Code::unavailable, "local mount namespace is unavailable");
  return NamespaceIdentity{value.st_dev, value.st_ino};
}
auto open_namespace() -> Descriptor {
  // This fixed proc capability intentionally follows the kernel namespace link.
  return open_at(AT_FDCWD, "/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
}
auto boot_identity(const Budget& budget) -> std::expected<std::string, Error> {
  if (auto valid = budget.check(); !valid)
    return std::unexpected(valid.error());
  auto descriptor = open_at(AT_FDCWD, "/proc/sys/kernel/random/boot_id",
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (!descriptor)
    return failure(Code::unavailable, "local boot identity is unavailable");
  std::array<char, 64> bytes{};
  const auto count = ::read(descriptor.get(), bytes.data(), bytes.size());
  if (count != 37 || bytes[36] != '\n')
    return failure(Code::unavailable, "local boot identity is unavailable");
  for (std::size_t index = 0; index < 36; ++index) {
    const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
    const char character = bytes[index];
    const bool hex = (character >= 'a' && character <= 'f') ||
                     (character >= '0' && character <= '9');
    if (hyphen ? character != '-' : !hex)
      return failure(Code::unavailable, "local boot identity is unavailable");
  }
  if (auto valid = budget.check(); !valid)
    return std::unexpected(valid.error());
  return std::string{bytes.data(), 36};
}
auto hash_field(detail::Sha256& hash, std::string_view value) -> void {
  const auto size = std::to_string(value.size());
  hash.update(std::as_bytes(std::span{size.data(), size.size()}));
  constexpr std::array separator{std::byte{':'}};
  hash.update(separator);
  hash.update(std::as_bytes(std::span{value.data(), value.size()}));
}
auto hash_number(detail::Sha256& hash, std::uint64_t value) -> void {
  hash_field(hash, std::to_string(value));
}

struct RootComponent {
  std::string name;
  Descriptor descriptor;
  Metadata state;
};
auto root_components(const std::filesystem::path& path,
                     const domain::LocalSourceLimits& limits,
                     const Budget& budget)
    -> std::expected<std::vector<RootComponent>, Error> {
  const auto& raw = path.native();
  if (raw.empty() || raw.size() > limits.maximum_path_bytes ||
      raw.find('\0') != std::string::npos || !path.is_absolute() ||
      path.lexically_normal() != path)
    return failure(Code::invalid_request, "local folder path is invalid");
  auto descriptor = open_at(AT_FDCWD, "/", directory_flags);
  if (!descriptor) return filesystem_error();
  auto state = metadata(descriptor.get());
  if (!state) return std::unexpected(state.error());
  std::vector<RootComponent> components;
  components.push_back({"", std::move(descriptor), *state});
  for (const auto& component : path.relative_path()) {
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    const auto& name = component.native();
    if (name.empty() || name == "." || name == ".." || name.size() > 255 ||
        components.size() > limits.maximum_depth)
      return failure(Code::invalid_request, "local folder path is invalid");
    const auto parent = components.back().descriptor.get();
    const auto before = metadata(parent, name.c_str());
    if (!before) return std::unexpected(before.error());
    if (!directory(*before))
      return failure(Code::unsupported_entry,
                     "local folder is not a directory");
    auto next = open_at(parent, name.c_str(), directory_flags);
    if (!next) return filesystem_error();
    auto opened = metadata(next.get());
    if (!opened) return std::unexpected(opened.error());
    if (!physical_equal(*before, *opened))
      return failure(Code::concurrent_change,
                     "local folder changed during grant");
    components.push_back({name, std::move(next), *opened});
  }
  return components;
}
auto physical_binding(std::string_view boot,
                      const NamespaceIdentity& mount_namespace,
                      const std::vector<RootComponent>& components)
    -> domain::LocalRootIdentity {
  detail::Sha256 hash;
  hash_field(hash, "aiforge.local-root.linux-boot-mount.v1");
  hash_field(hash, boot);
  hash_number(hash, mount_namespace.device);
  hash_number(hash, mount_namespace.inode);
  hash_number(hash, components.size());
  for (const auto& component : components) {
    hash_number(hash, component.state.mount);
    hash_number(hash, component.state.device_major);
    hash_number(hash, component.state.device_minor);
    hash_number(hash, component.state.inode);
  }
  return {1, hash.finish()};
}
auto narrowed(const domain::LocalSourceLimits& request,
              const domain::LocalSourceLimits& grant) -> bool {
  const std::array<std::pair<std::uint64_t, std::uint64_t>, 10> bounds{
      {{request.maximum_roots, grant.maximum_roots},
       {request.maximum_selected_files, grant.maximum_selected_files},
       {request.maximum_file_bytes, grant.maximum_file_bytes},
       {request.maximum_total_bytes, grant.maximum_total_bytes},
       {request.maximum_path_bytes, grant.maximum_path_bytes},
       {request.maximum_depth, grant.maximum_depth},
       {request.maximum_list_entries, grant.maximum_list_entries},
       {request.maximum_scanned_entries, grant.maximum_scanned_entries},
       {request.maximum_listing_bytes, grant.maximum_listing_bytes},
       {request.maximum_preview_bytes, grant.maximum_preview_bytes}}};
  return request.timeout <= grant.timeout &&
         std::ranges::all_of(bounds, [](const auto& pair) {
           return pair.first <= pair.second;
         });
}

struct OpenedDirectory {
  Descriptor descriptor;
  std::vector<Metadata> ancestry;
  Metadata state;
};
auto same_ancestry(const OpenedDirectory& left, const OpenedDirectory& right)
    -> bool {
  return left.ancestry.size() == right.ancestry.size() &&
         std::ranges::equal(left.ancestry, right.ancestry, physical_equal);
}
auto safe_name(std::string_view value) -> bool {
  return detail::is_safe_utf8_text(value) &&
         value.find_first_of("\r\n\t") == std::string_view::npos;
}
auto entry_kind(const Metadata& state) -> runtime::LocalEntryKind {
  if (regular(state)) return runtime::LocalEntryKind::regular_file;
  if (directory(state)) return runtime::LocalEntryKind::directory;
  if (S_ISLNK(state.mode)) return runtime::LocalEntryKind::symbolic_link;
  return runtime::LocalEntryKind::unsupported;
}
auto incomplete_suffix(std::string_view text) -> bool {
  const auto first = static_cast<unsigned char>(text.front());
  std::size_t needed{};
  if (first >= 0xc2 && first <= 0xdf)
    needed = 2;
  else if (first >= 0xe0 && first <= 0xef)
    needed = 3;
  else if (first >= 0xf0 && first <= 0xf4)
    needed = 4;
  if (needed == 0 || text.size() >= needed) return false;
  return std::ranges::all_of(text.substr(1), [](unsigned char value) {
    return (value & 0xc0U) == 0x80U;
  });
}
auto supported_prefix(std::string_view text)
    -> std::expected<std::size_t, Error> {
  std::size_t offset{};
  while (offset < text.size()) {
    auto decoded = detail::decode_utf8_codepoint(text, offset);
    if (!decoded) {
      // Bounded lookahead must subsequently validate the full codepoint.
      if (incomplete_suffix(text.substr(offset))) return offset;
      return failure(Code::invalid_text, "local preview is not supported text");
    }
    if (detail::is_unsafe_text_control(decoded->value))
      return failure(Code::invalid_text, "local preview is not supported text");
    offset += decoded->bytes;
  }
  return offset;
}
} // namespace

struct LocalFileSource::Impl {
  domain::SessionId session;
  std::uint64_t generation;
  domain::LocalSourceLimits limits;
  domain::LocalRootIdentity identity;
  Descriptor mount_namespace;
  NamespaceIdentity namespace_state;
  std::vector<RootComponent> root;
  std::atomic<bool> revoked{};

  [[nodiscard]] auto authorize(const runtime::LocalSourceRequestToken& request,
                               const domain::LocalSourceLimits& requested,
                               const Budget& budget) const -> Status {
    if (auto valid = budget.check(); !valid) return valid;
    if (request.session_id != session || request.root != identity ||
        request.lease_generation != generation)
      return failure(Code::stale_lease,
                     "local folder lease does not match request");
    if (!narrowed(requested, limits))
      return failure(Code::invalid_request,
                     "local request exceeds folder grant limits");
    return {};
  }
  [[nodiscard]] auto verified_root(const Budget& budget) const
      -> std::expected<Descriptor, Error> {
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    auto current_namespace = open_namespace();
    if (!current_namespace)
      return failure(Code::unavailable, "local mount namespace is unavailable");
    auto current_identity = namespace_identity(current_namespace.get());
    if (!current_identity || *current_identity != namespace_state)
      return failure(Code::source_mismatch, "local mount namespace changed");
    auto current = open_at(AT_FDCWD, "/", directory_flags);
    if (!current) return filesystem_error();
    for (const auto& component : root) {
      if (auto valid = budget.check(); !valid)
        return std::unexpected(valid.error());
      if (!component.name.empty()) {
        auto next =
            open_at(current.get(), component.name.c_str(), directory_flags);
        if (!next) return filesystem_error();
        current = std::move(next);
      }
      auto observed = metadata(current.get());
      auto pinned = metadata(component.descriptor.get());
      if (!observed || !pinned || !physical_equal(component.state, *observed) ||
          !physical_equal(component.state, *pinned))
        return failure(Code::source_mismatch,
                       "local folder physical identity changed");
    }
    return current;
  }
  [[nodiscard]] auto open_directory(std::string_view relative,
                                    const Budget& budget) const
      -> std::expected<OpenedDirectory, Error> {
    auto current = verified_root(budget);
    if (!current) return std::unexpected(current.error());
    OpenedDirectory result{std::move(*current), {}, {}};
    std::size_t begin{};
    while (begin < relative.size()) {
      if (auto valid = budget.check(); !valid)
        return std::unexpected(valid.error());
      const auto end = relative.find('/', begin);
      const auto name = std::string{relative.substr(
          begin, end == std::string_view::npos ? end : end - begin)};
      auto before = metadata(result.descriptor.get(), name.c_str());
      if (!before) return std::unexpected(before.error());
      if (!directory(*before))
        return failure(Code::unsupported_entry,
                       "local parent is not a directory");
      auto next =
          open_at(result.descriptor.get(), name.c_str(), directory_flags);
      if (!next) return filesystem_error();
      auto opened = metadata(next.get());
      if (!opened || !physical_equal(*before, *opened))
        return failure(Code::concurrent_change,
                       "local directory changed during observation");
      result.ancestry.push_back(*opened);
      result.descriptor = std::move(next);
      if (end == std::string_view::npos) break;
      begin = end + 1;
    }
    auto state = metadata(result.descriptor.get());
    if (!state) return std::unexpected(state.error());
    result.state = *state;
    return result;
  }
  [[nodiscard]] auto read_text(const runtime::LocalPreviewRequest& request,
                               const Budget& budget) const
      -> std::expected<runtime::LocalPreviewResult, Error>;
  [[nodiscard]] auto read_directory(const runtime::LocalListRequest& request,
                                    const Budget& budget) const
      -> std::expected<runtime::LocalListResult, Error>;
};

namespace {
auto open_regular_file(int parent, const char* name, const Metadata& before)
    -> std::expected<Descriptor, Error> {
  // Pin without opening a device/FIFO even if the pathname races its stat.
  auto pinned = open_at(parent, name, O_PATH | O_CLOEXEC | O_NOFOLLOW);
  if (!pinned) return filesystem_error();
  auto pinned_state = metadata(pinned.get());
  if (!pinned_state || !regular(*pinned_state) || *pinned_state != before)
    return failure(Code::concurrent_change,
                   "local file changed before pinning");
  // This is the kernel link for an owned, validated regular-file descriptor.
  // Do not reopen the user-supplied pathname with read-open flags.
  const auto descriptor_path = "/proc/self/fd/" + std::to_string(pinned.get());
  auto file = open_at(AT_FDCWD, descriptor_path.c_str(),
                      O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (!file) return filesystem_error();
  auto opened = metadata(file.get());
  if (!opened || !regular(*opened) || *pinned_state != *opened)
    return failure(Code::concurrent_change,
                   "local pinned file changed before read");
  return file;
}
auto read_bytes(int descriptor, std::uint64_t size, std::uint64_t maximum,
                const Budget& budget) -> std::expected<std::string, Error> {
  const auto target = static_cast<std::size_t>(std::min(size, maximum));
  std::string result;
  result.reserve(target);
  std::array<char, 4096> buffer{};
  while (result.size() < target) {
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    const auto amount = std::min(buffer.size(), target - result.size());
    const auto count = ::read(descriptor, buffer.data(), amount);
    if (count < 0) {
      if (errno == EINTR) continue;
      return filesystem_error();
    }
    if (count == 0)
      return failure(Code::concurrent_change, "local file changed during read");
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (size <= maximum) {
    // Verify EOF even for size-zero pseudo-files and files that grew.
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    char extra{};
    const auto count = ::read(descriptor, &extra, 1);
    if (count < 0) return filesystem_error();
    if (count != 0)
      return failure(Code::concurrent_change, "local file changed during read");
  }
  return result;
}
auto preview_text(std::string bytes, std::uint64_t file_size,
                  std::uint64_t maximum) -> std::expected<std::string, Error> {
  // Up to three lookahead bytes complete a codepoint cut by the display limit.
  const auto visible =
      std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(maximum));
  auto prefix = supported_prefix(std::string_view{bytes}.substr(0, visible));
  if (!prefix) return std::unexpected(prefix.error());
  if (*prefix < visible) {
    const auto full = detail::decode_utf8_codepoint(bytes, *prefix);
    if (!full || detail::is_unsafe_text_control(full->value))
      return failure(Code::invalid_text, "local preview is not supported text");
  }
  if (file_size <= maximum && *prefix != bytes.size())
    return failure(Code::invalid_text, "local file has incomplete text");
  bytes.resize(*prefix);
  if (bytes.empty() && file_size != 0)
    return failure(Code::resource_exhausted,
                   "local preview cannot fit a complete character");
  return bytes;
}
} // namespace

auto LocalFileSource::Impl::read_text(
    const runtime::LocalPreviewRequest& request, const Budget& budget) const
    -> std::expected<runtime::LocalPreviewResult, Error> {
  const auto slash = request.relative_path.rfind('/');
  const auto parent_path = slash == std::string::npos
                               ? std::string{}
                               : request.relative_path.substr(0, slash);
  const auto name = slash == std::string::npos
                        ? request.relative_path
                        : request.relative_path.substr(slash + 1);
  auto parent = open_directory(parent_path, budget);
  if (!parent) return std::unexpected(parent.error());
  auto before = metadata(parent->descriptor.get(), name.c_str());
  if (!before) return std::unexpected(before.error());
  if (!regular(*before))
    return failure(Code::unsupported_entry, "local file is not regular");
  if (request.mode == runtime::LocalPreviewMode::exact &&
      before->size > request.limits.maximum_file_bytes)
    return failure(Code::unavailable,
                   "local file exceeds exact evidence limit");
  auto file =
      open_regular_file(parent->descriptor.get(), name.c_str(), *before);
  if (!file) return std::unexpected(file.error());
  auto opened = metadata(file->get());
  if (!opened || *before != *opened)
    return failure(Code::concurrent_change,
                   "local pinned file changed before read");
  const auto maximum = request.mode == runtime::LocalPreviewMode::exact
                           ? request.limits.maximum_file_bytes
                           : request.limits.maximum_preview_bytes;
  const auto read_limit =
      request.mode == runtime::LocalPreviewMode::exact ? maximum : maximum + 3;
  auto bytes = read_bytes(file->get(), opened->size, read_limit, budget);
  if (!bytes) return std::unexpected(bytes.error());
  auto text = preview_text(std::move(*bytes), opened->size, maximum);
  if (!text) return std::unexpected(text.error());
  auto after = metadata(file->get());
  auto rebound = open_directory(parent_path, budget);
  if (!rebound) return std::unexpected(rebound.error());
  auto located = metadata(rebound->descriptor.get(), name.c_str());
  if (!after || !located || *opened != *after || *opened != *located ||
      !same_ancestry(*parent, *rebound))
    return failure(Code::concurrent_change, "local file changed during read");
  runtime::LocalPreviewResult result{request.token,
                                     request.relative_path,
                                     opened->size,
                                     std::move(*text),
                                     runtime::LocalPreviewState::prefix,
                                     std::nullopt};
  if (result.text.size() == opened->size) {
    detail::Sha256 hash;
    hash.update(
        std::as_bytes(std::span{result.text.data(), result.text.size()}));
    result.state = runtime::LocalPreviewState::complete;
    result.source =
        domain::LocalSourceIdentity{identity,
                                    request.relative_path,
                                    {"sha256", hash.finish(), opened->size}};
  }
  if (auto valid = runtime::validate_local_preview_result(request, result);
      !valid)
    return std::unexpected(valid.error());
  if (auto valid = budget.check(); !valid)
    return std::unexpected(valid.error());
  return result;
}

namespace {
struct DirectoryCloser {
  auto operator()(DIR* value) const noexcept -> void {
    if (value != nullptr) static_cast<void>(::closedir(value));
  }
};
auto append_name(runtime::LocalListResult& result,
                 const runtime::LocalListRequest& request, int descriptor,
                 std::string_view name, std::size_t& bytes)
    -> std::expected<bool, Error> {
  ++result.scanned_entries;
  if (!safe_name(name)) {
    ++result.skipped_unsupported_entries;
    return true;
  }
  if (name.find(request.filename_filter) == std::string_view::npos) {
    ++result.skipped_filtered_entries;
    return true;
  }
  auto path = request.directory.empty()
                  ? std::string{name}
                  : request.directory + "/" + std::string{name};
  if (!domain::validate_local_relative_path(path, false, request.limits)) {
    ++result.skipped_unsupported_entries;
    return true;
  }
  if (result.entries.size() == request.limits.maximum_list_entries ||
      path.size() > request.limits.maximum_listing_bytes - bytes) {
    ++result.skipped_capacity_entries;
    return false;
  }
  const auto raw = std::string{name};
  auto state = metadata(descriptor, raw.c_str());
  if (!state) return std::unexpected(state.error());
  result.entries.push_back({std::move(path), entry_kind(*state)});
  bytes += result.entries.back().relative_path.size();
  return true;
}
auto next_name(DIR* stream)
    -> std::expected<std::optional<std::string>, Error> {
  errno = 0;
  // NOLINTNEXTLINE(concurrency-mt-unsafe) -- Each operation owns its DIR.
  const auto* entry = ::readdir(stream);
  if (entry == nullptr) {
    if (errno != 0) return filesystem_error();
    return std::nullopt;
  }
  const auto bytes = std::span{entry->d_name};
  const auto end = std::ranges::find(bytes, '\0');
  return std::string{bytes.begin(), end};
}
auto scan_directory(DIR* stream, int descriptor,
                    const runtime::LocalListRequest& request,
                    const Budget& budget)
    -> std::expected<runtime::LocalListResult, Error> {
  runtime::LocalListResult result{request.token,
                                  request.directory,
                                  {},
                                  0,
                                  runtime::LocalListingState::partial};
  std::size_t bytes{};
  while (result.scanned_entries < request.limits.maximum_scanned_entries) {
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    auto name = next_name(stream);
    if (!name) return std::unexpected(name.error());
    if (!*name) {
      result.state = runtime::LocalListingState::complete;
      break;
    }
    if (**name == "." || **name == "..") continue;
    if ((**name).size() > 255) {
      ++result.scanned_entries;
      ++result.skipped_unsupported_entries;
      continue;
    }
    auto appended = append_name(result, request, descriptor, **name, bytes);
    if (!appended) return std::unexpected(appended.error());
    if (!*appended) break;
  }
  return result;
}
} // namespace

auto LocalFileSource::Impl::read_directory(
    const runtime::LocalListRequest& request, const Budget& budget) const
    -> std::expected<runtime::LocalListResult, Error> {
  auto opened = open_directory(request.directory, budget);
  if (!opened) return std::unexpected(opened.error());
  auto enumeration = open_at(opened->descriptor.get(), ".", directory_flags);
  if (!enumeration) return filesystem_error();
  std::unique_ptr<DIR, DirectoryCloser> stream{::fdopendir(enumeration.get())};
  if (!stream) return filesystem_error();
  static_cast<void>(enumeration.release());
  auto result =
      scan_directory(stream.get(), opened->descriptor.get(), request, budget);
  if (!result) return std::unexpected(result.error());
  auto after = metadata(opened->descriptor.get());
  auto rebound = open_directory(request.directory, budget);
  if (!rebound) return std::unexpected(rebound.error());
  if (!after || *after != opened->state || rebound->state != opened->state ||
      !same_ancestry(*opened, *rebound))
    return failure(Code::concurrent_change,
                   "local directory changed during listing");
  if (auto valid = budget.check(); !valid)
    return std::unexpected(valid.error());
  if (auto valid = runtime::validate_local_list_result(request, *result);
      !valid)
    return std::unexpected(valid.error());
  if (auto valid = budget.check(); !valid)
    return std::unexpected(valid.error());
  return result;
}

LocalFileSource::LocalFileSource(std::unique_ptr<Impl> implementation)
    : m_impl(std::move(implementation)) {
}
LocalFileSource::~LocalFileSource() = default;
auto LocalFileSource::root_identity() const noexcept
    -> const domain::LocalRootIdentity& {
  return m_impl->identity;
}
auto LocalFileSource::revoke() noexcept -> void {
  m_impl->revoked.store(true);
}
auto LocalFileSource::guarantees_pinned_read_only_sources() const noexcept
    -> bool {
  return true;
}

auto grant_local_folder(domain::SessionId session_id,
                        std::filesystem::path absolute_root,
                        std::uint64_t lease_generation,
                        domain::LocalSourceLimits limits, std::stop_token stop)
    -> std::expected<std::shared_ptr<LocalFileSource>, Error> {
  try {
    if (auto valid = domain::validate_local_source_limits(limits); !valid)
      return std::unexpected(valid.error());
    if (lease_generation == 0)
      return failure(Code::invalid_request,
                     "local lease generation is invalid");
    Budget budget{std::chrono::steady_clock::now(), limits.timeout, stop,
                  nullptr};
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    auto boot = boot_identity(budget);
    if (!boot) return std::unexpected(boot.error());
    auto mount_namespace = open_namespace();
    if (!mount_namespace)
      return failure(Code::unavailable, "local mount namespace is unavailable");
    auto namespace_state = namespace_identity(mount_namespace.get());
    if (!namespace_state) return std::unexpected(namespace_state.error());
    auto components = root_components(absolute_root, limits, budget);
    if (!components) return std::unexpected(components.error());
    auto identity = physical_binding(*boot, *namespace_state, *components);
    auto implementation = std::make_unique<LocalFileSource::Impl>(
        std::move(session_id), lease_generation, limits, std::move(identity),
        std::move(mount_namespace), *namespace_state, std::move(*components));
    if (auto verified = implementation->verified_root(budget); !verified)
      return std::unexpected(verified.error());
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    return std::shared_ptr<LocalFileSource>{
        new LocalFileSource{std::move(implementation)}};
  } catch (...) {
    return failure(Code::internal_failure, "local folder grant failed");
  }
}
auto LocalFileSource::list(runtime::LocalListRequest request,
                           std::stop_token stop)
    -> std::expected<runtime::LocalListResult, Error> {
  try {
    if (auto valid = runtime::validate_local_list_request(request); !valid)
      return std::unexpected(valid.error());
    const Budget budget{std::chrono::steady_clock::now(),
                        request.limits.timeout, stop, &m_impl->revoked};
    if (auto valid = m_impl->authorize(request.token, request.limits, budget);
        !valid)
      return std::unexpected(valid.error());
    return m_impl->read_directory(request, budget);
  } catch (...) {
    return failure(Code::internal_failure, "local directory listing failed");
  }
}
auto LocalFileSource::preview(runtime::LocalPreviewRequest request,
                              std::stop_token stop)
    -> std::expected<runtime::LocalPreviewResult, Error> {
  try {
    if (auto valid = runtime::validate_local_preview_request(request); !valid)
      return std::unexpected(valid.error());
    const Budget budget{std::chrono::steady_clock::now(),
                        request.limits.timeout, stop, &m_impl->revoked};
    if (auto valid = m_impl->authorize(request.token, request.limits, budget);
        !valid)
      return std::unexpected(valid.error());
    return m_impl->read_text(request, budget);
  } catch (...) {
    return failure(Code::internal_failure, "local file preview failed");
  }
}
auto LocalFileSource::revalidate(runtime::LocalRevalidateRequest request,
                                 std::stop_token stop)
    -> std::expected<runtime::LocalReadResult, Error> {
  try {
    if (auto valid = runtime::validate_local_revalidate_request(request);
        !valid)
      return std::unexpected(valid.error());
    const Budget budget{std::chrono::steady_clock::now(),
                        request.limits.timeout, stop, &m_impl->revoked};
    if (auto valid = m_impl->authorize(request.token, request.limits, budget);
        !valid)
      return std::unexpected(valid.error());
    auto read =
        m_impl->read_text({request.token, request.expected_source.relative_path,
                           runtime::LocalPreviewMode::exact, request.limits},
                          budget);
    if (!read) return std::unexpected(read.error());
    if (!read->source)
      return failure(Code::invalid_result,
                     "local exact source proof is missing");
    runtime::LocalReadResult result{request.token, std::move(*read->source),
                                    std::move(read->text)};
    if (auto valid = runtime::validate_local_read_result(request, result);
        !valid)
      return std::unexpected(valid.error());
    if (auto valid = budget.check(); !valid)
      return std::unexpected(valid.error());
    return result;
  } catch (...) {
    return failure(Code::internal_failure, "local source recovery failed");
  }
}
} // namespace aiforge::adapters
