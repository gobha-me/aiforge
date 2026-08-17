cmake_minimum_required(VERSION 3.28)

# A consumed AIForge library inherits its caller's toolchain. Select our default
# only when this repository is the top-level build.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain/default.cmake)
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

project(aiforge
  VERSION ${VERSION}
  LANGUAGES CXX
)

message(STATUS "${PROJECT_NAME}:${PROJECT_VERSION}")

set(VERSION_HEADER_IN "${CMAKE_CURRENT_LIST_DIR}/include/version.hpp.in.cmake")
set(VERSION_HEADER "${PROJECT_BINARY_DIR}/generated/include/version.hpp")
configure_file(${VERSION_HEADER_IN} ${VERSION_HEADER} @ONLY)

option(${PROJECT_NAME}_BUILD_BIN "Build the AIForge executable" ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_TESTS "Build the AIForge test suite" ${PROJECT_IS_TOP_LEVEL})

# This is the dependency activation point. Recipes not named here are inert.
set(${PROJECT_NAME}_DEPS
  catch2
  cli11
  nlohmann_json
)

if (NOT ${PROJECT_NAME}_TESTS)
  list(REMOVE_ITEM ${PROJECT_NAME}_DEPS catch2)
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/dependencies.cmake)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/lib)

if (${PROJECT_NAME}_BUILD_BIN)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/bin)
endif ()

if (${PROJECT_NAME}_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
endif ()
