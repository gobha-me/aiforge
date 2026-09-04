#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <aiforge/adapters/filesystem_artifact_store.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/transcript_projection.hpp>
#include <aiforge/runtime/video_artifacts.hpp>
#include <aiforge/testing/scripted_artifact_store.hpp>
#include <aiforge/video/mp4.hpp>

namespace {

using namespace aiforge;

template <typename IdType>
[[nodiscard]] auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto append_u32(std::vector<std::byte>& bytes, const std::uint32_t value)
    -> void {
  for (const auto shift : {24U, 16U, 8U, 0U})
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

auto set_u32(std::vector<std::byte>& bytes, const std::size_t offset,
             const std::uint32_t value) -> void {
  for (std::size_t index{}; index < 4; ++index) {
    const auto shift = static_cast<unsigned>((3 - index) * 8);
    bytes[offset + index] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

auto set_u64(std::vector<std::byte>& bytes, const std::size_t offset,
             const std::uint64_t value) -> void {
  set_u32(bytes, offset, static_cast<std::uint32_t>(value >> 32U));
  set_u32(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

auto append_text(std::vector<std::byte>& bytes, const std::string_view text)
    -> void {
  for (const unsigned char value : text)
    bytes.push_back(static_cast<std::byte>(value));
}

[[nodiscard]] auto box(const std::string_view type,
                       const std::span<const std::byte> payload = {})
    -> std::vector<std::byte> {
  std::vector<std::byte> bytes;
  append_u32(bytes, static_cast<std::uint32_t>(payload.size() + 8));
  append_text(bytes, type);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

auto append(std::vector<std::byte>& into,
            const std::span<const std::byte> value) -> void {
  into.insert(into.end(), value.begin(), value.end());
}

[[nodiscard]] auto file_type(const std::string_view major = "isom",
                             const std::size_t compatible = 0)
    -> std::vector<std::byte> {
  std::vector<std::byte> payload;
  append_text(payload, major);
  append_u32(payload, 0);
  for (std::size_t index{}; index < compatible; ++index)
    append_text(payload, "avc1");
  return box("ftyp", payload);
}

[[nodiscard]] auto video_track(const bool video_handler = true)
    -> std::vector<std::byte> {
  std::vector<std::byte> handler_payload(24);
  const auto handler = video_handler ? "vide" : "soun";
  for (std::size_t index{}; index < 4; ++index)
    handler_payload[8 + index] = static_cast<std::byte>(handler[index]);
  const auto handler_box = box("hdlr", handler_payload);
  const auto media = box("mdia", handler_box);
  return box("trak", media);
}

[[nodiscard]] auto minimal_mp4(const std::string_view major = "isom",
                               const std::size_t compatible = 0,
                               const std::size_t tracks = 1,
                               const bool video_handler = true)
    -> std::vector<std::byte> {
  auto bytes = file_type(major, compatible);
  std::vector<std::byte> movie_payload;
  for (std::size_t index{}; index < tracks; ++index)
    append(movie_payload, video_track(video_handler));
  append(bytes, box("moov", movie_payload));
  const std::array media{std::byte{0x42}};
  append(bytes, box("mdat", media));
  return bytes;
}

[[nodiscard]] auto digest_of(const std::span<const std::byte> content)
    -> std::string {
  detail::Sha256 digest;
  digest.update(content);
  return "sha256:" + digest.finish();
}

[[nodiscard]] auto metadata(const std::string& id,
                            const std::span<const std::byte> content)
    -> domain::ArtifactMetadata {
  return {make_id<domain::ArtifactId>(id),
          "video/mp4",
          static_cast<std::uint64_t>(content.size()),
          digest_of(content),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    m_path =
        std::filesystem::temp_directory_path() / ("aiforge-mp4-test-" + suffix);
    std::filesystem::create_directories(m_path);
#ifndef _WIN32
    REQUIRE(::chmod(m_path.c_str(), S_IRWXU) == 0);
#endif
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

TEST_CASE("MP4 limits and byte bounds fail closed") {
  const auto valid = minimal_mp4();
  for (const auto limits : {video::Mp4Limits{0, 6, 3, 1, 1},
                            video::Mp4Limits{valid.size(), 0, 3, 1, 1},
                            video::Mp4Limits{valid.size(), 6, 0, 1, 1},
                            video::Mp4Limits{valid.size(), 6, 3, 0, 1},
                            video::Mp4Limits{valid.size(), 6, 3, 1, 0}}) {
    auto rejected = video::validate_mp4(valid, limits);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == video::Mp4ErrorCode::invalid_limits);
  }
  auto empty = video::validate_mp4({});
  REQUIRE_FALSE(empty);
  CHECK(empty.error().code == video::Mp4ErrorCode::empty);
  auto too_large =
      video::validate_mp4(valid, {.maximum_bytes = valid.size() - 1});
  REQUIRE_FALSE(too_large);
  CHECK(too_large.error().code == video::Mp4ErrorCode::too_large);
  REQUIRE(video::validate_mp4(valid, {.maximum_bytes = valid.size()}));

  constexpr std::size_t default_maximum = 32U * 1024U * 1024U;
  auto exact_limit = valid;
  const auto media_offset =
      file_type().size() + box("moov", video_track()).size();
  exact_limit.resize(default_maximum);
  set_u32(exact_limit, media_offset,
          static_cast<std::uint32_t>(default_maximum - media_offset));
  REQUIRE(video::validate_mp4(exact_limit));
  exact_limit.push_back(std::byte{0});
  auto default_over_limit = video::validate_mp4(exact_limit);
  REQUIRE_FALSE(default_over_limit);
  CHECK(default_over_limit.error().code == video::Mp4ErrorCode::too_large);

  std::stop_source stopped;
  stopped.request_stop();
  auto cancelled = video::validate_mp4(valid, {}, stopped.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == video::Mp4ErrorCode::cancelled);
}

TEST_CASE("MP4 box sizes and complete coverage are checked") {
  const auto valid = minimal_mp4();
  for (const auto size : {std::size_t{1}, std::size_t{7}, valid.size() - 1}) {
    auto truncated = valid;
    truncated.resize(size);
    CHECK_FALSE(video::validate_mp4(truncated));
  }

  auto too_small = valid;
  too_small[0] = std::byte{0};
  too_small[1] = std::byte{0};
  too_small[2] = std::byte{0};
  too_small[3] = std::byte{7};
  CHECK_FALSE(video::validate_mp4(too_small));

  auto extended = valid;
  extended[0] = std::byte{0};
  extended[1] = std::byte{0};
  extended[2] = std::byte{0};
  extended[3] = std::byte{1};
  CHECK_FALSE(video::validate_mp4(extended));

  auto valid_extended = valid;
  valid_extended.insert(valid_extended.begin() + 8, 8, std::byte{0});
  set_u32(valid_extended, 0, 1);
  set_u64(valid_extended, 8, 24);
  REQUIRE(video::validate_mp4(valid_extended));
  set_u64(valid_extended, 8, 15);
  CHECK_FALSE(video::validate_mp4(valid_extended));
  set_u64(valid_extended, 8, std::numeric_limits<std::uint64_t>::max());
  CHECK_FALSE(video::validate_mp4(valid_extended));

  auto nested_extended = valid;
  constexpr std::size_t movie_offset = 16;
  constexpr std::size_t track_offset = 24;
  constexpr std::size_t media_offset = 32;
  constexpr std::size_t handler_offset = 40;
  nested_extended.insert(nested_extended.begin() + handler_offset + 8, 8,
                         std::byte{0});
  set_u32(nested_extended, movie_offset, 64);
  set_u32(nested_extended, track_offset, 56);
  set_u32(nested_extended, media_offset, 48);
  set_u32(nested_extended, handler_offset, 1);
  set_u64(nested_extended, handler_offset + 8, 40);
  REQUIRE(video::validate_mp4(nested_extended));

  auto zero_sized_nested = valid;
  const auto nested_track_offset = file_type().size() + 8;
  for (std::size_t index{}; index < 4; ++index)
    zero_sized_nested[nested_track_offset + index] = std::byte{0};
  CHECK_FALSE(video::validate_mp4(zero_sized_nested));

  auto zero_final = valid;
  const auto final_media_offset =
      file_type().size() + box("moov", video_track()).size();
  for (std::size_t index{}; index < 4; ++index)
    zero_final[final_media_offset + index] = std::byte{0};
  REQUIRE(video::validate_mp4(zero_final));
}

TEST_CASE("MP4 major brands are exact and compatible brands do not rescue") {
  constexpr std::array allowed{"isom", "iso2", "iso3", "iso4", "iso5",
                               "iso6", "iso7", "iso8", "iso9", "isoa",
                               "isob", "isoc", "mp41", "mp42"};
  for (const auto* brand : allowed) {
    auto parsed = video::validate_mp4(minimal_mp4(brand));
    REQUIRE(parsed);
    CHECK(parsed->major_brand ==
          std::array{brand[0], brand[1], brand[2], brand[3]});
  }
  auto rescued = minimal_mp4("avc1", 1);
  const auto compatible_offset = std::size_t{16};
  for (std::size_t index{}; index < 4; ++index)
    rescued[compatible_offset + index] = static_cast<std::byte>("isom"[index]);
  auto rejected = video::validate_mp4(rescued);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == video::Mp4ErrorCode::unsupported);
  CHECK_FALSE(video::validate_mp4(minimal_mp4("ISOM")));
}

TEST_CASE("MP4 required structure and every count limit are enforced") {
  const auto valid = minimal_mp4();
  REQUIRE(video::validate_mp4(valid, {.maximum_bytes = valid.size(),
                                      .maximum_boxes = 6,
                                      .maximum_nesting_depth = 3,
                                      .maximum_tracks = 1,
                                      .maximum_compatible_brands = 1}));
  CHECK_FALSE(video::validate_mp4(
      valid, {.maximum_bytes = valid.size(), .maximum_boxes = 5}));
  CHECK_FALSE(video::validate_mp4(
      valid, {.maximum_bytes = valid.size(), .maximum_nesting_depth = 2}));
  CHECK_FALSE(
      video::validate_mp4(minimal_mp4("isom", 0, 2), {.maximum_tracks = 1}));
  CHECK_FALSE(video::validate_mp4(minimal_mp4("isom", 2),
                                  {.maximum_compatible_brands = 1}));
  REQUIRE(video::validate_mp4(minimal_mp4("isom", 1),
                              {.maximum_compatible_brands = 1}));
  REQUIRE(video::validate_mp4(minimal_mp4("isom", 64)));
  CHECK_FALSE(video::validate_mp4(minimal_mp4("isom", 65)));
  REQUIRE(video::validate_mp4(minimal_mp4("isom", 0, 16)));
  CHECK_FALSE(video::validate_mp4(minimal_mp4("isom", 0, 17)));

  auto exact_box_limit = valid;
  const auto empty_box = box("free");
  for (std::size_t index{}; index < 4090; ++index) {
    exact_box_limit.insert(exact_box_limit.begin() +
                               static_cast<std::ptrdiff_t>(file_type().size()),
                           empty_box.begin(), empty_box.end());
  }
  auto exact_boxes = video::validate_mp4(exact_box_limit);
  REQUIRE(exact_boxes);
  CHECK(exact_boxes->box_count == 4096);
  exact_box_limit.insert(exact_box_limit.begin() +
                             static_cast<std::ptrdiff_t>(file_type().size()),
                         empty_box.begin(), empty_box.end());
  CHECK_FALSE(video::validate_mp4(exact_box_limit));

  auto missing_file_type = valid;
  missing_file_type.erase(missing_file_type.begin(),
                          missing_file_type.begin() +
                              static_cast<std::ptrdiff_t>(file_type().size()));
  CHECK_FALSE(video::validate_mp4(missing_file_type));

  auto duplicate_file_type = valid;
  const auto another_file_type = file_type();
  duplicate_file_type.insert(
      duplicate_file_type.begin() +
          static_cast<std::ptrdiff_t>(another_file_type.size()),
      another_file_type.begin(), another_file_type.end());
  CHECK_FALSE(video::validate_mp4(duplicate_file_type));

  auto misplaced_file_type = valid;
  misplaced_file_type.insert(misplaced_file_type.begin(), empty_box.begin(),
                             empty_box.end());
  CHECK_FALSE(video::validate_mp4(misplaced_file_type));

  auto short_file_type = valid;
  set_u32(short_file_type, 0, 12);
  CHECK_FALSE(video::validate_mp4(short_file_type));
  auto misaligned_file_type = valid;
  misaligned_file_type.insert(misaligned_file_type.begin() + 16, std::byte{0});
  set_u32(misaligned_file_type, 0, 17);
  CHECK_FALSE(video::validate_mp4(misaligned_file_type));

  const std::array media{std::byte{1}};
  std::vector<std::vector<std::byte>> incomplete;
  auto no_movie = file_type();
  append(no_movie, box("mdat", media));
  incomplete.push_back(std::move(no_movie));
  auto no_media_data = file_type();
  append(no_media_data, box("moov", video_track()));
  incomplete.push_back(std::move(no_media_data));
  auto no_track = file_type();
  append(no_track, box("moov"));
  append(no_track, box("mdat", media));
  incomplete.push_back(std::move(no_track));
  auto no_media = file_type();
  append(no_media, box("moov", box("trak")));
  append(no_media, box("mdat", media));
  incomplete.push_back(std::move(no_media));
  auto no_handler = file_type();
  append(no_handler, box("moov", box("trak", box("mdia"))));
  append(no_handler, box("mdat", media));
  incomplete.push_back(std::move(no_handler));
  for (const auto& candidate : incomplete)
    CHECK_FALSE(video::validate_mp4(candidate));

  auto duplicate_movie = valid;
  const auto movie = box("moov", video_track());
  duplicate_movie.insert(duplicate_movie.end() - 9, movie.begin(), movie.end());
  CHECK_FALSE(video::validate_mp4(duplicate_movie));

  CHECK_FALSE(video::validate_mp4(minimal_mp4("isom", 0, 1, false)));
  auto empty_media = valid;
  empty_media.resize(empty_media.size() - 1);
  const auto last_size = empty_media.size() - 8;
  empty_media[last_size + 3] = std::byte{8};
  CHECK_FALSE(video::validate_mp4(empty_media));
}

TEST_CASE("MP4 unknown payloads are opaque") {
  auto bytes = file_type();
  const auto fake_handler = video_track();
  const auto unknown = box("free", fake_handler);
  append(bytes, box("moov", unknown));
  const std::array media{std::byte{1}};
  append(bytes, box("mdat", media));
  CHECK_FALSE(video::validate_mp4(bytes));

  auto top_level_handler = file_type();
  append(top_level_handler, box("moov"));
  append(top_level_handler, video_track());
  append(top_level_handler, box("mdat", media));
  CHECK_FALSE(video::validate_mp4(top_level_handler));
}

TEST_CASE("MP4 publication validates before storage and verifies metadata") {
  const auto bytes = minimal_mp4();
  const auto expected = metadata("video-one", bytes);
  testing::ScriptedArtifactStore store{{
      {{{expected.artifact_id, "video/mp4", std::nullopt, std::nullopt,
         std::nullopt, std::nullopt},
        bytes},
       expected},
  }};
  auto published = runtime::publish_mp4_artifact(
      store, {expected.artifact_id, std::nullopt, std::nullopt}, bytes);
  REQUIRE(published);
  CHECK(*published == expected);
  CHECK(store.remaining_exchanges() == 0);

  testing::ScriptedArtifactStore unused;
  auto malformed = bytes;
  malformed.pop_back();
  auto rejected = runtime::publish_mp4_artifact(
      unused, {expected.artifact_id, std::nullopt, std::nullopt}, malformed);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        runtime::VideoArtifactErrorCode::invalid_media);
  CHECK(unused.recorded_calls().empty());

  auto conflicting = runtime::publish_mp4_artifact(
      unused,
      {expected.artifact_id, make_id<domain::InvocationId>("invocation"),
       make_id<domain::InferenceId>("inference")},
      bytes);
  REQUIRE_FALSE(conflicting);
  CHECK(conflicting.error().code ==
        runtime::VideoArtifactErrorCode::invalid_request);
  CHECK(unused.recorded_calls().empty());

  std::stop_source stopped;
  stopped.request_stop();
  auto cancelled = runtime::publish_mp4_artifact(
      unused, {expected.artifact_id, std::nullopt, std::nullopt}, bytes, {},
      stopped.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == runtime::VideoArtifactErrorCode::cancelled);
  CHECK(unused.recorded_calls().empty());

  auto check_forged = [&](domain::ArtifactMetadata forged) {
    testing::ScriptedArtifactStore forged_store{{
        {{{expected.artifact_id, "video/mp4", std::nullopt, std::nullopt,
           std::nullopt, std::nullopt},
          bytes},
         std::move(forged)},
    }};
    auto result = runtime::publish_mp4_artifact(
        forged_store, {expected.artifact_id, std::nullopt, std::nullopt},
        bytes);
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::VideoArtifactErrorCode::integrity_failure);
  };
  auto forged = expected;
  forged.artifact_id = make_id<domain::ArtifactId>("different");
  check_forged(forged);
  forged = expected;
  forged.media_type = "application/octet-stream";
  check_forged(forged);
  forged = expected;
  ++forged.byte_size;
  check_forged(forged);
  forged = expected;
  forged.digest = "sha256:" + std::string(64, 'f');
  check_forged(forged);
  forged = expected;
  forged.producing_inference_id = make_id<domain::InferenceId>("forged");
  check_forged(forged);
  forged = expected;
  forged.width = 1;
  forged.height = 1;
  check_forged(forged);

  testing::ScriptedArtifactStore failing_store{{
      {{{expected.artifact_id, "video/mp4", std::nullopt, std::nullopt,
         std::nullopt, std::nullopt},
        bytes},
       storage::ArtifactStoreError{storage::ArtifactStoreErrorCode::io_failure,
                                   "secret path and provider bytes", true}},
  }};
  rejected = runtime::publish_mp4_artifact(
      failing_store, {expected.artifact_id, std::nullopt, std::nullopt}, bytes);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == runtime::VideoArtifactErrorCode::unavailable);
  CHECK(rejected.error().retryable);
  CHECK(rejected.error().message.find("secret") == std::string::npos);
}

TEST_CASE("MP4 loading requires exact metadata digest and revalidation") {
  const auto bytes = minimal_mp4();
  const auto expected = metadata("video-load", bytes);
  testing::ScriptedArtifactStore store{
      {},
      {{expected, 32U * 1024U * 1024U,
        storage::ArtifactRead{expected, bytes}}}};
  auto loaded = runtime::load_mp4_artifact(store, expected);
  REQUIRE(loaded);
  CHECK(loaded->metadata == expected);
  CHECK(loaded->content == bytes);

  auto altered = bytes;
  altered.back() = std::byte{9};
  testing::ScriptedArtifactStore altered_store{
      {},
      {{expected, 32U * 1024U * 1024U,
        storage::ArtifactRead{expected, altered}}}};
  auto rejected = runtime::load_mp4_artifact(altered_store, expected);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        runtime::VideoArtifactErrorCode::integrity_failure);

  auto truncated = bytes;
  truncated.pop_back();
  testing::ScriptedArtifactStore truncated_store{
      {},
      {{expected, 32U * 1024U * 1024U,
        storage::ArtifactRead{expected, truncated}}}};
  rejected = runtime::load_mp4_artifact(truncated_store, expected);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        runtime::VideoArtifactErrorCode::integrity_failure);

  auto malformed = bytes;
  malformed[4] = std::byte{'x'};
  const auto malformed_metadata = metadata("malformed-video", malformed);
  testing::ScriptedArtifactStore malformed_store{
      {},
      {{malformed_metadata, 32U * 1024U * 1024U,
        storage::ArtifactRead{malformed_metadata, malformed}}}};
  rejected = runtime::load_mp4_artifact(malformed_store, malformed_metadata);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        runtime::VideoArtifactErrorCode::invalid_media);

  auto returned_metadata = expected;
  returned_metadata.artifact_id = make_id<domain::ArtifactId>("returned-other");
  testing::ScriptedArtifactStore mismatched_store{
      {},
      {{expected, 32U * 1024U * 1024U,
        storage::ArtifactRead{returned_metadata, bytes}}}};
  rejected = runtime::load_mp4_artifact(mismatched_store, expected);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        runtime::VideoArtifactErrorCode::integrity_failure);

