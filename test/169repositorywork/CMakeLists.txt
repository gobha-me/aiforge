if (WIN32 OR NOT TARGET ${PROJECT_NAME}::adapters OR NOT GIT_EXECUTABLE)
  return()
endif ()

add_executable(169repositorywork-test ../main.cpp test.cpp)
target_compile_features(169repositorywork-test PRIVATE cxx_std_23)
target_compile_definitions(169repositorywork-test
  PRIVATE REPOSITORY_TEST_GIT="$<SHELL_PATH:${GIT_EXECUTABLE}>")
target_link_libraries(169repositorywork-test PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters)
target_link_options(169repositorywork-test PRIVATE "-Wl,--wrap=close")
