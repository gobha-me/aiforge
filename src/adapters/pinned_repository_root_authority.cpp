#include <aiforge/adapters/pinned_repository_root_authority.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <aiforge/detail/sha256.hpp>

namespace aiforge::adapters {
namespace {

using Error = runtime::AutomaticApprovalMatcherError;
using ErrorCode = runtime::AutomaticApprovalMatcherErrorCode;

[[nodiscard]] auto failure(const ErrorCode code, std::string message)
    -> std::unexpected<Error> {
  return std::unexpected(Error{code, std::move(message)});
}

class UniqueFd final {
 public:
  explicit UniqueFd(const int descriptor = -1) : m_descriptor(descriptor) {}
  UniqueFd(const UniqueFd&) = delete;
  auto operator=(const UniqueFd&) -> UniqueFd& = delete;
  UniqueFd(UniqueFd&& other) noexcept
      : m_descriptor(std::exchange(other.m_descriptor, -1)) {}
  auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
    if (this != &other) reset(std::exchange(other.m_descriptor, -1));
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] auto get() const noexcept -> int { return m_descriptor; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return m_descriptor >= 0;
  }

 private:
  auto reset(const int descriptor = -1) noexcept -> void {
    if (m_descriptor >= 0) static_cast<void>(::close(m_descriptor));
    m_descriptor = descriptor;
  }

  int m_descriptor{-1};
};

struct FileIdentity {
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint32_t mode{};
  auto operator==(const FileIdentity&) const -> bool = default;
};

struct RootComponent {
  std::string name;
  FileIdentity identity;
};

[[nodiscard]] auto descriptor_identity(const int descriptor)
    -> std::optional<FileIdentity> {
  if (descriptor < 0) return std::nullopt;
  struct stat state{};
  if (::fstat(descriptor, &state) != 0) return std::nullopt;
  return FileIdentity{static_cast<std::uint64_t>(state.st_dev),
                      static_cast<std::uint64_t>(state.st_ino),
                      static_cast<std::uint32_t>(state.st_mode)};
}

[[nodiscard]] auto open_existing(const char* path, const int flags) -> int {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX open API.
  return ::open(path, flags);
}

[[nodiscard]] auto open_existing_at(const int parent, const char* path,
                                    const int flags) -> int {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX openat API.
  return ::openat(parent, path, flags);
}

[[nodiscard]] auto path_identity(const int parent, const std::string& name)
    -> std::optional<FileIdentity> {
  struct stat state{};
  if (::fstatat(parent, name.c_str(), &state, AT_SYMLINK_NOFOLLOW) != 0) {
    return std::nullopt;
  }
  return FileIdentity{static_cast<std::uint64_t>(state.st_dev),
                      static_cast<std::uint64_t>(state.st_ino),
                      static_cast<std::uint32_t>(state.st_mode)};
}

[[nodiscard]] auto directory_identity(const FileIdentity& identity) -> bool {
  return S_ISDIR(static_cast<mode_t>(identity.mode));
}

[[nodiscard]] auto regular_identity(const FileIdentity& identity) -> bool {
  return S_ISREG(static_cast<mode_t>(identity.mode));
}

[[nodiscard]] auto valid_relative_path(const std::string_view value,
                                       const bool allow_empty) -> bool {
  if (value.empty()) return allow_empty;
  if (value.size() > 4096U || value.front() == '/' || value.back() == '/' ||
      value.find("//") != std::string_view::npos ||
      value.find('\\') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos) {
    return false;
  }
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.lexically_normal() != path || path.generic_string() != value) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& component) {
    return component.empty() || component == "." || component == ".." ||
           component == ".git";
  });
}

[[nodiscard]] auto within(const std::string_view allowed,
                          const std::string_view candidate) -> bool {
  return allowed.empty() || candidate == allowed ||
         (candidate.size() > allowed.size() && candidate.starts_with(allowed) &&
          candidate[allowed.size()] == '/');
}

