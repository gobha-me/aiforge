#pragma once

#include <string_view>

namespace aiforge {

[[nodiscard]] auto bootstrap_status() noexcept -> std::string_view;

}  // namespace aiforge
