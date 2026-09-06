if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
    NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(linux-launcher-test-fixture fixture.cpp)
target_compile_features(linux-launcher-test-fixture PRIVATE cxx_std_23)

add_executable(108linuxlauncher-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  test.cpp
)
target_compile_features(108linuxlauncher-test PRIVATE cxx_std_23)
target_compile_definitions(108linuxlauncher-test
  PRIVATE LINUX_LAUNCHER_TEST_FIXTURE="$<TARGET_FILE:linux-launcher-test-fixture>")
target_link_libraries(108linuxlauncher-test
  PRIVATE Catch2::Catch2
  PRIVATE aiforge::adapters
)
add_dependencies(108linuxlauncher-test linux-launcher-test-fixture)
