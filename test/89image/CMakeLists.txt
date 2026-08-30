add_executable(89image-test
  ../main.cpp
  test.cpp
)

target_include_directories(89image-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(89image-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
  PRIVATE httplib::httplib
)
