#pragma once

#include "evidence_v3.hpp"

#include <filesystem>
#include <string_view>

namespace aiforge::evaluation::process_isolation::v3 {

[[nodiscard]] auto run_low_capability_probe(
    const std::filesystem::path& state_directory) -> ProbeRecord;

[[nodiscard]] auto run_low_capability_payload(
    std::string_view cap_last_document, std::string_view bounding_document)
    -> int;

[[nodiscard]] auto run_private_root_capability_probe(
    const std::filesystem::path& state_directory) -> ProbeRecord;
[[nodiscard]] auto run_private_root_capability_payload(
    std::string_view cap_last_document) -> int;

} // namespace aiforge::evaluation::process_isolation::v3
