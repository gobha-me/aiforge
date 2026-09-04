#pragma once

#include "evidence.hpp"

#include <filesystem>

namespace aiforge::evaluation::process_isolation {

[[nodiscard]] auto child_process_is_sanitized() noexcept -> bool;

[[nodiscard]] auto run_probe(ProbeId probe_id,
                             const std::filesystem::path& state_directory)
    -> ProbeRecord;

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

[[nodiscard]] auto callable_assertion_failure() -> ProbeRecord;
[[nodiscard]] auto runtime_permission_denial() -> ProbeRecord;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation
