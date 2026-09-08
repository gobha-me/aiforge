#include <catch2/catch_test_macros.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/local_source.hpp>

#include <limits>
#include <span>

namespace {
using namespace aiforge;
auto root() -> domain::LocalRootIdentity {
  return {1, std::string(64, 'a')};
}
auto token() -> runtime::LocalSourceRequestToken {
  return {domain::SessionId::from("session").value(), root(), 1, 1, 1};
}
auto source(std::string path = "notes.txt", std::string_view text = "hello")
    -> domain::LocalSourceIdentity {
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {root(), std::move(path), {"sha256", hash.finish(), text.size()}};
}

class FakeReader final : public runtime::LocalSourceReader {
 public:
  runtime::LocalSourceRequestToken live{token()};
  std::string bytes{"hello"};
  bool revoked{};
  auto list(runtime::LocalListRequest request, std::stop_token stop)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    if (auto valid = authorize(request.token, stop); !valid)
      return std::unexpected(valid.error());
    if (auto valid = runtime::validate_local_list_request(request); !valid)
      return std::unexpected(valid.error());
    runtime::LocalListResult result{
        request.token,
        request.directory,
        {{"notes.txt", runtime::LocalEntryKind::regular_file}},
        1,
        runtime::LocalListingState::complete};
    if (auto valid = runtime::validate_local_list_result(request, result);
        !valid)
      return std::unexpected(valid.error());
    return result;
  }
  auto preview(runtime::LocalPreviewRequest request, std::stop_token stop)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    if (auto valid = authorize(request.token, stop); !valid)
      return std::unexpected(valid.error());
    if (auto valid = runtime::validate_local_preview_request(request); !valid)
      return std::unexpected(valid.error());
    runtime::LocalPreviewResult result{request.token,
                                       request.relative_path,
                                       bytes.size(),
                                       bytes,
                                       runtime::LocalPreviewState::complete,
                                       source(request.relative_path, bytes)};
    if (auto valid = runtime::validate_local_preview_result(request, result);
        !valid)
      return std::unexpected(valid.error());
    return result;
  }
  auto revalidate(runtime::LocalRevalidateRequest request, std::stop_token stop)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    if (auto valid = authorize(request.token, stop); !valid)
      return std::unexpected(valid.error());
    if (auto valid = runtime::validate_local_revalidate_request(request);
        !valid)
      return std::unexpected(valid.error());
    runtime::LocalReadResult result{
        request.token, source(request.expected_source.relative_path, bytes),
        bytes};
    if (auto valid = runtime::validate_local_read_result(request, result);
        !valid)
      return std::unexpected(valid.error());
    return result;
  }

 private:
  auto authorize(const runtime::LocalSourceRequestToken& request,
                 std::stop_token stop)
      -> std::expected<void, domain::LocalSourceError> {
    if (stop.stop_requested())
      return std::unexpected(domain::LocalSourceError{
          domain::LocalSourceErrorCode::cancelled, "cancelled"});
    if (revoked || request.session_id != live.session_id ||
        request.root != live.root ||
        request.lease_generation != live.lease_generation)
      return std::unexpected(domain::LocalSourceError{
          domain::LocalSourceErrorCode::stale_lease, "lease unavailable"});
    return {};
  }
};
} // namespace

