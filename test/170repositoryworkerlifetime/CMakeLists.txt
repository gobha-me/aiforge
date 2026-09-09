add_executable(170repositoryworkerlifetime-test ../main.cpp test.cpp)
target_include_directories(170repositoryworkerlifetime-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(170repositoryworkerlifetime-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib)
target_link_options(170repositoryworkerlifetime-test
  PRIVATE -static-libstdc++ "-Wl,--wrap=pthread_create")
