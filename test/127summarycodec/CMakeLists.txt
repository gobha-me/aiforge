add_executable(127summarycodec-test ../main.cpp test.cpp)
target_include_directories(127summarycodec-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(127summarycodec-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib SQLite3::SQLite3)
