if (WIN32 OR NOT TARGET ${PROJECT_NAME}::adapters OR NOT GIT_EXECUTABLE)
  return()
endif ()

add_executable(78repositoryadapter-test
  ../main.cpp
  test.cpp
)
target_compile_features(78repositoryadapter-test PRIVATE cxx_std_23)
target_compile_definitions(78repositoryadapter-test
  PRIVATE REPOSITORY_TEST_GIT="$<SHELL_PATH:${GIT_EXECUTABLE}>")
target_link_libraries(78repositoryadapter-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
