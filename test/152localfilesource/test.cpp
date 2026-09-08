#include <catch2/catch_test_macros.hpp>

#include <aiforge/adapters/local_file_source.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <span>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace aiforge;
namespace fs = std::filesystem;
using Code = domain::LocalSourceErrorCode;

class Directory final {
 public:
  Directory() {
    std::array pattern{'/', 't', 'm', 'p', '/', 'a', 'i', 'f',
                       'o', 'r', 'g', 'e', '-', '2', '3', '2',
                       '-', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    path = created;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  Directory(const Directory&) = delete;
  auto operator=(const Directory&) -> Directory& = delete;
  fs::path path;
};

auto write(const fs::path& path, std::string_view text) -> void {
  std::ofstream stream(path, std::ios::binary);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.close();
  REQUIRE(stream.good());
}
auto session() -> domain::SessionId {
  return domain::SessionId::from("session").value();
}
auto grant(const fs::path& path, std::uint64_t generation = 1,
           domain::LocalSourceLimits limits = {})
    -> std::shared_ptr<adapters::LocalFileSource> {
  auto result =
      adapters::grant_local_folder(session(), path, generation, limits);
  INFO((result ? "granted" : result.error().message));
  REQUIRE(result);
  return *result;
}
auto token(const adapters::LocalFileSource& lease, std::uint64_t generation = 1)
    -> runtime::LocalSourceRequestToken {
  return {session(), lease.root_identity(), generation, 1, 1};
}
auto exact(const std::shared_ptr<adapters::LocalFileSource>& lease,
           std::string path, std::uint64_t generation = 1)
    -> std::expected<runtime::LocalPreviewResult, domain::LocalSourceError> {
  return lease->preview({token(*lease, generation),
                         std::move(path),
                         runtime::LocalPreviewMode::exact,
                         {}});
}
} // namespace

TEST_CASE("folder grants reject invalid roots symlinks and authority tokens") {
  Directory temporary;
  auto root = temporary.path / "root";
  fs::create_directory(root);
  fs::path requested = root;
  std::uint64_t generation = 1;
  domain::LocalSourceLimits limits;
  SECTION("relative root") {
    requested = "relative";
  }
  SECTION("missing root") {
    requested /= "missing";
  }
  SECTION("file as root") {
    write(root / "file", "x");
    requested /= "file";
  }
  SECTION("root symlink") {
    fs::create_directory_symlink(root, temporary.path / "link");
    requested = temporary.path / "link";
  }
  SECTION("ancestor symlink") {
    fs::create_directory(root / "child");
    fs::create_directory_symlink(root, temporary.path / "link");
    requested = temporary.path / "link/child";
  }
  SECTION("dot dot") {
    requested = root / "../root";
  }
  SECTION("zero generation") {
    generation = 0;
  }
  SECTION("invalid limit") {
    limits.maximum_file_bytes = std::numeric_limits<std::uint64_t>::max();
  }
  REQUIRE_FALSE(
      adapters::grant_local_folder(session(), requested, generation, limits));
}

TEST_CASE(
    "local lease refuses cross root session generation and revoked reads") {
  Directory temporary;
  write(temporary.path / "notes.txt", "hello");
  auto lease = grant(temporary.path);
  runtime::LocalPreviewRequest request{
      token(*lease), "notes.txt", runtime::LocalPreviewMode::exact, {}};
  SECTION("foreign root") {
    request.token.root.binding[0] =
        request.token.root.binding[0] == 'a' ? 'b' : 'a';
  }
  SECTION("foreign session") {
    request.token.session_id = domain::SessionId::from("other").value();
  }
  SECTION("foreign generation") {
    ++request.token.lease_generation;
  }
  SECTION("revoked") {
    lease->revoke();
  }
  auto result = lease->preview(request);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::stale_lease);
  CHECK(result.error().message.find(temporary.path.string()) ==
        std::string::npos);
}

