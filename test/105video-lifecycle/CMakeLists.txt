add_executable(105video-lifecycle-test
  ../main.cpp
  test.cpp
)

target_include_directories(105video-lifecycle-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(105video-lifecycle-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::lib
)
