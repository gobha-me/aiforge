add_executable(91playback-test
  ../main.cpp
  test.cpp
)

target_include_directories(91playback-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src/adapters
)

target_link_libraries(91playback-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)

if (${PROJECT_NAME}_AUDIO_PLAYBACK)
  target_compile_definitions(91playback-test PRIVATE AIFORGE_TEST_RTAUDIO=1)
endif ()
