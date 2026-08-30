add_executable(90audio-test
  ../main.cpp
  test.cpp
)

target_include_directories(90audio-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(90audio-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
  PRIVATE httplib::httplib
)
