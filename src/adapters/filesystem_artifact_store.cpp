#include <aiforge/adapters/filesystem_artifact_store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/detail/sha256.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(const storage::ArtifactStoreErrorCode code,
                           std::string message, const bool retryable = false)
    -> std::unexpected<storage::ArtifactStoreError> {
  return std::unexpected(
      storage::ArtifactStoreError{code, std::move(message), retryable});
}

[[nodiscard]] auto has_control(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char byte) {
    return byte < 0x20U || byte == 0x7FU;
  });
}

[[nodiscard]] auto digest_of(const std::span<const std::byte> content)
    -> std::string {
  detail::Sha256 digest;
  digest.update(content);
  return "sha256:" + digest.finish();
}

[[nodiscard]] auto valid_digest(const std::string_view digest) -> bool {
  if (!digest.starts_with("sha256:") || digest.size() != 71) return false;
  return std::ranges::all_of(digest.substr(7), [](const unsigned char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
  });
}

[[nodiscard]] auto blob_path(const std::filesystem::path& root,
                             const std::string_view digest)
    -> std::filesystem::path {
  const auto hex = digest.substr(7);
  return root / "sha256" / std::string{hex.substr(0, 2)} / std::string{hex};
}

#ifndef _WIN32
class Descriptor final {
 public:
  explicit Descriptor(const int descriptor = -1) : m_descriptor(descriptor) {}
  ~Descriptor() {
    if (m_descriptor >= 0) static_cast<void>(::close(m_descriptor));
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept
      : m_descriptor(std::exchange(other.m_descriptor, -1)) {}
  [[nodiscard]] auto get() const noexcept -> int { return m_descriptor; }

 private:
  int m_descriptor;
};

[[nodiscard]] auto ensure_directory(const std::filesystem::path& path)
    -> std::expected<void, storage::ArtifactStoreError> {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return failure(storage::ArtifactStoreErrorCode::io_failure,
                   "artifact directory could not be inspected", true);
  }
  if (std::filesystem::is_symlink(status)) {
    return failure(storage::ArtifactStoreErrorCode::permission_denied,
                   "artifact directory must not be a symbolic link");
  }
  if (!std::filesystem::exists(status)) {
    if (!std::filesystem::create_directories(path, error) || error) {
      return failure(storage::ArtifactStoreErrorCode::io_failure,
                     "artifact directory could not be created", true);
    }
  } else if (!std::filesystem::is_directory(status)) {
    return failure(storage::ArtifactStoreErrorCode::permission_denied,
                   "artifact path is not a directory");
  }
  if (::chmod(path.c_str(), S_IRWXU) != 0) {
    return failure(storage::ArtifactStoreErrorCode::permission_denied,
                   "artifact directory permissions could not be restricted");
  }
  struct stat attributes{};
  if (::stat(path.c_str(), &attributes) != 0 ||
      attributes.st_uid != ::geteuid()) {
    return failure(storage::ArtifactStoreErrorCode::permission_denied,
                   "artifact directory ownership is unsafe");
  }
  return {};
}

[[nodiscard]] auto inspect_directory(const std::filesystem::path& path)
    -> std::expected<void, storage::ArtifactStoreError> {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    return failure(storage::ArtifactStoreErrorCode::unavailable,
                   "artifact directory is unavailable");
  }
  struct stat attributes{};
  if (::stat(path.c_str(), &attributes) != 0 ||
      attributes.st_uid != ::geteuid() ||
      (attributes.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return failure(storage::ArtifactStoreErrorCode::permission_denied,
                   "artifact directory permissions are unsafe");
  }
  return {};
}

[[nodiscard]] auto write_all(const int descriptor,
                             const std::span<const std::byte> content,
                             const std::stop_token stop_token)
    -> std::expected<void, storage::ArtifactStoreError> {
  std::size_t offset{};
  while (offset < content.size()) {
    if (stop_token.stop_requested()) {
      return failure(storage::ArtifactStoreErrorCode::cancelled,
                     "artifact write cancelled");
    }
    const auto count =
        ::write(descriptor, content.data() + offset, content.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return failure(errno == ENOSPC
                         ? storage::ArtifactStoreErrorCode::resource_exhausted
                         : storage::ArtifactStoreErrorCode::io_failure,
                     "artifact content could not be written", true);
    }
    offset += static_cast<std::size_t>(count);
  }
  return {};
}
#endif

} // namespace