TEST_CASE("root and ancestor substitution invalidate already pinned grants") {
  Directory temporary;
  fs::create_directories(temporary.path / "parent/root");
  const auto root = temporary.path / "parent/root";
  write(root / "notes.txt", "hello");
  auto lease = grant(root);
  SECTION("root replacement") {
    fs::rename(root, temporary.path / "old-root");
    fs::create_directory(root);
    write(root / "notes.txt", "hello");
  }
  SECTION("ancestor replacement") {
    fs::rename(temporary.path / "parent", temporary.path / "old-parent");
    fs::create_directories(root);
    write(root / "notes.txt", "hello");
  }
  SECTION("root replaced by symlink") {
    fs::rename(root, temporary.path / "old-root");
    fs::create_directory_symlink(temporary.path / "old-root", root);
  }
  REQUIRE_FALSE(exact(lease, "notes.txt"));
  REQUIRE_FALSE(lease->list({token(*lease), "", {}}));
}

TEST_CASE(
    "local reads reject traversal symlink nonregular and missing inputs") {
  Directory temporary;
  fs::create_directory(temporary.path / "folder");
  write(temporary.path / "notes.txt", "hello");
  auto lease = grant(temporary.path);
  std::string path;
  SECTION("traversal") {
    path = "../notes.txt";
  }
  SECTION("absolute") {
    path = (temporary.path / "notes.txt").string();
  }
  SECTION("missing") {
    path = "missing";
  }
  SECTION("directory") {
    path = "folder";
  }
  SECTION("leaf symlink") {
    fs::create_symlink("notes.txt", temporary.path / "link");
    path = "link";
  }
  SECTION("directory symlink") {
    fs::create_directory_symlink(temporary.path, temporary.path / "link");
    path = "link/notes.txt";
  }
  SECTION("FIFO") {
    REQUIRE(::mkfifo((temporary.path / "pipe").c_str(), 0600) == 0);
    path = "pipe";
  }
  SECTION("unsafe name") {
    path = "odd\nname";
    write(temporary.path / path, "hello");
  }
  const auto started = std::chrono::steady_clock::now();
  REQUIRE_FALSE(exact(lease, path));
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{2});
}

TEST_CASE("exact local reads refuse invalid text and oversized files") {
  Directory temporary;
  std::string content;
  SECTION("NUL") {
    content = std::string{"a\0b", 3};
  }
  SECTION("invalid UTF8") {
    content = std::string{"a\xff", 2};
  }
  SECTION("terminal escape") {
    content = "a\x1b[2J";
  }
  SECTION("too large") {
    content = std::string(256 * 1024 + 1, 'x');
  }
  write(temporary.path / "notes.txt", content);
  REQUIRE_FALSE(exact(grant(temporary.path), "notes.txt"));
}

TEST_CASE(
    "exact revalidation detects change deletion and permits byte restoration") {
  Directory temporary;
  const auto path = temporary.path / "notes.txt";
  write(path, "original");
  auto lease = grant(temporary.path);
  auto original = exact(lease, "notes.txt");
  REQUIRE(original);
  REQUIRE(original->source);
  auto identity = *original->source;
  write(path, "modified");
  auto changed = lease->revalidate({token(*lease), identity, {}});
  REQUIRE_FALSE(changed);
  CHECK(changed.error().code == Code::source_mismatch);
  fs::remove(path);
  REQUIRE_FALSE(lease->revalidate({token(*lease), identity, {}}));
  write(temporary.path / "replacement", "original");
  fs::rename(temporary.path / "replacement", path);
  lease->revoke();
  auto fresh = grant(temporary.path, 2);
  CHECK(fresh->root_identity() == identity.root);
  auto restored = fresh->revalidate({token(*fresh, 2), identity, {}});
  REQUIRE(restored);
  CHECK(restored->source == identity);
  CHECK(restored->text == "original");
  REQUIRE_FALSE(lease->revalidate({token(*lease), identity, {}}));
}

