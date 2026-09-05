#include "runner_v3.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include <sys/stat.h>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v3 = aiforge::evaluation::process_isolation::v3;

namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-v3-runner-test-XXXXXX")
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

TEST_CASE("evidence v3 runner rejects unsafe bounds",
          "[process-isolation][evidence-v3][failure]") {
  v3::RunnerOptions options;
  options.child_executable = "relative";
  auto result = v3::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == v3::RunnerErrorCode::invalid_options);

  options.child_executable = "/bin/true";
  options.maximum_child_output_bytes = v3::maximum_child_record_bytes + 1;
  result = v3::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == v3::RunnerErrorCode::invalid_options);

  options.maximum_child_output_bytes = v3::maximum_child_record_bytes;
  options.child_timeout = std::chrono::milliseconds{60001};
  result = v3::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == v3::RunnerErrorCode::invalid_options);

  for (const auto& path : {std::filesystem::path{"relative"},
                           std::filesystem::path{"/tmp/../escape"},
                           std::filesystem::path{"/tmp/"}}) {
    options.child_timeout = std::chrono::seconds{3};
    options.delegated_cgroup_root = path;
    result = v3::run_evaluation(std::string(40, 'a'), options);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == v3::RunnerErrorCode::invalid_options);
  }
}

TEST_CASE("v3 cleanup uncertainty dominates any direct-tree result",
          "[process-isolation][evidence-v3][failure]") {
  for (const auto record : {
           v3::ProbeRecord{v3::ProbeId::direct_process_tree_cgroup_nonescape,
                           isolation::ProbeState::enforced,
                           v3::ReasonCode::none},
           v3::ProbeRecord{v3::ProbeId::direct_process_tree_cgroup_nonescape,
                           isolation::ProbeState::unavailable,
                           v3::ReasonCode::unsupported_kernel},
           v3::ProbeRecord{v3::ProbeId::direct_process_tree_cgroup_nonescape,
                           isolation::ProbeState::probe_error,
                           v3::ReasonCode::timeout},
       }) {
    CHECK(v3::test_support::cleanup_outcome(record, true) == record);
    const auto failed = v3::test_support::cleanup_outcome(record, false);
    CHECK(failed.state == isolation::ProbeState::probe_error);
    CHECK(failed.reason == v3::ReasonCode::cleanup_failed);
  }
}

TEST_CASE("v3 capability helper protocol failures are closed and cleaned",
          "[process-isolation][evidence-v3][capability][failure]") {
  TemporaryDirectory temporary;
  v3::RunnerOptions options;
  options.child_executable = "/bin/true";
  options.temporary_parent = temporary.path();
  const auto result = v3::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(result);
  REQUIRE(result->probes.size() == v3::required_probe_ids().size());
  CHECK(result->probes[1].probe_id ==
        v3::ProbeId::low_capability_nonescalation);
  CHECK(result->probes[1].state == isolation::ProbeState::probe_error);
  CHECK(result->probes[1].reason == v3::ReasonCode::malformed_protocol);
  CHECK(result->probes[2].probe_id ==
        v3::ProbeId::private_root_capability_discard);
  CHECK(result->probes[2].state == isolation::ProbeState::probe_error);
  CHECK(result->probes[2].reason == v3::ReasonCode::malformed_protocol);
  CHECK(std::filesystem::is_empty(temporary.path()));
}

TEST_CASE("v3 final temporary cleanup failure dominates every row",
          "[process-isolation][evidence-v3][failure]") {
  TemporaryDirectory temporary;
  v3::RunnerOptions options;
  options.child_executable = "/bin/true";
  options.temporary_parent = temporary.path();
  options.force_temporary_root_cleanup_failure = true;
  const auto result = v3::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(result);
  for (const auto& row : result->probes) {
    CHECK(row.state == isolation::ProbeState::probe_error);
    CHECK(row.reason == v3::ReasonCode::cleanup_failed);
  }
  CHECK(std::filesystem::is_empty(temporary.path()));
}