  testing::ScriptedArtifactStore failing_store{
      {},
      {{expected, 32U * 1024U * 1024U,
        storage::ArtifactStoreError{storage::ArtifactStoreErrorCode::io_failure,
                                    "private source path", true}}}};
  rejected = runtime::load_mp4_artifact(failing_store, expected);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == runtime::VideoArtifactErrorCode::unavailable);
  CHECK(rejected.error().retryable);
  CHECK(rejected.error().message.find("private") == std::string::npos);

  std::vector<domain::ArtifactMetadata> invalid_metadata;
  auto invalid = expected;
  invalid.media_type = "application/octet-stream";
  invalid_metadata.push_back(invalid);
  invalid = expected;
  invalid.byte_size = 0;
  invalid_metadata.push_back(invalid);
  invalid = expected;
  invalid.byte_size = 32U * 1024U * 1024U + 1U;
  invalid_metadata.push_back(invalid);
  invalid = expected;
  invalid.digest = "sha256:invalid";
  invalid_metadata.push_back(invalid);
  invalid = expected;
  invalid.producing_invocation_id = make_id<domain::InvocationId>("one");
  invalid.producing_inference_id = make_id<domain::InferenceId>("two");
  invalid_metadata.push_back(invalid);
  invalid = expected;
  invalid.width = 1;
  invalid.height = 1;
  invalid_metadata.push_back(invalid);
  testing::ScriptedArtifactStore unused;
  for (const auto& candidate : invalid_metadata) {
    rejected = runtime::load_mp4_artifact(unused, candidate);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code ==
          runtime::VideoArtifactErrorCode::invalid_request);
  }

  std::stop_source stopped;
  stopped.request_stop();
  rejected =
      runtime::load_mp4_artifact(unused, expected, {}, stopped.get_token());
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == runtime::VideoArtifactErrorCode::cancelled);
}

