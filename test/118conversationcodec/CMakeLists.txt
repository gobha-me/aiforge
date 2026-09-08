add_executable(118conversationcodec-test ../main.cpp test.cpp)
target_include_directories(118conversationcodec-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(118conversationcodec-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib SQLite3::SQLite3)
