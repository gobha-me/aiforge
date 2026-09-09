add_executable(179opsobservationtool-test ../main.cpp test.cpp kernel.cpp resources.cpp)
target_include_directories(179opsobservationtool-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(179opsobservationtool-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib nlohmann_json::nlohmann_json)
