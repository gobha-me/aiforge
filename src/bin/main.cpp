#include <cstdlib>
#include <iostream>

#include <aiforge/bootstrap.hpp>

auto main() -> int {
  std::cerr << aiforge::bootstrap_status() << '\n';
  return EXIT_SUCCESS;
}
