# venice-cpp: header-only Venice API client.
# Prefer an installed package, then the sibling checkout for coordinated
# development, then the reviewed merged-source fallback.
if (NOT TARGET venice-cpp::lib)
  find_package(venice-cpp 0.29.18 CONFIG QUIET)
endif ()

if (venice-cpp_FOUND AND NOT TARGET venice-cpp::lib)
  message(FATAL_ERROR "venice-cpp package is missing the canonical venice-cpp::lib target")
endif ()

if (NOT TARGET venice-cpp::lib)
  block(SCOPE_FOR VARIABLES)
    # Installed dependencies found inside the embedded source must remain the
    # same canonical targets visible to AIForge's adapters and contract gate.
    # Restore the caller's normal/cache preference when acquisition finishes.
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
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
        # Delivered #120: shared header-only httplib0.51/OpenSSL3 contract.
        # Pin the reviewed merge, not an assumed future release/tag. A full SHA
        # must remain fetchable even after the remote branch advances.
        GIT_TAG 339729e945d0b3d6584702ecf49f013c1ad6779a)
      FetchContent_MakeAvailable(venice-cpp)
    endif ()
  endblock()
endif ()

if (NOT TARGET venice-cpp::lib)
  message(FATAL_ERROR "venice-cpp did not provide the canonical venice-cpp::lib target")
endif ()

include("${CMAKE_CURRENT_LIST_DIR}/../venice-transport-contract.cmake")
