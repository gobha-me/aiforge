if (WIN32 OR NOT TARGET ${PROJECT_NAME}::adapters OR NOT GIT_EXECUTABLE)
  return()
endif ()

add_executable(116devsurface-test ../main.cpp test.cpp)
target_compile_features(116devsurface-test PRIVATE cxx_std_23)
target_compile_definitions(116devsurface-test
  PRIVATE REPOSITORY_TEST_GIT="$<SHELL_PATH:${GIT_EXECUTABLE}>")
target_link_libraries(116devsurface-test PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters)
