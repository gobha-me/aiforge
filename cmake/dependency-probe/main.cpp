#if defined(PROBE_TERMFORGE)
#include <termforge/core/screen.hpp>

auto main() -> int {
  termforge::Screen screen{2, 1};
  return screen.cols() == 2 && screen.rows() == 1 ? 0 : 1;
}
#elif defined(PROBE_VENICE_CPP)
#include <venice/venice.hpp>

auto main() -> int {
  const venice::Client client{"dependency-probe-key"};
  (void)client;
  return 0;
}
#elif defined(PROBE_RASTERFORGE)
#include <rasterforge/rasterforge.hpp>

auto main() -> int {
  const auto image = rasterforge::Image::create({1, 1});
  return image && image->size_bytes() == 4 ? 0 : 1;
}
#else
#error "No dependency probe was selected"
#endif
