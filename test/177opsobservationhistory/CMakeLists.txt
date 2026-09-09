add_executable(177opsobservationhistory-test ../main.cpp test.cpp)
target_include_directories(177opsobservationhistory-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(177opsobservationhistory-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib SQLite3::SQLite3 nlohmann_json::nlohmann_json)
