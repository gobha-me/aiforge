#pragma once

#include "evidence_v3.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace aiforge::evaluation::process_isolation::v3 {

struct RunnerOptions {
  std::filesystem::path child_executable;
  std::vector<std::string> child_argument_prefix;
  std::filesystem::path temporary_parent;
  std::filesystem::path delegated_cgroup_root;
  std::chrono::milliseconds child_timeout{std::chrono::seconds{15}};
  std::size_t maximum_child_output_bytes{maximum_child_record_bytes};
#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
  bool force_temporary_root_cleanup_failure{};
#endif
};

enum class RunnerErrorCode {
  invalid_options,
  platform_metadata,
  internal_error,
};

struct RunnerError {
  RunnerErrorCode code{RunnerErrorCode::internal_error};
  std::string message;
  auto operator==(const RunnerError&) const -> bool = default;
};

[[nodiscard]] auto run_evaluation(std::string source_sha,
                                  const RunnerOptions& options,
                                  std::stop_token stop_token = {})
    -> std::expected<EvidenceReport, RunnerError>;

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

[[nodiscard]] auto cleanup_outcome(ProbeRecord record, bool cleanup_complete)
    -> ProbeRecord;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v3