FilesystemArtifactStore::FilesystemArtifactStore(
    std::filesystem::path root, FilesystemArtifactStoreLimits limits)
    : m_root(std::move(root)), m_limits(limits) {
}

FilesystemArtifactStore::~FilesystemArtifactStore() = default;

auto FilesystemArtifactStore::open(std::filesystem::path root,
                                   const FilesystemArtifactStoreLimits limits,
                                   const FilesystemArtifactStoreOpenMode mode)
    -> std::expected<std::unique_ptr<FilesystemArtifactStore>,
                     storage::ArtifactStoreError> {
  try {
    if (!root.is_absolute() || limits.maximum_artifact_bytes == 0) {
      return failure(storage::ArtifactStoreErrorCode::invalid_request,
                     "artifact store root or limits are invalid");
    }
#ifdef _WIN32
    return failure(storage::ArtifactStoreErrorCode::unavailable,
                   "filesystem artifact storage is unavailable on Windows");
#else
    if (mode == FilesystemArtifactStoreOpenMode::create) {
      if (auto created = ensure_directory(root); !created)
        return std::unexpected(created.error());
      if (auto created = ensure_directory(root / "sha256"); !created)
        return std::unexpected(created.error());
    } else {
      if (auto inspected = inspect_directory(root); !inspected)
        return std::unexpected(inspected.error());
      if (auto inspected = inspect_directory(root / "sha256"); !inspected)
        return std::unexpected(inspected.error());
    }
    return std::unique_ptr<FilesystemArtifactStore>{
        new FilesystemArtifactStore{std::move(root), limits}};
#endif
  } catch (...) {
    return failure(storage::ArtifactStoreErrorCode::internal_failure,
                   "artifact store could not be opened");
  }
}

