#pragma once

#include <chrono>

namespace aiforge::domain {

struct RunEvent;
using EventTimestamp = std::chrono::sys_time<std::chrono::milliseconds>;

} // namespace aiforge::domain
