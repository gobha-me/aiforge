add_executable(104mp4-test
  ../main.cpp
  test.cpp
)

target_include_directories(104mp4-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(104mp4-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
