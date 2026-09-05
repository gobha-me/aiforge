#include "runner_v3.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v3 = aiforge::evaluation::process_isolation::v3;

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
                    "aiforge-v3-integration-XXXXXX")
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

TEST_CASE("Linux evidence v3 emits a complete bounded supplemental report",
          "[process-isolation][evidence-v3][integration]") {
  TemporaryDirectory temporary;
  v3::RunnerOptions options;
  options.child_executable =
      executable_directory() / "aiforge_process_isolation_probe_v3";
  options.temporary_parent = temporary.path();

  const auto report = v3::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(report);
  REQUIRE(v3::validate_report(*report));
  REQUIRE(report->probes.size() == v3::required_probe_ids().size());
  CHECK(report->probes[0].probe_id ==
        v3::ProbeId::direct_process_tree_cgroup_nonescape);
  CHECK(report->probes[0].state == isolation::ProbeState::unavailable);
  CHECK(report->probes[0].reason == v3::ReasonCode::missing_delegation);
  CHECK(report->probes[1].probe_id ==
        v3::ProbeId::low_capability_nonescalation);
  CHECK(report->probes[1].state == isolation::ProbeState::enforced);
  CHECK(report->probes[1].reason == v3::ReasonCode::none);
  CHECK(report->probes[2].probe_id ==
        v3::ProbeId::private_root_capability_discard);
  if (report->probes[2].state == isolation::ProbeState::enforced) {
    CHECK(report->probes[2].reason == v3::ReasonCode::none);
  } else {
    CHECK(report->probes[2].state == isolation::ProbeState::unavailable);
    CHECK(
        (report->probes[2].reason == v3::ReasonCode::unsupported_kernel ||
         report->probes[2].reason == v3::ReasonCode::unsupported_architecture ||
         report->probes[2].reason == v3::ReasonCode::permission_denied ||
         report->probes[2].reason == v3::ReasonCode::mechanism_absent ||
         report->probes[2].reason == v3::ReasonCode::prerequisite_unavailable ||
         report->probes[2].reason == v3::ReasonCode::enforcement_failed));
  }
  CHECK(report->platform == "linux");
  CHECK(report->source_sha == std::string(40, 'a'));
  CHECK(std::filesystem::is_empty(temporary.path()));

  const auto encoded = v3::serialize_report(*report);
  REQUIRE(encoded);
  CHECK(encoded->size() <= v3::maximum_report_bytes);
  CHECK(encoded->find("isolation_level") == std::string::npos);
  const auto decoded = v3::parse_report(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == *report);
}
