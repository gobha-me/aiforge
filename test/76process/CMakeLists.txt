if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(process-test-fixture fixture.cpp)
target_compile_features(process-test-fixture PRIVATE cxx_std_23)

add_executable(76process-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  test.cpp
)
target_compile_features(76process-test PRIVATE cxx_std_23)
target_compile_definitions(76process-test
  PRIVATE PROCESS_TEST_FIXTURE="$<TARGET_FILE:process-test-fixture>")
target_link_libraries(76process-test
  PRIVATE Catch2::Catch2
  PRIVATE aiforge::adapters
  PRIVATE nlohmann_json::nlohmann_json
)
add_dependencies(76process-test process-test-fixture)