auto FilesystemArtifactStore::put(storage::ArtifactWrite write,
                                  const std::span<const std::byte> content,
                                  const std::stop_token stop_token)
    -> std::expected<domain::ArtifactMetadata, storage::ArtifactStoreError> {
  try {
    if (content.empty() || content.size() > m_limits.maximum_artifact_bytes ||
        write.media_type.empty() || write.media_type.size() > 255 ||
        has_control(write.media_type) ||
        (write.producing_invocation_id.has_value() ==
         write.producing_inference_id.has_value()) ||
        write.width.has_value() != write.height.has_value() ||
        (write.width && (*write.width == 0 || *write.height == 0))) {
      return failure(storage::ArtifactStoreErrorCode::invalid_request,
                     "artifact write is malformed or exceeds its limits");
    }
    if (stop_token.stop_requested()) {
      return failure(storage::ArtifactStoreErrorCode::cancelled,
                     "artifact write cancelled");
    }
    const auto digest = digest_of(content);
    const auto target = blob_path(m_root, digest);
#ifdef _WIN32
    static_cast<void>(target);
    return failure(storage::ArtifactStoreErrorCode::unavailable,
                   "filesystem artifact storage is unavailable on Windows");
#else
    if (auto created = ensure_directory(target.parent_path()); !created)
      return std::unexpected(created.error());
    domain::ArtifactMetadata metadata{
        std::move(write.artifact_id),
        write.media_type,
        static_cast<std::uint64_t>(content.size()),
        digest,
        std::move(write.producing_invocation_id),
        write.width,
        write.height,
        std::move(write.producing_inference_id)};

    static std::atomic<std::uint64_t> suffix{};
    const auto temporary =
        target.parent_path() / (".artifact-" + std::to_string(::getpid()) +
                                "-" + std::to_string(suffix.fetch_add(1)));
    Descriptor descriptor{::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR)};
    if (descriptor.get() < 0) {
      return failure(storage::ArtifactStoreErrorCode::io_failure,
                     "artifact temporary file could not be created", true);
    }
    if (auto written = write_all(descriptor.get(), content, stop_token);
        !written) {
      static_cast<void>(::unlink(temporary.c_str()));
      return std::unexpected(written.error());
    }
    if (::fsync(descriptor.get()) != 0) {
      static_cast<void>(::unlink(temporary.c_str()));
      return failure(storage::ArtifactStoreErrorCode::io_failure,
                     "artifact content could not be synchronized", true);
    }
    if (::link(temporary.c_str(), target.c_str()) != 0 && errno != EEXIST) {
      static_cast<void>(::unlink(temporary.c_str()));
      return failure(storage::ArtifactStoreErrorCode::io_failure,
                     "artifact content could not be published", true);
    }
    static_cast<void>(::unlink(temporary.c_str()));
    Descriptor parent{::open(target.parent_path().c_str(),
                             O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
    if (parent.get() < 0 || ::fsync(parent.get()) != 0) {
      return failure(storage::ArtifactStoreErrorCode::io_failure,
                     "artifact directory could not be synchronized", true);
    }
    auto verified = get(metadata, m_limits.maximum_artifact_bytes, stop_token);
    if (!verified) return std::unexpected(verified.error());
    return metadata;
#endif
  } catch (...) {
    return failure(storage::ArtifactStoreErrorCode::internal_failure,
                   "artifact write failed internally");
  }
}

auto FilesystemArtifactStore::get(const domain::ArtifactMetadata& metadata,
                                  const std::size_t maximum_bytes,
                                  const std::stop_token stop_token)
    -> std::expected<storage::ArtifactRead, storage::ArtifactStoreError> {
  try {
    if (!valid_digest(metadata.digest) || metadata.byte_size == 0 ||
        metadata.media_type.empty() || metadata.media_type.size() > 255 ||
        has_control(metadata.media_type) ||
        (metadata.producing_invocation_id.has_value() ==
         metadata.producing_inference_id.has_value()) ||
        metadata.width.has_value() != metadata.height.has_value() ||
        (metadata.width && (*metadata.width == 0 || *metadata.height == 0)) ||
        maximum_bytes == 0 || metadata.byte_size > maximum_bytes ||
        maximum_bytes > m_limits.maximum_artifact_bytes) {
      return failure(storage::ArtifactStoreErrorCode::invalid_request,
                     "artifact read metadata or limits are invalid");
    }
    if (stop_token.stop_requested()) {
      return failure(storage::ArtifactStoreErrorCode::cancelled,
                     "artifact read cancelled");
    }
#ifdef _WIN32
    return failure(storage::ArtifactStoreErrorCode::unavailable,
                   "filesystem artifact storage is unavailable on Windows");
#else
    const auto path = blob_path(m_root, metadata.digest);
    if (auto inspected = inspect_directory(path.parent_path()); !inspected)
      return std::unexpected(inspected.error());
    Descriptor descriptor{
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor.get() < 0) {
      return failure(errno == EACCES
                         ? storage::ArtifactStoreErrorCode::permission_denied
                         : storage::ArtifactStoreErrorCode::unavailable,
                     "artifact content is unavailable");
    }
    struct stat status{};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) != metadata.byte_size) {
      return failure(storage::ArtifactStoreErrorCode::permission_denied,
                     "artifact content failed identity or permission checks");
    }
    std::vector<std::byte> content(static_cast<std::size_t>(status.st_size));
    std::size_t offset{};
    while (offset < content.size()) {
      if (stop_token.stop_requested()) {
        return failure(storage::ArtifactStoreErrorCode::cancelled,
                       "artifact read cancelled");
      }
      const auto count = ::read(descriptor.get(), content.data() + offset,
                                content.size() - offset);
      if (count < 0) {
        if (errno == EINTR) continue;
        return failure(storage::ArtifactStoreErrorCode::io_failure,
                       "artifact content could not be read", true);
      }
      if (count == 0) {
        return failure(storage::ArtifactStoreErrorCode::io_failure,
                       "artifact content was truncated");
      }
      offset += static_cast<std::size_t>(count);
    }
    if (digest_of(content) != metadata.digest) {
      return failure(storage::ArtifactStoreErrorCode::io_failure,
                     "artifact content digest does not match its metadata");
    }
    return storage::ArtifactRead{metadata, std::move(content)};
#endif
  } catch (...) {
    return failure(storage::ArtifactStoreErrorCode::internal_failure,
                   "artifact read failed internally");
  }
}

auto FilesystemArtifactStore::root() const noexcept
    -> const std::filesystem::path& {
  return m_root;
}

} // namespace aiforge::adapters
