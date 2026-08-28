#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/storage/artifact_store.hpp>

namespace aiforge::adapters {

struct ProcessToolLimits {
  std::size_t executables{64};
  std::size_t arguments{256};
  std::size_t argument_bytes{std::size_t{256} * 1024U};
  std::size_t roots{64};
  std::size_t environment_variables{64};
  std::chrono::milliseconds timeout{std::chrono::seconds{120}};
  std::size_t output_bytes{std::size_t{8} * 1024U * 1024U};
  std::size_t inline_output_bytes{std::size_t{32} * 1024U};
  std::size_t progress_chunk_bytes{std::size_t{4} * 1024U};
  std::size_t progress_events{64};
  std::chrono::milliseconds termination_grace{std::chrono::milliseconds{100}};
  auto operator==(const ProcessToolLimits&) const -> bool = default;
};

struct ProcessEnvironmentVariable {
  std::string name;
  std::string value;
  auto operator==(const ProcessEnvironmentVariable&) const -> bool = default;
};

struct ProcessToolConfiguration {
  // Filesystem roots are capability-policy ceilings and constrain the working
  // directory. They do not provide an operating-system filesystem sandbox.
  std::vector<std::filesystem::path> executable_allowlist;
  std::vector<std::filesystem::path> readable_roots;
  std::vector<std::filesystem::path> writable_roots;
  std::vector<ProcessEnvironmentVariable> environment_allowlist;
  ProcessToolLimits limits{};
  auto operator==(const ProcessToolConfiguration&) const -> bool = default;
};

[[nodiscard]] auto process_tool_declaration(
    const ProcessToolConfiguration& configuration)
    -> std::expected<backend::ToolDeclaration, runtime::ToolRegistryError>;

[[nodiscard]] auto register_process_tool(runtime::ToolRegistry& registry,
                                         storage::ArtifactStore& artifact_store,
                                         ProcessToolConfiguration configuration)
    -> std::expected<void, runtime::ToolRegistryError>;

} // namespace aiforge::adapters