TEST_CASE("listing is bounded immediate nonrecursive and explicit about unsafe "
          "names") {
  Directory temporary;
  fs::create_directory(temporary.path / "folder");
  write(temporary.path / "folder/hidden.txt", "nested");
  write(temporary.path / "notes.txt", "hello");
  write(temporary.path / "odd\nname", "hello");
  write(temporary.path / std::string{"bad\xff", 4}, "hello");
  fs::create_symlink("notes.txt", temporary.path / "link");
  REQUIRE(::mkfifo((temporary.path / "pipe").c_str(), 0600) == 0);
  auto lease = grant(temporary.path);
  auto result = lease->list({token(*lease), "", {}});
  REQUIRE(result);
  REQUIRE(
      runtime::validate_local_list_result({token(*lease), "", {}}, *result));
  CHECK(result->state == runtime::LocalListingState::complete);
  CHECK(result->scanned_entries == 6);
  for (const auto& entry : result->entries)
    CHECK(entry.relative_path.find('/') == std::string::npos);
  CHECK(result->skipped_unsupported_entries >= 2);
  auto filtered = lease->list({token(*lease), "", {}, "txt"});
  REQUIRE(filtered);
  REQUIRE(filtered->entries.size() == 1);
  CHECK(filtered->entries[0].relative_path == "notes.txt");
  CHECK(filtered->scanned_entries == 6);
  CHECK(filtered->skipped_filtered_entries > 0);
  auto nested = lease->list({token(*lease), "folder", {}});
  REQUIRE(nested);
  REQUIRE(nested->entries.size() == 1);
  CHECK(nested->entries[0].relative_path == "folder/hidden.txt");
}

TEST_CASE(
    "listing limits charge scan and capacity without false completeness") {
  Directory temporary;
  write(temporary.path / "a.txt", "a");
  write(temporary.path / "b.txt", "b");
  write(temporary.path / "c.txt", "c");
  auto lease = grant(temporary.path);
  runtime::LocalListRequest request{token(*lease), "", {}};
  SECTION("entry bound") {
    request.limits.maximum_list_entries = 1;
  }
  SECTION("byte bound") {
    request.limits.maximum_listing_bytes = 1;
  }
  SECTION("scan bound") {
    request.limits.maximum_list_entries = 1;
    request.limits.maximum_scanned_entries = 1;
    request.filename_filter = "unmatched";
  }
  auto result = lease->list(request);
  REQUIRE(result);
  CHECK(result->state == runtime::LocalListingState::partial);
  CHECK(result->scanned_entries <= request.limits.maximum_scanned_entries);
  CHECK(result->entries.size() <= request.limits.maximum_list_entries);
  CHECK(runtime::validate_local_list_result(request, *result));
  if (request.limits.maximum_listing_bytes == 1)
    CHECK(result->skipped_capacity_entries == 1);
}

TEST_CASE(
    "cancellation and grant-specific ceilings apply before source operations") {
  Directory temporary;
  write(temporary.path / "notes.txt", "hello");
  std::stop_source stop;
  stop.request_stop();
  REQUIRE_FALSE(adapters::grant_local_folder(session(), temporary.path, 1, {},
                                             stop.get_token()));
  domain::LocalSourceLimits limits;
  limits.maximum_file_bytes = 8;
  limits.maximum_preview_bytes = 4;
  auto lease = grant(temporary.path, 1, limits);
  CHECK_FALSE(exact(lease, "notes.txt"));
  auto preview = runtime::LocalPreviewRequest{
      token(*lease), "notes.txt", runtime::LocalPreviewMode::exact, limits};
  auto cancelled = lease->preview(preview, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == Code::cancelled);
  REQUIRE(lease->preview(preview));
  auto listing = lease->list({token(*lease), "", limits}, stop.get_token());
  REQUIRE_FALSE(listing);
  CHECK(listing.error().code == Code::cancelled);
}

