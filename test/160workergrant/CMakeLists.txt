add_executable(160workergrant-test ../main.cpp test.cpp)
target_include_directories(160workergrant-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(160workergrant-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib)
# Pull std::thread from the archive so its pthread_create call participates in
# the test's deterministic first/second thread-creation failure injection.
target_link_options(160workergrant-test
  PRIVATE -static-libstdc++ "-Wl,--wrap=pthread_create")
