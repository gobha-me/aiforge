#pragma once

#include <expected>
#include <stop_token>
#include <string>

#include <aiforge/domain/repository.hpp>

namespace aiforge::adapters {

[[nodiscard]] auto observe_process_repository(std::stop_token stop_token = {})
    -> std::expected<domain::RepositorySnapshot, std::string>;

} // namespace aiforge::adapters
