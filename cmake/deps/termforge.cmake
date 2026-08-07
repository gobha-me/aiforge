# termforge: terminal UI framework. Prefer an installed package, then a sibling
# checkout for coordinated development, then the compatible stable release.
if (NOT TARGET termforge::lib)
  find_package(termforge CONFIG QUIET)
endif ()

if (NOT TARGET termforge::lib)
  set(termforge_TESTS OFF CACHE BOOL "Build the termforge test suite" FORCE)
  set(termforge_EXAMPLES OFF CACHE BOOL "Build the termforge examples" FORCE)
  set(termforge_BIN OFF CACHE BOOL "Build the termforge demo binary" FORCE)
  set(termforge_INSTALL OFF CACHE BOOL "Generate termforge install rules" FORCE)

  if (EXISTS ${PROJECT_SOURCE_DIR}/../termforge/CMakeLists.txt)
    message(STATUS "Using local termforge at ../termforge")
    add_subdirectory(${PROJECT_SOURCE_DIR}/../termforge ${CMAKE_BINARY_DIR}/_deps/termforge-build)
  else ()
    include(FetchContent)
    FetchContent_Declare(termforge
      GIT_REPOSITORY https://github.com/gobha-me/termforge.git
      GIT_TAG v0.7.2
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(termforge)
  endif ()
endif ()

if (NOT TARGET termforge::lib)
  message(FATAL_ERROR "termforge did not provide the canonical termforge::lib target")
endif ()
