#pragma once
#include <aiforge/surfaces/context_control.hpp>
#include <string_view>
namespace aiforge::adapters {
[[nodiscard]] auto parse_context_request(std::string_view line)
    -> std::expected<surfaces::ContextRequest, surfaces::ContextFailure>;
[[nodiscard]] auto encode_context_reply(const surfaces::ContextReply& reply)
    -> std::expected<std::string, surfaces::ContextFailure>;
} // namespace aiforge::adapters
