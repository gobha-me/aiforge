#pragma once

#include <sys/prctl.h>
#include <sys/resource.h>

namespace aiforge::evaluation::audio_device {

// Linux resets process dumpability across exec. Each probe therefore reapplies
// both controls before constructing a backend or handling evaluation data.
[[nodiscard]] inline auto harden_probe_process() noexcept -> bool {
  const rlimit no_core{0, 0};
  return ::setrlimit(RLIMIT_CORE, &no_core) == 0 &&
         ::prctl(PR_SET_DUMPABLE, 0) == 0;
}

} // namespace aiforge::evaluation::audio_device
