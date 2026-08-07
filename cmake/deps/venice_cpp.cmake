# venice-cpp: header-only Venice API client.
# Prefer an installed package, then the sibling checkout for coordinated
# development, then the compatible stable release.
if (NOT TARGET venice-cpp::lib)
  find_package(venice-cpp CONFIG QUIET)
endif ()

if (NOT TARGET venice-cpp::lib)
  set(venice-cpp_BUILD_BIN OFF CACHE BOOL "Build the venice-cpp smoke binary" FORCE)
  set(venice-cpp_TESTS OFF CACHE BOOL "Build the venice-cpp test suite" FORCE)
  set(venice-cpp_INSTALL OFF CACHE BOOL "Generate venice-cpp install rules" FORCE)

  if (EXISTS ${PROJECT_SOURCE_DIR}/../venice-cpp/CMakeLists.txt)
    message(STATUS "Using local venice-cpp at ../venice-cpp")
    add_subdirectory(${PROJECT_SOURCE_DIR}/../venice-cpp ${CMAKE_BINARY_DIR}/_deps/venice-cpp-build)
  else ()
    include(FetchContent)
    FetchContent_Declare(venice-cpp
      GIT_REPOSITORY https://github.com/gobha-me/venice-cpp.git
      GIT_TAG v0.5.0
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(venice-cpp)
  endif ()
endif ()

if (NOT TARGET venice-cpp::lib)
  message(FATAL_ERROR "venice-cpp did not provide the canonical venice-cpp::lib target")
endif ()
