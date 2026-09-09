#include <aiforge/adapters/pinned_repository_root_authority.hpp>

#include <aiforge/adapters/git_exact_source_editor.hpp>
#include <aiforge/adapters/git_project_instruction_source.hpp>
#include <aiforge/adapters/git_repository_snapshot_source.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
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
    : public runtime::PinnedRepositoryReadAuthority {
 public:
  PinnedRepositoryRootAuthority(UniqueFd descriptor, FileIdentity root,
                                std::vector<RootComponent> components,
                                std::string identity,
                                std::string canonical_root,
                                GitRepositorySnapshotSource& snapshot_source,
                                GitExactSourceEditor& exact_source)
      : m_descriptor(std::move(descriptor)), m_root(root),
        m_components(std::move(components)), m_identity(std::move(identity)),
        m_canonical_root(std::move(canonical_root)),
        m_snapshot_source(snapshot_source), m_exact_source(exact_source) {}

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
      auto pinned = duplicate_pinned_root();
      if (!pinned) return std::unexpected(std::move(pinned.error()));
      if (auto traversed =
              traverse_candidate(std::move(*pinned), candidate_relative_path);
          !traversed) {
        return std::unexpected(std::move(traversed.error()));
      }
      const auto pinned_identity = descriptor_identity(m_descriptor.get());
      if (!pinned_identity || *pinned_identity != m_root) {
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

  [[nodiscard]] auto baseline() const noexcept
      -> const domain::RepositorySnapshot& override {
    if (!m_baseline) std::terminate();
    return *m_baseline;
  }

  [[nodiscard]] auto establish_baseline(
      const repository::RepositorySnapshotLimits limits)
      -> std::expected<void, repository::RepositorySnapshotError> {
    try {
      if (auto verified = verify_root(); !verified) {
        return std::unexpected(repository::RepositorySnapshotError{
            repository::RepositorySnapshotErrorCode::unstable,
            "pinned repository root changed or is unavailable", true});
      }
      auto result = m_snapshot_source.observe_pinned(
          m_descriptor.get(), m_canonical_root, limits, {});
      if (!result) return std::unexpected(std::move(result.error()));
      if (auto verified = verify_root(); !verified) {
        return std::unexpected(repository::RepositorySnapshotError{
            repository::RepositorySnapshotErrorCode::unstable,
            "pinned repository root changed or is unavailable", true});
      }
      m_baseline = std::move(*result);
      return {};
    } catch (...) {
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::internal_failure,
          "pinned repository observation failed internally"});
    }
  }

  [[nodiscard]] auto read_exact(repository::ExactSourceReadRequest request,
                                const std::stop_token stop_token) const
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override {
    try {
      if (!m_baseline || request.baseline != *m_baseline ||
          request.baseline.root.canonical_path != m_canonical_root) {
        return std::unexpected(repository::ExactSourceEditError{
            repository::ExactSourceEditErrorCode::invalid_request,
            "pinned exact-source baseline root is invalid",
            {},
            {},
            false,
            false});
      }
      if (auto verified = verify_root(); !verified) {
        return root_read_failure();
      }
      auto result = m_exact_source.read_pinned(std::move(request),
                                               m_descriptor.get(), stop_token);
      if (!result) return std::unexpected(std::move(result.error()));
      if (auto verified = verify_root(); !verified) {
        return root_read_failure();
      }
      return result;
    } catch (...) {
      return std::unexpected(repository::ExactSourceEditError{
          repository::ExactSourceEditErrorCode::internal_failure,
          "pinned exact-source read failed internally",
          {},
          {},
          false,
          false});
    }
  }

  [[nodiscard]] auto coupled_instructions(
      const GitProjectInstructionSource& source) const noexcept -> bool {
    return source.guarantees_read_only_discovery() &&
           source.is_coupled_to(m_snapshot_source);
  }

  [[nodiscard]] auto observe_current(
      repository::RepositorySnapshotLimits limits, std::stop_token stop) const
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> {
    if (!verify_root())
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::unstable,
          "pinned repository root changed or is unavailable", true});
    auto result = m_snapshot_source.observe_pinned(
        m_descriptor.get(), m_canonical_root, limits, stop);
    if (!verify_root())
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::unstable,
          "pinned repository root changed or is unavailable", true});
    return result;
  }

  [[nodiscard]] auto read_current(repository::ExactSourceReadRequest request,
                                  std::stop_token stop) const
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> {
    if (!m_baseline || request.baseline.root != m_baseline->root ||
        !verify_root())
      return root_read_failure();
    auto result = m_exact_source.read_pinned(std::move(request),
                                             m_descriptor.get(), stop);
    if (!verify_root()) return root_read_failure();
    return result;
  }

  [[nodiscard]] auto discover_current(
      GitProjectInstructionSource& source,
      repository::ProjectInstructionRequest request, std::stop_token stop) const
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> {
    if (!m_baseline || request.baseline.root != m_baseline->root ||
        !coupled_instructions(source) || !verify_root())
      return std::unexpected(repository::ProjectInstructionError{
          repository::ProjectInstructionErrorCode::unstable,
          "pinned repository root changed or is unavailable",
          {},
          true});
    auto result =
        source.discover_pinned(std::move(request), m_descriptor.get(), stop);
    if (!verify_root())
      return std::unexpected(repository::ProjectInstructionError{
          repository::ProjectInstructionErrorCode::unstable,
          "pinned repository root changed or is unavailable",
          {},
          true});
    return result;
  }

 private:
  [[nodiscard]] static auto root_read_failure()
      -> std::unexpected<repository::ExactSourceEditError> {
    return std::unexpected(repository::ExactSourceEditError{
        repository::ExactSourceEditErrorCode::concurrent_change,
        "pinned repository root changed or is unavailable",
        {},
        {},
        true});
  }

  [[nodiscard]] auto verify_root() const -> std::expected<void, Error> {
    auto reopened = reopen_verified_root();
    if (!reopened) return std::unexpected(std::move(reopened.error()));
    const auto pinned_identity = descriptor_identity(m_descriptor.get());
    if (!pinned_identity || *pinned_identity != m_root) {
      return failure(ErrorCode::path_unavailable,
                     "repository approval root changed or is unavailable");
    }
    return {};
  }

  [[nodiscard]] auto duplicate_pinned_root() const
      -> std::expected<UniqueFd, Error> {
    const auto descriptor = ::fcntl(m_descriptor.get(), F_DUPFD_CLOEXEC, 0);
    UniqueFd duplicate{descriptor};
    const auto identity = descriptor_identity(duplicate.get());
    if (!duplicate || !identity || *identity != m_root ||
        !directory_identity(*identity)) {
      return failure(ErrorCode::path_unavailable,
                     "repository approval root changed or is unavailable");
    }
    return duplicate;
  }

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
      -> std::expected<UniqueFd, Error> {
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
    return current;
  }

  UniqueFd m_descriptor;
  FileIdentity m_root;
  std::vector<RootComponent> m_components;
  std::string m_identity;
  std::string m_canonical_root;
  GitRepositorySnapshotSource& m_snapshot_source;
  GitExactSourceEditor& m_exact_source;
  std::optional<domain::RepositorySnapshot> m_baseline;
};

