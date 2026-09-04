if (NOT TARGET ${PROJECT_NAME}::adapters OR NOT GIT_EXECUTABLE)
  return()
endif ()

add_executable(97planprocess-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  test.cpp
)
target_compile_features(97planprocess-test PRIVATE cxx_std_23)
target_compile_definitions(97planprocess-test
  PRIVATE PLAN_PROCESS_TEST_GIT="$<SHELL_PATH:${GIT_EXECUTABLE}>")
target_link_libraries(97planprocess-test
  PRIVATE Catch2::Catch2
  PRIVATE aiforge::adapters
  PRIVATE nlohmann_json::nlohmann_json
)
