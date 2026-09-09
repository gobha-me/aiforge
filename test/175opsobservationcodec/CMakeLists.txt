add_executable(175opsobservationcodec-test ../main.cpp test.cpp)
target_include_directories(175opsobservationcodec-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(175opsobservationcodec-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib nlohmann_json::nlohmann_json)