class PinnedRepositoryContextSource final
    : public runtime::RepositoryContextSource {
 public:
  PinnedRepositoryContextSource(
      std::shared_ptr<const PinnedRepositoryRootAuthority> root,
      GitProjectInstructionSource& instructions)
      : m_root(std::move(root)), m_instructions(instructions) {}
  [[nodiscard]] auto identity() const noexcept -> std::string_view override {
    return m_root->identity();
  }
  [[nodiscard]] auto guarantees_pinned_read_only_sources() const noexcept
      -> bool override {
    return m_root->coupled_instructions(m_instructions);
  }
  [[nodiscard]] auto observe(repository::RepositorySnapshotLimits limits,
                             std::stop_token stop)
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> override {
    try {
      return m_root->observe_current(limits, stop);
    } catch (...) {
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::internal_failure,
          "repository context observation failed internally"});
    }
  }
  [[nodiscard]] auto discover(repository::ProjectInstructionRequest request,
                              std::stop_token stop)
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override {
    try {
      return m_root->discover_current(m_instructions, std::move(request), stop);
    } catch (...) {
      return std::unexpected(repository::ProjectInstructionError{
          repository::ProjectInstructionErrorCode::internal_failure,
          "repository context discovery failed internally",
          {},
          false});
    }
  }
  [[nodiscard]] auto read(repository::ExactSourceReadRequest request,
                          std::stop_token stop)
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override {
    try {
      return m_root->read_current(std::move(request), stop);
    } catch (...) {
      return std::unexpected(repository::ExactSourceEditError{
          repository::ExactSourceEditErrorCode::internal_failure,
          "repository context read failed internally",
          {},
          {},
          false,
          false});
    }
  }

 private:
  std::shared_ptr<const PinnedRepositoryRootAuthority> m_root;
  GitProjectInstructionSource& m_instructions;
};