auto append_integer(detail::Sha256& digest, const std::uint64_t value) -> void {
  std::array<std::byte, sizeof(value)> bytes{};
  for (std::size_t index{}; index < bytes.size(); ++index) {
    const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
    bytes[index] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
  digest.update(bytes);
}

auto append_text(detail::Sha256& digest, const std::string_view value) -> void {
  append_integer(digest, value.size());
  digest.update(std::as_bytes(std::span{value.data(), value.size()}));
}

[[nodiscard]] auto root_identity(const std::vector<RootComponent>& components,
                                 const FileIdentity& root_identity)
    -> std::string {
  detail::Sha256 digest;
  append_text(digest, "aiforge.pinned-repository-root.v1");
  append_integer(digest, root_identity.device);
  append_integer(digest, root_identity.inode);
  for (const auto& component : components) {
    append_text(digest, component.name);
    append_integer(digest, component.identity.device);
    append_integer(digest, component.identity.inode);
  }
  return "sha256:" + digest.finish();
}

struct OpenedRoot {
  UniqueFd descriptor;
  FileIdentity identity;
  std::vector<RootComponent> components;
};

[[nodiscard]] auto open_root(const std::filesystem::path& root)
    -> std::expected<OpenedRoot, Error> {
  if (root.empty() || root.native().size() > 4096U || !root.is_absolute() ||
      root.lexically_normal() != root ||
      root.native().find('\0') != std::string::npos) {
    return failure(ErrorCode::invalid_configuration,
                   "repository approval root is invalid");
  }
  UniqueFd current{
      open_existing("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  auto current_identity = descriptor_identity(current.get());
  if (!current || !current_identity || !directory_identity(*current_identity)) {
    return failure(ErrorCode::path_unavailable,
                   "repository approval root is unavailable");
  }
  std::vector<RootComponent> components;
  for (const auto& raw_component : root.relative_path()) {
    const auto component = raw_component.string();
    if (component.empty() || component == "." || component == ".." ||
        component.find('\0') != std::string::npos) {
      return failure(ErrorCode::invalid_configuration,
                     "repository approval root is invalid");
    }
    const auto listed = path_identity(current.get(), component);
    if (!listed || !directory_identity(*listed)) {
      return failure(ErrorCode::path_unavailable,
                     "repository approval root changed or is unavailable");
    }
    UniqueFd next{
        open_existing_at(current.get(), component.c_str(),
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    const auto opened = descriptor_identity(next.get());
    if (!next || !opened || !directory_identity(*opened) ||
        *opened != *listed) {
      return failure(ErrorCode::path_unavailable,
                     "repository approval root changed or is unavailable");
    }
    components.push_back({component, *opened});
    current = std::move(next);
    current_identity = opened;
  }
  return OpenedRoot{std::move(current), *current_identity,
                    std::move(components)};
}

class PinnedRepositoryRootAuthority final
    : public runtime::DescriptorRelativePathAuthority {
 public:
  PinnedRepositoryRootAuthority(UniqueFd descriptor, FileIdentity root,
                                std::vector<RootComponent> components,
                                std::string identity)
      : m_descriptor(std::move(descriptor)), m_root(root),
        m_components(std::move(components)), m_identity(std::move(identity)) {}

  [[nodiscard]] auto identity() const noexcept -> std::string_view override {
    return m_identity;
  }

  [[nodiscard]] auto contains(const std::string_view allowed_relative_path,
                              const std::string_view candidate_relative_path)
      const -> std::expected<bool, Error> override {
    try {
      if (!valid_relative_path(allowed_relative_path, true) ||
          !valid_relative_path(candidate_relative_path, false)) {
        return failure(ErrorCode::invalid_request,
                       "repository approval path is invalid");
      }
      if (!within(allowed_relative_path, candidate_relative_path)) return false;
      auto reopened = reopen_verified_root();
      if (!reopened) return std::unexpected(std::move(reopened.error()));
      if (auto traversed =
              traverse_candidate(std::move(*reopened), candidate_relative_path);
          !traversed) {
        return std::unexpected(std::move(traversed.error()));
      }
      const auto pinned = descriptor_identity(m_descriptor.get());
      if (!pinned || *pinned != m_root) {
        return failure(ErrorCode::path_unavailable,
                       "repository approval root changed or is unavailable");
      }
      auto final_root = reopen_verified_root();
      if (!final_root) {
        return std::unexpected(std::move(final_root.error()));
      }
      return true;
    } catch (...) {
      return failure(ErrorCode::internal_failure,
                     "repository approval path check failed internally");
    }
  }

 private:
  [[nodiscard]] auto reopen_verified_root() const
      -> std::expected<UniqueFd, Error> {
    UniqueFd current{
        open_existing("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    auto opened = descriptor_identity(current.get());
    if (!current || !opened || !directory_identity(*opened)) {
      return failure(ErrorCode::path_unavailable,
                     "repository approval root is unavailable");
    }
    for (const auto& expected : m_components) {
      const auto listed = path_identity(current.get(), expected.name);
      if (!listed || *listed != expected.identity ||
          !directory_identity(*listed)) {
        return failure(ErrorCode::path_unavailable,
                       "repository approval root changed or is unavailable");
      }
      UniqueFd next{
          open_existing_at(current.get(), expected.name.c_str(),
                           O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
      opened = descriptor_identity(next.get());
      if (!next || !opened || *opened != expected.identity ||
          !directory_identity(*opened)) {
        return failure(ErrorCode::path_unavailable,
                       "repository approval root changed or is unavailable");
      }
      current = std::move(next);
    }
    if (!opened || *opened != m_root) {
      return failure(ErrorCode::path_unavailable,
                     "repository approval root changed or is unavailable");
    }
    return current;
  }

  [[nodiscard]] static auto traverse_candidate(
      UniqueFd current, const std::string_view candidate_relative_path)
      -> std::expected<void, Error> {
    const std::filesystem::path candidate{candidate_relative_path};
    auto component = candidate.begin();
    while (component != candidate.end()) {
      const auto name = component->string();
      const bool last = std::next(component) == candidate.end();
      const auto listed = path_identity(current.get(), name);
      if (!listed || (!last && !directory_identity(*listed)) ||
          (last && !regular_identity(*listed))) {
        return failure(ErrorCode::path_unavailable,
                       "repository approval path is unavailable");
      }
      const int flags =
          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY);
      UniqueFd next{open_existing_at(current.get(), name.c_str(), flags)};
      const auto opened = descriptor_identity(next.get());
      if (!next || !opened || *opened != *listed ||
          (!last && !directory_identity(*opened)) ||
          (last && !regular_identity(*opened))) {
        return failure(ErrorCode::path_unavailable,
                       "repository approval path changed or is unavailable");
      }
      current = std::move(next);
      ++component;
    }
    return {};
  }

  UniqueFd m_descriptor;
  FileIdentity m_root;
  std::vector<RootComponent> m_components;
  std::string m_identity;
};

} // namespace

auto open_pinned_repository_root_authority(
    std::filesystem::path repository_root)
    -> std::expected<
        std::shared_ptr<const runtime::DescriptorRelativePathAuthority>,
        runtime::AutomaticApprovalMatcherError> {
  try {
    auto opened = open_root(repository_root);
    if (!opened) return std::unexpected(std::move(opened.error()));
    auto identity = root_identity(opened->components, opened->identity);
    auto authority = std::make_shared<PinnedRepositoryRootAuthority>(
        std::move(opened->descriptor), opened->identity,
        std::move(opened->components), std::move(identity));
    return std::shared_ptr<const runtime::DescriptorRelativePathAuthority>{
        std::move(authority)};
  } catch (...) {
    return failure(ErrorCode::internal_failure,
                   "repository approval root pinning failed internally");
  }
}

} // namespace aiforge::adapters
