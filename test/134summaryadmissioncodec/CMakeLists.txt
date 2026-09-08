add_executable(134summaryadmissioncodec-test ../main.cpp test.cpp)
target_include_directories(134summaryadmissioncodec-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(134summaryadmissioncodec-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib SQLite3::SQLite3)
