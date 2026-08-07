# termforge: terminal UI framework.
# Prefer the sibling local checkout; fall back to FetchContent from GitHub.
if (NOT TARGET termforge_lib)
  if (EXISTS ${CMAKE_SOURCE_DIR}/../termforge/CMakeLists.txt)
    message(STATUS "Using local termforge at ../termforge")
    add_subdirectory(${CMAKE_SOURCE_DIR}/../termforge ${CMAKE_BINARY_DIR}/_deps/termforge-build)
  else ()
    include(FetchContent)
    FetchContent_Declare(termforge
      GIT_REPOSITORY https://github.com/gobha-me/termforge.git
      GIT_TAG main)
    FetchContent_MakeAvailable(termforge)
  endif()
endif()
