#include "fixture.hpp"
#include <aiforge/adapters/local_file_source.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace folder_grant_test;
TEST_CASE("folder grant validates private bounded rootless requests") {
  auto value = request();
  SECTION("epoch") {
    value.token.session_epoch = 0;
  }
  SECTION("request") {
    value.token.request_id = 0;
  }
  SECTION("generation") {
    value.lease_generation = 0;
  }
  SECTION("empty path") {
    value.absolute_path.clear();
  }
  SECTION("relative path") {
    value.absolute_path = "relative";
  }
  SECTION("oversized path") {
    value.absolute_path = "/" + std::string(4096, 'x');
  }
  SECTION("control path") {
    value.absolute_path = "/private\nsecret";
  }
  SECTION("embedded nul") {
    value.absolute_path = std::string("/private\0secret", 15);
  }
  SECTION("invalid utf8") {
    value.absolute_path = "/\xff";
  }
  SECTION("invalid limits") {
    value.limits.maximum_roots = 0;
  }
  const auto invalid = runtime::validate_local_folder_grant_request(value);
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().message.find("private") == std::string::npos);
}
TEST_CASE("folder grant refuses forged and unpinned lease results") {
  const auto input = request();
  auto lease = std::make_shared<Lease>();
  runtime::LocalFolderGrantResult result{input.token, lease->root, lease};
  SECTION("null") {
    result.lease.reset();
  }
  SECTION("foreign epoch") {
    ++result.token.session_epoch;
  }
  SECTION("foreign request") {
    ++result.token.request_id;
  }
  SECTION("foreign session") {
    result.token.session_id = domain::SessionId::from("other").value();
  }
  SECTION("invalid root") {
    result.root.binding = "invalid";
  }
  SECTION("mismatched root") {
    result.root.binding = std::string(64, 'b');
  }
  SECTION("lease session") {
    lease->session = domain::SessionId::from("other").value();
  }
  SECTION("lease generation") {
    ++lease->generation;
  }
  SECTION("unpinned") {
    lease->pinned = false;
  }
  REQUIRE_FALSE(runtime::validate_local_folder_grant_result(input, result));
}
TEST_CASE("neutral folder factory refuses cancelled and unavailable grants") {
  adapters::LocalFileGrantFactory factory;
  auto value = request();
  std::stop_source stop;
  SECTION("cancelled") {
    stop.request_stop();
  }
  SECTION("missing root") {
    value.absolute_path = "/aiforge-missing-folder-grant-fixture";
  }
  auto result = factory.grant(value, stop.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find(value.absolute_path) == std::string::npos);
}
TEST_CASE(
    "neutral folder lease lists explicit root and revokes read authority") {
  auto pattern =
      (std::filesystem::temp_directory_path() / "aiforge-folder-grant-XXXXXX")
          .string();
  REQUIRE(::mkdtemp(pattern.data()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{pattern};
  {
    std::ofstream file{cleanup.path / "notes.txt"};
    file << "hello";
    REQUIRE(file.good());
  }
  auto value = request();
  value.absolute_path = pattern;
  adapters::LocalFileGrantFactory factory;
  auto result = factory.grant(value);
  INFO((result ? "granted" : result.error().message));
  REQUIRE(result);
  REQUIRE(runtime::validate_local_folder_grant_result(value, *result));
  runtime::LocalListRequest listing{
      {value.token.session_id, result->root, value.lease_generation, 2, 1},
      "",
      {}};
  auto entries = result->lease->list(listing);
  REQUIRE(entries);
  REQUIRE(entries->entries.size() == 1);
  CHECK(entries->entries.front().relative_path == "notes.txt");
  result->lease->revoke();
  const auto denied = result->lease->list(listing);
  REQUIRE_FALSE(denied);
  CHECK(denied.error().code == domain::LocalSourceErrorCode::stale_lease);
}
