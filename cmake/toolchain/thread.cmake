# Thread sanitizer toolchain. Compose with Clang using CXX=clang++.
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

message(STATUS "thread sanitizer: -fsanitize=thread")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=thread")
