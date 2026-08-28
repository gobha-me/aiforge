#pragma once

#include <cstdint>
#include <string>

namespace aiforge::domain {

struct ContentDigest {
  std::string algorithm;
  std::string value;
  std::uint64_t byte_size{};
  auto operator==(const ContentDigest&) const -> bool = default;
};

} // namespace aiforge::domain