TEST_CASE("prefix boundaries preserve UTF8 and empty exact proof") {
  Directory temporary;
  write(temporary.path / "unicode", "ab\xe2\x82\xac-tail");
  write(temporary.path / "empty", "");
  write(temporary.path / "AGENTS.md", "ordinary evidence");
  auto lease = grant(temporary.path);
  runtime::LocalPreviewRequest request{
      token(*lease), "unicode", runtime::LocalPreviewMode::bounded_prefix, {}};
  request.limits.maximum_preview_bytes = 4;
  auto result = lease->preview(request);
  REQUIRE(result);
  CHECK(result->text == "ab");
  CHECK(result->state == runtime::LocalPreviewState::prefix);
  CHECK_FALSE(result->source);
  auto empty = exact(lease, "empty");
  REQUIRE(empty);
  CHECK(empty->state == runtime::LocalPreviewState::complete);
  REQUIRE(empty->source);
  CHECK(empty->source->content_digest.byte_size == 0);
  CHECK(empty->source->content_digest.value ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  auto agents = exact(lease, "AGENTS.md");
  REQUIRE(agents);
  CHECK(agents->text == "ordinary evidence");
  CHECK(lease->guarantees_pinned_read_only_sources());
}

TEST_CASE(
    "repeated and concurrent listings have independent directory positions") {
  Directory temporary;
  write(temporary.path / "a", "a");
  write(temporary.path / "b", "b");
  auto lease = grant(temporary.path);
  const runtime::LocalListRequest request{token(*lease), "", {}};
  for (int index = 0; index < 3; ++index) {
    const auto result = lease->list(request);
    REQUIRE(result);
    CHECK(result->state == runtime::LocalListingState::complete);
    CHECK(result->entries.size() == 2);
  }
  auto first = std::async(std::launch::async,
                          [lease, request] { return lease->list(request); });
  auto second = std::async(std::launch::async,
                           [lease, request] { return lease->list(request); });
  const auto left = first.get();
  const auto right = second.get();
  REQUIRE(left);
  REQUIRE(right);
  CHECK(left->entries.size() == 2);
  CHECK(right->entries.size() == 2);
  CHECK(left->state == runtime::LocalListingState::complete);
  CHECK(right->state == runtime::LocalListingState::complete);
}

TEST_CASE("unreadable file returns safe permission failure") {
  Directory temporary;
  const auto path = temporary.path / "private-file";
  write(path, "private contents must not appear in errors");
  REQUIRE(::chmod(temporary.path.c_str(), 0755) == 0);
  REQUIRE(::chmod(path.c_str(), 0000) == 0);
  const auto child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // Privileged test runners still exercise real DAC refusal in this child.
    if (::geteuid() == 0 && (::setgid(65534) != 0 || ::setuid(65534) != 0))
      ::_exit(2);
    auto lease = adapters::grant_local_folder(session(), temporary.path, 1);
    if (!lease) ::_exit(3);
    const auto result = (*lease)->preview(
        {token(**lease), "private-file", runtime::LocalPreviewMode::exact, {}});
    if (result || result.error().code != Code::permission_denied ||
        result.error().message.find("private") != std::string::npos ||
        result.error().message.find(temporary.path.string()) !=
            std::string::npos)
      ::_exit(4);
    ::_exit(0);
  }
  int status{};
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
}

TEST_CASE(
    "prefix trims only an incomplete boundary not invalid earlier encoding") {
  Directory temporary;
  std::string bytes;
  SECTION("invalid earlier byte") {
    bytes = std::string{"a\xff-tail", 7};
  }
  SECTION("invalid continuation at boundary") {
    bytes = "ab\xe2-tail";
  }
  SECTION("overlong codepoint at boundary") {
    bytes = "ab\xe0\x80\x80-tail";
  }
  SECTION("truncated actual file") {
    bytes = "ab\xe2\x82";
  }
  write(temporary.path / "text", bytes);
  auto lease = grant(temporary.path);
  runtime::LocalPreviewRequest request{
      token(*lease), "text", runtime::LocalPreviewMode::bounded_prefix, {}};
  request.limits.maximum_preview_bytes = 4;
  const auto result = lease->preview(request);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_text);
}
