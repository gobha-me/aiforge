#include "run_metadata.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using aiforge::evaluation::drawforge::record_accounting_and_validate_submission;
using aiforge::evaluation::drawforge::RunAccounting;
using aiforge::evaluation::drawforge::RunFinalizationErrorCode;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = fs::temp_directory_path() /
             ("aiforge-drawforge-metadata-" + std::to_string(unique));
    fs::create_directories(m_path);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(m_path, error);
  }

  [[nodiscard]] auto path() const -> const fs::path& { return m_path; }

 private:
  fs::path m_path;
};

auto write_json(const fs::path& path, const Json& value) -> void {
  std::ofstream output{path};
  REQUIRE(output);
  output << value.dump() << '\n';
  REQUIRE(output);
}

auto read_json(const fs::path& path) -> Json {
  std::ifstream input{path};
  REQUIRE(input);
  return Json::parse(input);
}

auto metadata(Json events = Json::array()) -> Json {
  return {{"schema_version", 2},
          {"usage", {{"cost_usd", nullptr}}},
          {"events", std::move(events)}};
}

constexpr RunAccounting accounting{123, 45, 0.125};

} // namespace

TEST_CASE("DrawForge run accounting survives missing acceptance",
          "[drawforge][evaluation][accounting][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "run.json";
  write_json(path, metadata());

  const auto result =
      record_accounting_and_validate_submission(path, accounting);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == RunFinalizationErrorCode::submission_missing);

  const auto persisted = read_json(path);
  REQUIRE(persisted["usage"]["input_tokens"] == 123);
  REQUIRE(persisted["usage"]["output_tokens"] == 45);
  REQUIRE(persisted["usage"]["cost_usd"] == 0.125);
}

TEST_CASE("DrawForge run accounting survives malformed acceptance evidence",
          "[drawforge][evaluation][accounting][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "run.json";

  SECTION("events are missing") {
    auto value = metadata();
    value.erase("events");
    write_json(path, value);
  }

  SECTION("events have the wrong type") {
    write_json(path, metadata("submission_accepted"));
  }

  const auto result =
      record_accounting_and_validate_submission(path, accounting);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == RunFinalizationErrorCode::submission_missing);
  REQUIRE(read_json(path)["usage"]["cost_usd"] == 0.125);
}

TEST_CASE("DrawForge accepted run records accounting",
          "[drawforge][evaluation][accounting]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "run.json";
  write_json(path, metadata(Json::array({"submission_accepted"})));

  REQUIRE(record_accounting_and_validate_submission(path, accounting));
  const auto persisted = read_json(path);
  REQUIRE(persisted["usage"]["input_tokens"] == 123);
  REQUIRE(persisted["usage"]["output_tokens"] == 45);
  REQUIRE(persisted["usage"]["cost_usd"] == 0.125);
}

TEST_CASE("DrawForge run finalization fails closed on metadata IO",
          "[drawforge][evaluation][accounting][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "run.json";

  SECTION("metadata is unreadable") {
    const auto result =
        record_accounting_and_validate_submission(path, accounting);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            RunFinalizationErrorCode::metadata_unavailable);
  }

  SECTION("metadata replacement fails") {
    write_json(path, metadata(Json::array({"submission_accepted"})));
    fs::create_directory(path.string() + ".new");
    const auto result =
        record_accounting_and_validate_submission(path, accounting);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            RunFinalizationErrorCode::metadata_write_failed);
    REQUIRE(read_json(path)["usage"]["cost_usd"].is_null());
  }
}
