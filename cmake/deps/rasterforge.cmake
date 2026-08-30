# RasterForge: bounded raster decode and pixel transforms for image artifacts.
# Prefer an installed package, then a sibling checkout for coordinated
# development, then the compatible stable release.
if (NOT TARGET rasterforge::lib)
  find_package(rasterforge 0.5.0 CONFIG QUIET)
endif ()

if (NOT TARGET rasterforge::lib)
  set(rasterforge_BUILD_LIB ON CACHE BOOL "Build the RasterForge library" FORCE)
  set(rasterforge_BUILD_BIN OFF CACHE BOOL "Build the RasterForge executable" FORCE)
  set(rasterforge_TESTS OFF CACHE BOOL "Build the RasterForge test suite" FORCE)
  set(rasterforge_FUZZERS OFF CACHE BOOL "Build the RasterForge fuzzers" FORCE)
  set(rasterforge_BENCHMARKS OFF CACHE BOOL "Build RasterForge benchmarks" FORCE)
  set(rasterforge_INSTALL OFF CACHE BOOL "Generate RasterForge install rules" FORCE)

  if (EXISTS ${PROJECT_SOURCE_DIR}/../rasterforge/CMakeLists.txt)
    message(STATUS "Using local RasterForge at ../rasterforge")
    add_subdirectory(${PROJECT_SOURCE_DIR}/../rasterforge ${CMAKE_BINARY_DIR}/_deps/rasterforge-build)
  else ()
    include(FetchContent)
    FetchContent_Declare(rasterforge
      GIT_REPOSITORY https://github.com/gobha-me/rasterforge.git
      # v0.5.0 is the first complete decode, transform, composite, and external
      # RGBA bridge baseline required by planned media artifact consumers.
      GIT_TAG v0.5.0
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(rasterforge)
  endif ()
endif ()

if (NOT TARGET rasterforge::lib)
  message(FATAL_ERROR "RasterForge did not provide the canonical rasterforge::lib target")
endif ()
