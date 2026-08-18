add_executable(65storage-test
  ../main.cpp
  test.cpp
)

target_include_directories(65storage-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(65storage-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::lib
  PRIVATE SQLite::SQLite3
)
