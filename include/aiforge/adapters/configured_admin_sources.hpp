#pragma once

#include <aiforge/config/config.hpp>
#include <aiforge/surfaces/admin_controller.hpp>

namespace aiforge::adapters {
// Owns validated configuration records. Making/listing the catalog and creating
// its native preparation factory perform no source or referenced-file IO.
[[nodiscard]] auto make_configured_admin_sources(
    const config::ResolvedConfig& resolved) noexcept
    -> std::expected<std::shared_ptr<surfaces::AdminSourceCatalog>,
                     surfaces::ManualOpsFailure>;
// Reads the actual process configuration once. Only a successfully loaded empty
// file layer may yield the built-in local target; failures never fall back.
[[nodiscard]] auto load_process_admin_sources() noexcept
    -> std::expected<std::shared_ptr<surfaces::AdminSourceCatalog>,
                     surfaces::ManualOpsFailure>;
} // namespace aiforge::adapters
