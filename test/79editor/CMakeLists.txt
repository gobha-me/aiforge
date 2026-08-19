if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(editor-test-fixture fixture.cpp)
target_compile_features(editor-test-fixture PRIVATE cxx_std_23)

add_executable(79editor-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  test.cpp
)
target_compile_features(79editor-test PRIVATE cxx_std_23)
target_compile_definitions(79editor-test
  PRIVATE EDITOR_TEST_FIXTURE="$<TARGET_FILE:editor-test-fixture>")
target_link_libraries(79editor-test
  PRIVATE Catch2::Catch2
  PRIVATE aiforge::adapters
)
add_dependencies(79editor-test editor-test-fixture)