// Construct at its final address before any references are bound. Internal
// shared owners never alias this envelope, so there is no ownership cycle.
struct OwnedRepositoryEnvelope {
  explicit OwnedRepositoryEnvelope(GitRepositorySnapshotSource source)
      : snapshots(std::move(source)),
        exact(snapshots, GitExactSourceReadPolicy::tracked_regular_files),
        instructions(snapshots) {}
  GitRepositorySnapshotSource snapshots;
  GitExactSourceEditor exact;
  GitProjectInstructionSource instructions;
  std::shared_ptr<const runtime::PinnedRepositoryReadAuthority> authority;
  std::shared_ptr<runtime::RepositoryContextSource> context;
};
// One fixed launch graph per application, bounded across overlapping retiring
// applications as well. This is not a per-job or unbounded cleanup queue.
auto repository_graphs() -> std::atomic<unsigned>& {
  static std::atomic<unsigned> count{};
  return count;
}
constexpr unsigned maximum_repository_graphs = 2;
auto reserve_repository_graph() -> bool {
  auto count = repository_graphs().load();
  while (count < maximum_repository_graphs)
    if (repository_graphs().compare_exchange_weak(count, count + 1))
      return true;
  return false;
}
struct GraphReservation {
  bool active{true};
  ~GraphReservation() {
    if (active) --repository_graphs();
  }
};
struct RepositoryRetirement {
  std::mutex mutex;
  std::condition_variable changed;
  bool released{};
  std::unique_ptr<OwnedRepositoryEnvelope> graph;
  ~RepositoryRetirement() {
    graph.reset();
    --repository_graphs();
  }
  auto release() -> void {
    {
      const std::lock_guard lock{mutex};
      released = true;
    }
    changed.notify_all();
  }
  auto retire() -> void {
    {
      std::unique_lock lock{mutex};
      changed.wait(lock, [&] { return released; });
    }
    graph.reset(); // Outside the mutex, and before the reserved slot is
                   // released.
  }
};
struct RepositoryTicket {
  std::shared_ptr<RepositoryRetirement> retirement;
  ~RepositoryTicket() { retirement->release(); }
};
auto publish_repository_graph(
    const std::shared_ptr<RepositoryRetirement>& retirement)
    -> OwnedPinnedRepositorySources {
  auto ticket = std::make_shared<RepositoryTicket>(retirement);
  std::thread cleanup{[retirement] { retirement->retire(); }};
  try {
    cleanup.detach();
  } catch (...) {
    // Startup failure only: no aliases escaped. Wake before joining a started
    // thread so std::thread destruction cannot terminate the process.
    ticket.reset();
    if (cleanup.joinable()) cleanup.join();
    throw;
  }
  auto& owner = *retirement->graph;
  return {{ticket, &owner.snapshots},
          {ticket, &owner.exact},
          {ticket, owner.authority.get()},
          {ticket, owner.context.get()}};
}
} // namespace

