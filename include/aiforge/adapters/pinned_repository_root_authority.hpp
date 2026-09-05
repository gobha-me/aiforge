#pragma once

#include <expected>
#include <filesystem>
#include <memory>

#include <aiforge/runtime/automatic_approval_matcher.hpp>

namespace aiforge::adapters {

// Pins one absolute repository root for the application lifetime. Every match
// reopens the recorded root chain without following symlinks and traverses the
// candidate relative to that verified descriptor.
[[nodiscard]] auto open_pinned_repository_root_authority(
    std::filesystem::path repository_root)
    -> std::expected<
        std::shared_ptr<const runtime::DescriptorRelativePathAuthority>,
        runtime::AutomaticApprovalMatcherError>;

} // namespace aiforge::adapters
