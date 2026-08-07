# venice-cpp: header-only Venice API client.
# Prefer the sibling local checkout (fast dev loop when both are changing);
# fall back to FetchContent from GitHub for standalone builds.
if (NOT TARGET venice-cpp_lib)
  if (EXISTS ${CMAKE_SOURCE_DIR}/../venice-cpp/CMakeLists.txt)
    message(STATUS "Using local venice-cpp at ../venice-cpp")
    add_subdirectory(${CMAKE_SOURCE_DIR}/../venice-cpp ${CMAKE_BINARY_DIR}/_deps/venice-cpp-build)
  else ()
    include(FetchContent)
    FetchContent_Declare(venice-cpp
      GIT_REPOSITORY https://github.com/gobha-me/venice-cpp.git
      GIT_TAG main)
    FetchContent_MakeAvailable(venice-cpp)
  endif()
endif()
