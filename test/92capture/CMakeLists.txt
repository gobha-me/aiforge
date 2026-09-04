add_executable(92capture-test
  ../main.cpp
  test.cpp
)

target_include_directories(92capture-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src/adapters
)

target_link_libraries(92capture-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)

if (${PROJECT_NAME}_AUDIO_CAPTURE)
  target_compile_definitions(92capture-test PRIVATE AIFORGE_TEST_RTAUDIO_CAPTURE=1)
endif ()