auto open_pinned_repository_root_authority(
    std::filesystem::path repository_root,
    GitRepositorySnapshotSource& snapshot_source,
    GitExactSourceEditor& exact_source,
    const repository::RepositorySnapshotLimits snapshot_limits)
    -> std::expected<
        std::shared_ptr<const runtime::PinnedRepositoryReadAuthority>,
        runtime::AutomaticApprovalMatcherError> {
  try {
    if (!snapshot_source.guarantees_read_only_observation() ||
        !exact_source.guarantees_tracked_regular_files() ||
        !exact_source.guarantees_read_only_execution() ||
        !exact_source.is_coupled_to(snapshot_source)) {
      return failure(ErrorCode::invalid_configuration,
                     "pinned repository sources are invalid");
    }
    auto opened = open_root(repository_root);
    if (!opened) return std::unexpected(std::move(opened.error()));
    auto identity = root_identity(opened->components, opened->identity);
    auto authority = std::make_shared<PinnedRepositoryRootAuthority>(
        std::move(opened->descriptor), opened->identity,
        std::move(opened->components), std::move(identity),
        repository_root.generic_string(), snapshot_source, exact_source);
    if (auto established = authority->establish_baseline(snapshot_limits);
        !established) {
      const auto code =
          established.error().code ==
                  repository::RepositorySnapshotErrorCode::invalid_request
              ? ErrorCode::invalid_configuration
              : ErrorCode::path_unavailable;
      return failure(code,
                     "pinned repository baseline could not be established");
    }
    return std::shared_ptr<const runtime::PinnedRepositoryReadAuthority>{
        std::move(authority)};
  } catch (...) {
    return failure(ErrorCode::internal_failure,
                   "repository approval root pinning failed internally");
  }
}

auto make_pinned_repository_context_source(
    std::shared_ptr<const runtime::PinnedRepositoryReadAuthority> authority,
    GitProjectInstructionSource& instructions)
    -> std::expected<std::shared_ptr<runtime::RepositoryContextSource>, Error> {
  try {
    auto root = std::dynamic_pointer_cast<const PinnedRepositoryRootAuthority>(
        std::move(authority));
    if (!root || !root->coupled_instructions(instructions))
      return failure(ErrorCode::invalid_configuration,
                     "repository context sources are not coupled to the pinned "
                     "read-only root");
    return std::shared_ptr<runtime::RepositoryContextSource>{
        std::make_shared<PinnedRepositoryContextSource>(std::move(root),
                                                        instructions)};
  } catch (...) {
    return failure(ErrorCode::internal_failure,
                   "repository context source creation failed internally");
  }
}

auto open_owned_pinned_repository_sources(
    std::filesystem::path repository_root, GitRepositorySnapshotSource source,
    repository::RepositorySnapshotLimits snapshot_limits)
    -> std::expected<OwnedPinnedRepositorySources, Error> {
  try {
    if (!reserve_repository_graph())
      return failure(ErrorCode::path_unavailable,
                     "Repository source retirement capacity remains occupied");
    GraphReservation reservation;
    auto retirement = std::make_shared<RepositoryRetirement>();
    reservation.active = false;
    retirement->graph =
        std::make_unique<OwnedRepositoryEnvelope>(std::move(source));
    auto& owner = *retirement->graph;
    auto authority = open_pinned_repository_root_authority(
        std::move(repository_root), owner.snapshots, owner.exact,
        snapshot_limits);
    if (!authority) return std::unexpected(authority.error());
    owner.authority = std::move(*authority);
    auto context = make_pinned_repository_context_source(owner.authority,
                                                         owner.instructions);
    if (!context) return std::unexpected(context.error());
    owner.context = std::move(*context);
    return publish_repository_graph(retirement);
  } catch (...) {
    return failure(ErrorCode::internal_failure,
                   "Owned repository source creation failed internally");
  }
}

} // namespace aiforge::adapters