TEST_CASE(
    "local source identity rejects unsupported and ambiguous references") {
  auto value = source();
  SECTION("future physical identity version") {
    value.root.version = 2;
  }
  SECTION("path is not a physical binding") {
    value.root.binding = "/home/user";
  }
  SECTION("noncanonical binding") {
    value.root.binding[0] = 'A';
  }
  SECTION("foreign digest algorithm") {
    value.content_digest.algorithm = "git-sha256";
  }
  SECTION("invalid digest") {
    value.content_digest.value[0] = 'X';
  }
  SECTION("huge source") {
    value.content_digest.byte_size = std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("parent traversal") {
    value.relative_path = "one/../two";
  }
  SECTION("absolute path") {
    value.relative_path = "/notes";
  }
  SECTION("empty component") {
    value.relative_path = "one//two";
  }
  SECTION("dot component") {
    value.relative_path = "one/./two";
  }
  SECTION("trailing slash") {
    value.relative_path = "one/";
  }
  SECTION("NUL") {
    value.relative_path = std::string{"a\0b", 3};
  }
  REQUIRE_FALSE(domain::validate_local_source_identity(value));
}

TEST_CASE("local source limits reject widening zero and inconsistent values") {
  domain::LocalSourceLimits limits;
  SECTION("roots") {
    limits.maximum_roots = 17;
  }
  SECTION("selection count") {
    limits.maximum_selected_files = 65;
  }
  SECTION("file bytes") {
    ++limits.maximum_file_bytes;
  }
  SECTION("aggregate overflow") {
    limits.maximum_total_bytes = std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("zero path") {
    limits.maximum_path_bytes = 0;
  }
  SECTION("depth") {
    limits.maximum_depth = 65;
  }
  SECTION("entries") {
    ++limits.maximum_list_entries;
  }
  SECTION("scan") {
    ++limits.maximum_scanned_entries;
  }
  SECTION("list bytes") {
    ++limits.maximum_listing_bytes;
  }
  SECTION("preview") {
    limits.maximum_preview_bytes = limits.maximum_file_bytes + 1;
  }
  SECTION("zero timeout") {
    limits.timeout = std::chrono::milliseconds{0};
  }
  SECTION("huge timeout") {
    limits.timeout = std::chrono::milliseconds::max();
  }
  SECTION("zero count") {
    limits.maximum_selected_files = 0;
  }
  REQUIRE_FALSE(domain::validate_local_source_limits(limits));
}

TEST_CASE(
    "local selection checks count roots duplicates and aggregate before use") {
  std::vector<domain::LocalSourceIdentity> values{source()};
  domain::LocalSourceLimits limits;
  SECTION("duplicate root and path even with different bytes") {
    values.push_back(source("notes.txt", "changed"));
  }
  SECTION("too many sources") {
    values.resize(65, source());
  }
  SECTION("root ceiling") {
    limits.maximum_roots = 1;
    values.push_back(source("other"));
    values.back().root.binding[0] = 'b';
  }
  SECTION("total bytes") {
    limits.maximum_total_bytes = 5;
    limits.maximum_file_bytes = 5;
    limits.maximum_preview_bytes = 5;
    values.push_back(source("other"));
  }
  REQUIRE_FALSE(domain::validate_local_source_selection(values, limits));
}

TEST_CASE(
    "directory result rejects forged binding counts identities and status") {
  runtime::LocalListRequest request{token(), "dir", {}};
  runtime::LocalListResult result{
      token(),
      "dir",
      {{"dir/file", runtime::LocalEntryKind::regular_file}},
      1,
      runtime::LocalListingState::complete};
  SECTION("foreign session") {
    result.token.session_id = domain::SessionId::from("other").value();
  }
  SECTION("stale generation") {
    ++result.token.lease_generation;
  }
  SECTION("stale revision") {
    ++result.token.selection_revision;
  }
  SECTION("wrong request") {
    ++result.token.request_id;
  }
  SECTION("wrong directory") {
    result.directory = "other";
  }
  SECTION("outside directory") {
    result.entries[0].relative_path = "else/file";
  }
  SECTION("grandchild") {
    result.entries[0].relative_path = "dir/nested/file";
  }
  SECTION("duplicate") {
    result.entries.push_back(result.entries[0]);
    ++result.scanned_entries;
  }
  SECTION("underreported scan") {
    result.scanned_entries = 0;
  }
  SECTION("scan overflow") {
    result.scanned_entries = std::numeric_limits<std::size_t>::max();
  }
  SECTION("unknown state") {
    result.state = static_cast<runtime::LocalListingState>(99);
  }
  SECTION("unknown kind") {
    result.entries[0].kind = static_cast<runtime::LocalEntryKind>(99);
  }
  SECTION("byte bound") {
    request.limits.maximum_listing_bytes = 1;
  }
  REQUIRE_FALSE(runtime::validate_local_list_result(request, result));
}

TEST_CASE("preview completeness cannot fabricate exact source proof") {
  runtime::LocalPreviewRequest request{
      token(), "notes.txt", runtime::LocalPreviewMode::exact, {}};
  runtime::LocalPreviewResult result{
      token(), "notes.txt", 5, "hello", runtime::LocalPreviewState::complete,
      source()};
  SECTION("altered bytes") {
    result.text = "jello";
  }
  SECTION("wrong observed size") {
    result.observed_file_bytes = 6;
  }
  SECTION("missing exact proof") {
    result.source.reset();
  }
  SECTION("foreign path") {
    result.source->relative_path = "other";
  }
  SECTION("foreign root") {
    result.source->root.binding[0] = 'b';
  }
  SECTION("stale result") {
    ++result.token.lease_generation;
  }
  SECTION("binary text") {
    result.text[0] = '\0';
  }
  SECTION("invalid UTF8") {
    result.text[0] = static_cast<char>(0xff);
  }
  SECTION("terminal control") {
    result.text[0] = '\x1b';
  }
  SECTION("empty text with nonempty proof") {
    result.text.clear();
  }
  SECTION("prefix offered as exact") {
    result.state = runtime::LocalPreviewState::prefix;
    result.source.reset();
  }
  SECTION("prefix with full proof") {
    request.mode = runtime::LocalPreviewMode::bounded_prefix;
    result.state = runtime::LocalPreviewState::prefix;
  }
  SECTION("unknown mode") {
    request.mode = static_cast<runtime::LocalPreviewMode>(99);
  }
  SECTION("unknown completeness") {
    result.state = static_cast<runtime::LocalPreviewState>(99);
  }
  REQUIRE_FALSE(runtime::validate_local_preview_result(request, result));
}

TEST_CASE(
    "read-only fake refuses revoked foreign cancelled and changed sources") {
  FakeReader reader;
  runtime::LocalRevalidateRequest request{token(), source(), {}};
  std::stop_source stop;
  SECTION("revocation") {
    reader.revoked = true;
  }
  SECTION("new lease makes old request stale") {
    ++reader.live.lease_generation;
  }
  SECTION("other session") {
    request.token.session_id = domain::SessionId::from("other").value();
  }
  SECTION("source bytes changed") {
    reader.bytes = "changed";
  }
  SECTION("root mismatch") {
    request.expected_source.root.binding[0] = 'b';
  }
  SECTION("cancelled") {
    stop.request_stop();
  }
  REQUIRE_FALSE(reader.revalidate(request, stop.get_token()));
}

TEST_CASE(
    "exact restore uses durable root and bytes under a fresh explicit lease") {
  FakeReader reader;
  auto identity = source();
  reader.live.lease_generation = 2;
  auto fresh = token();
  fresh.lease_generation = 2;
  auto restored = reader.revalidate({fresh, identity, {}}, {});
  REQUIRE(restored);
  CHECK(restored->source == identity);
  CHECK(restored->token == fresh);
  auto stale = *restored;
  stale.token.lease_generation = 1;
  CHECK_FALSE(
      runtime::validate_local_read_result({fresh, identity, {}}, stale));
}

TEST_CASE(
    "bounded listing retains raw unsafe names separately from rendering") {
  runtime::LocalListRequest request{token(), "", {}};
  runtime::LocalListResult result{
      token(),
      "",
      {{"notes.txt", runtime::LocalEntryKind::regular_file},
       {"odd\nname", runtime::LocalEntryKind::unsupported},
       {std::string{"bad\xff", 4}, runtime::LocalEntryKind::unsupported}},
      3,
      runtime::LocalListingState::partial};
  REQUIRE(runtime::validate_local_list_result(request, result));
  CHECK(result.entries[1].relative_path == "odd\nname");
  result.state = runtime::LocalListingState::complete;
  REQUIRE(runtime::validate_local_list_result(request, result));
}

TEST_CASE(
    "prefix is visibly incomplete and exact read remains separately bounded") {
  runtime::LocalPreviewRequest request{
      token(), "notes.txt", runtime::LocalPreviewMode::bounded_prefix, {}};
  runtime::LocalPreviewResult result{token(),
                                     "notes.txt",
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "hello",
                                     runtime::LocalPreviewState::prefix,
                                     std::nullopt};
  REQUIRE(runtime::validate_local_preview_result(request, result));
  result.observed_file_bytes = result.text.size();
  CHECK_FALSE(runtime::validate_local_preview_result(request, result));
  result = {
      token(), "notes.txt", 5, "hello", runtime::LocalPreviewState::complete,
      source()};
  REQUIRE(runtime::validate_local_preview_result(request, result));
  FakeReader reader;
  REQUIRE(reader.list({token(), "", {}}, {}));
  REQUIRE(reader.preview(request, {}));
  REQUIRE(domain::validate_local_source_selection({}));
}

TEST_CASE(
    "empty files have complete preview proof but are unavailable evidence") {
  const auto empty = source("empty.txt", "");
  REQUIRE(domain::validate_local_source_identity(empty));
  CHECK(empty.content_digest.value ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  runtime::LocalPreviewRequest request{
      token(), "empty.txt", runtime::LocalPreviewMode::bounded_prefix, {}};
  runtime::LocalPreviewResult result{
      token(), "empty.txt", 0, "", runtime::LocalPreviewState::complete, empty};
  REQUIRE(runtime::validate_local_preview_result(request, result));
  const std::vector values{empty};
  const auto selected = domain::validate_local_source_selection(values);
  REQUIRE_FALSE(selected);
  CHECK(selected.error().code == domain::LocalSourceErrorCode::unavailable);
  result.state = runtime::LocalPreviewState::prefix;
  result.source.reset();
  CHECK_FALSE(runtime::validate_local_preview_result(request, result));
}

TEST_CASE(
    "listing accounts for skipped names and validates filter during scan") {
  runtime::LocalListRequest request{token(), "dir", {}, "txt"};
  runtime::LocalListResult result{
      token(),
      "dir",
      {{"dir/notes.txt", runtime::LocalEntryKind::regular_file}},
      3,
      runtime::LocalListingState::complete,
      1,
      1};
  REQUIRE(runtime::validate_local_list_result(request, result));
  SECTION("unmatched emitted name") {
    result.entries[0].relative_path = "dir/photo.png";
  }
  SECTION("hidden skipped name") {
    result.skipped_unsupported_entries = 0;
  }
  SECTION("overflow unsupported count") {
    result.skipped_unsupported_entries =
        std::numeric_limits<std::size_t>::max();
  }
  SECTION("overflow filtered count") {
    result.skipped_filtered_entries = std::numeric_limits<std::size_t>::max();
  }
  SECTION("filter absent but filtered count") {
    request.filename_filter.clear();
  }
  SECTION("invalid filter") {
    request.filename_filter = "dir/txt";
  }
  SECTION("oversized filter") {
    request.filename_filter = std::string(256, 'x');
  }
  SECTION("unsafe selectable name") {
    result.entries[0].relative_path = "dir/bad\ntxt";
  }
  REQUIRE_FALSE(runtime::validate_local_list_result(request, result));
}

TEST_CASE(
    "local requests reject missing identity and unsupported raw read paths") {
  runtime::LocalPreviewRequest request{
      token(), "notes.txt", runtime::LocalPreviewMode::exact, {}};
  SECTION("zero lease") {
    request.token.lease_generation = 0;
  }
  SECTION("zero request") {
    request.token.request_id = 0;
  }
  SECTION("zero revision") {
    request.token.selection_revision = 0;
  }
  SECTION("unsupported controls") {
    request.relative_path = "odd\nname";
  }
  SECTION("unsupported encoding") {
    request.relative_path = std::string{"bad\xff", 4};
  }
  SECTION("root is not file") {
    request.relative_path.clear();
  }
  SECTION("oversized path") {
    request.relative_path = std::string(4097, 'x');
  }
  SECTION("oversized component") {
    request.relative_path = std::string(256, 'x');
  }
  SECTION("excess depth") {
    request.relative_path = "x";
    for (int index = 0; index < 64; ++index)
      request.relative_path += "/x";
  }
  REQUIRE_FALSE(runtime::validate_local_preview_request(request));
}

TEST_CASE("exact boundaries and narrowed limits do not truncate file proof") {
  runtime::LocalPreviewRequest request{
      token(), "notes.txt", runtime::LocalPreviewMode::exact, {}};
  const std::string bytes(
      static_cast<std::size_t>(request.limits.maximum_file_bytes), 'x');
  runtime::LocalPreviewResult result{token(),
                                     "notes.txt",
                                     bytes.size(),
                                     bytes,
                                     runtime::LocalPreviewState::complete,
                                     source("notes.txt", bytes)};
  REQUIRE(runtime::validate_local_preview_result(request, result));
  auto too_large = result;
  too_large.text.push_back('x');
  CHECK_FALSE(runtime::validate_local_preview_result(request, too_large));
  request.mode = runtime::LocalPreviewMode::bounded_prefix;
  CHECK_FALSE(runtime::validate_local_preview_result(request, result));
  result.text.resize(
      static_cast<std::size_t>(request.limits.maximum_preview_bytes));
  result.state = runtime::LocalPreviewState::prefix;
  result.source.reset();
  REQUIRE(runtime::validate_local_preview_result(request, result));
  std::vector<domain::LocalSourceIdentity> selected;
  for (int index = 0; index < 8; ++index)
    selected.push_back(source(std::to_string(index), bytes));
  REQUIRE(domain::validate_local_source_selection(selected));
  selected.push_back(source("ninth", bytes));
  CHECK_FALSE(domain::validate_local_source_selection(selected));
  CHECK(domain::validate_local_relative_path(std::string(255, 'x')));
  CHECK(domain::validate_local_relative_path("", true));
  CHECK_FALSE(domain::validate_local_relative_path("", false));
}

TEST_CASE("partial listings account for inspected names that exceed output "
          "capacity") {
  runtime::LocalListRequest request{token(), "", {}};
  runtime::LocalListResult result{
      token(),
      "",
      {{"a", runtime::LocalEntryKind::regular_file}},
      2,
      runtime::LocalListingState::partial,
      0,
      0,
      1};
  request.limits.maximum_listing_bytes = 1;
  REQUIRE(runtime::validate_local_list_result(request, result));
  result.state = runtime::LocalListingState::complete;
  CHECK_FALSE(runtime::validate_local_list_result(request, result));
  result.state = runtime::LocalListingState::partial;
  result.skipped_capacity_entries = std::numeric_limits<std::size_t>::max();
  CHECK_FALSE(runtime::validate_local_list_result(request, result));
  result.skipped_capacity_entries = 0;
  CHECK_FALSE(runtime::validate_local_list_result(request, result));
}
