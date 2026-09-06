#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string_view>

#include <aiforge/cli/command_registry.hpp>

namespace aiforge::adapters::detail {

using SecureExportWriteOperation =
    std::function<std::expected<std::size_t, int>(int,
                                                  std::span<const std::byte>)>;

struct SecureExportTestHooks {
  SecureExportWriteOperation write;
  std::function<void(int, std::string_view)> before_publish;
  std::function<void(int, std::string_view)> after_publish;
  std::function<void(int)> before_cleanup;
};

[[nodiscard]] auto secure_export_bytes(std::span<const std::byte> content,
                                       const std::filesystem::path& path,
                                       std::stop_token stop_token = {},
                                       SecureExportTestHooks hooks = {})
    -> std::expected<void, cli::CommandFailure>;

} // namespace aiforge::adapters::detail
