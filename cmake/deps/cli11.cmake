# CLI11: command-line tokenization and grammar adapter.
# v2.7.2 is the compatibility pin accepted by ADR 0003.
if (NOT TARGET CLI11::CLI11)
  find_package(CLI11 2.7 CONFIG QUIET)
endif ()

if (NOT TARGET CLI11::CLI11)
  set(CLI11_BUILD_DOCS OFF CACHE BOOL "Build CLI11 documentation" FORCE)
  set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "Build CLI11 examples" FORCE)
  set(CLI11_BUILD_TESTS OFF CACHE BOOL "Build CLI11 tests" FORCE)
  set(CLI11_INSTALL OFF CACHE BOOL "Generate CLI11 install rules" FORCE)
  set(CLI11_MODULES OFF CACHE BOOL "Build CLI11 as a C++ module" FORCE)
  set(CLI11_PRECOMPILED OFF CACHE BOOL "Build precompiled CLI11" FORCE)
  set(CLI11_SINGLE_FILE OFF CACHE BOOL "Generate CLI11 single header" FORCE)

  include(FetchContent)
  FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.7.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(CLI11)
endif ()

if (NOT TARGET CLI11::CLI11)
  message(FATAL_ERROR "CLI11 did not provide the canonical CLI11::CLI11 target")
endif ()