TEST_CASE("filesystem MP4 publication deduplicates and detects corruption") {
#ifdef _WIN32
  SKIP("POSIX filesystem contract");
#else
  TemporaryDirectory temporary;
  const auto root = temporary.path() / "artifacts";
  auto store = adapters::FilesystemArtifactStore::open(root);
  REQUIRE(store);
  const auto bytes = minimal_mp4();
  auto first = runtime::publish_mp4_artifact(
      **store,
      {make_id<domain::ArtifactId>("video-first"), std::nullopt, std::nullopt},
      bytes);
  auto second = runtime::publish_mp4_artifact(
      **store,
      {make_id<domain::ArtifactId>("video-second"), std::nullopt, std::nullopt},
      bytes);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first->artifact_id != second->artifact_id);
  CHECK(first->digest == second->digest);
  auto reopened = adapters::FilesystemArtifactStore::open(
      root, {}, adapters::FilesystemArtifactStoreOpenMode::existing);
  REQUIRE(reopened);
  REQUIRE(runtime::load_mp4_artifact(**reopened, *first));

  const auto hex = first->digest.substr(7);
  const auto blob = root / "sha256" / hex.substr(0, 2) / hex;
  struct stat attributes{};
  REQUIRE(::stat(blob.c_str(), &attributes) == 0);
  CHECK((attributes.st_mode & (S_IRWXG | S_IRWXO)) == 0);
  std::fstream content(blob, std::ios::in | std::ios::out | std::ios::binary);
  REQUIRE(content);
  content.seekp(-1, std::ios::end);
  content.put(static_cast<char>(7));
  content.close();
  CHECK_FALSE(runtime::load_mp4_artifact(**reopened, *first));
