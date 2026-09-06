add_executable(106video-presentation-test
  ../main.cpp
  test.cpp
)

target_include_directories(106video-presentation-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src/adapters
)

target_link_libraries(106video-presentation-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
