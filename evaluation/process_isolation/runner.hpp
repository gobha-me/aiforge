#pragma once

#include "evidence.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace aiforge::evaluation::process_isolation {

struct RunnerOptions {
  std::filesystem::path child_executable;
  std::vector<std::string> child_argument_prefix;
  std::filesystem::path temporary_parent;
  std::chrono::milliseconds child_timeout{std::chrono::seconds{3}};
  std::size_t maximum_child_output_bytes{maximum_child_record_bytes};
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
                                  const RunnerOptions& options)
    -> std::expected<EvidenceReport, RunnerError>;

} // namespace aiforge::evaluation::process_isolation