#endif
}

TEST_CASE("generic SQLite and transcript contracts retain MP4 metadata") {
  TemporaryDirectory temporary;
  auto store =
      adapters::SqliteSessionStore::open(temporary.path() / "sessions.sqlite3");
  REQUIRE(store);
  const auto session = make_id<domain::SessionId>("video-session");
  const auto run = make_id<domain::RunId>("video-run");
  const auto message = make_id<domain::MessageId>("video-message");
  const auto bytes = minimal_mp4();
  const auto artifact = metadata("video-event", bytes);
  REQUIRE((*store)->create_session(
      {session, domain::EventTimestamp{std::chrono::milliseconds{1}}}));
  const std::vector<domain::RunEvent> events{
      {{make_id<domain::EventId>("event-1"), run, 1, 1,
        domain::EventTimestamp{std::chrono::milliseconds{1}}, std::nullopt,
        std::nullopt, std::nullopt},
       domain::RunStarted{make_id<domain::SurfaceId>("surface"),
                          make_id<domain::WorkspaceId>("media"),
                          make_id<domain::PermissionProfileId>("observe"),
                          std::nullopt}},
      {{make_id<domain::EventId>("event-2"), run, 2, 1,
        domain::EventTimestamp{std::chrono::milliseconds{2}}, std::nullopt,
        std::nullopt, std::nullopt},
       domain::UserContentAdded{domain::Message{message,
                                                domain::Role::user,
                                                {domain::TextBlock{"video"}},
                                                std::nullopt}}},
      {{make_id<domain::EventId>("event-3"), run, 3, 1,
        domain::EventTimestamp{std::chrono::milliseconds{3}}, std::nullopt,
        std::nullopt, std::nullopt},
       domain::ArtifactCreated{artifact}},
      {{make_id<domain::EventId>("event-4"), run, 4, 1,
        domain::EventTimestamp{std::chrono::milliseconds{4}}, std::nullopt,
        std::nullopt, std::nullopt},
       domain::ArtifactReferenced{artifact.artifact_id, message}},
  };
  REQUIRE((*store)->append_events(session, events));
  auto replayed = (*store)->replay_events(session);
  REQUIRE(replayed);
  CHECK(*replayed == events);
  auto transcript = domain::TranscriptProjection::rebuild(*replayed);
  REQUIRE(transcript);
  CHECK(std::get<domain::TranscriptMessage>(transcript->items().front())
            .artifacts == std::vector{artifact});
}

TEST_CASE("minimal bounded MP4 smoke case is accepted last") {
  const auto parsed = video::validate_mp4(minimal_mp4());
  REQUIRE(parsed);
  CHECK(parsed->major_brand == std::array{'i', 's', 'o', 'm'});
  CHECK(parsed->compatible_brand_count == 0);
  CHECK(parsed->box_count == 6);
  CHECK(parsed->track_count == 1);
}

} // namespace
