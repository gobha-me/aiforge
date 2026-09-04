#include "runner_v2.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v2 = aiforge::evaluation::process_isolation::v2;

namespace {

[[nodiscard]] auto executable_directory() -> std::filesystem::path {
  std::string path(4096, '\0');
  const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
  REQUIRE(count > 0);
  REQUIRE(static_cast<std::size_t>(count) < path.size());
  path.resize(static_cast<std::size_t>(count));
  return std::filesystem::path{path}.parent_path();
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-v2-integration-XXXXXX")
                       .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
    REQUIRE(::chmod(m_path.c_str(), S_IRWXU) == 0);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

} // namespace

TEST_CASE("Linux evidence v2 emits complete bounded non-authoritative evidence",
          "[process-isolation][evidence-v2][integration]") {
  TemporaryDirectory temporary;
  v2::RunnerOptions options;
  options.child_executable =
      executable_directory() / "aiforge_process_isolation_probe_v2";
  options.temporary_parent = temporary.path();

  const auto report = v2::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(report);
  REQUIRE(v2::validate_report(*report));
  REQUIRE(report->probes.size() == v2::required_probe_ids().size());
  for (std::size_t index{}; index < report->probes.size(); ++index) {
    CAPTURE(v2::probe_id_name(report->probes[index].probe_id));
    CAPTURE(isolation::probe_state_name(report->probes[index].state));
    CAPTURE(v2::reason_code_name(report->probes[index].reason));
    CHECK(report->probes[index].probe_id == v2::required_probe_ids()[index]);
    CHECK(report->probes[index].state != isolation::ProbeState::probe_error);
  }
  CHECK(report->platform == "linux");
  CHECK(report->source_sha == std::string(40, 'a'));
  CHECK(std::filesystem::is_empty(temporary.path()));

  const auto encoded = v2::serialize_report(*report);
  REQUIRE(encoded);
  CHECK(encoded->size() <= v2::maximum_report_bytes);
  CHECK(encoded->find("isolation_level") == std::string::npos);
  const auto decoded = v2::parse_report(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == *report);
  CHECK(v2::serialize_report(*decoded) == encoded);
}
