add_library(aiforge_audio_device_evaluation_support STATIC
  evidence.cpp
  runner.cpp
)
target_compile_features(aiforge_audio_device_evaluation_support
  PUBLIC cxx_std_23
)
target_include_directories(aiforge_audio_device_evaluation_support
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(aiforge_audio_device_evaluation_support
  PRIVATE nlohmann_json::nlohmann_json
)

add_library(aiforge_audio_device_contract STATIC
  device_contract.cpp
  scripted_device.cpp
)
target_compile_features(aiforge_audio_device_contract PUBLIC cxx_std_23)
target_include_directories(aiforge_audio_device_contract
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
)

add_executable(aiforge_audio_device_contract_probe contract_main.cpp)
target_compile_features(aiforge_audio_device_contract_probe PRIVATE cxx_std_23)
target_link_libraries(aiforge_audio_device_contract_probe
  PRIVATE aiforge_audio_device_contract
  PRIVATE aiforge_audio_device_evaluation_support
  PRIVATE ${PROJECT_NAME}::lib
)

add_executable(aiforge_audio_device_rtaudio_probe rtaudio_main.cpp)
target_compile_features(aiforge_audio_device_rtaudio_probe PRIVATE cxx_std_23)
target_link_libraries(aiforge_audio_device_rtaudio_probe
  PRIVATE aiforge_audio_device_evaluation_support
  PRIVATE RtAudio::rtaudio
)

add_executable(aiforge_audio_device_miniaudio_probe miniaudio_main.cpp)
target_compile_features(aiforge_audio_device_miniaudio_probe PRIVATE cxx_std_23)
target_link_libraries(aiforge_audio_device_miniaudio_probe
  PRIVATE aiforge_audio_device_evaluation_support
  PRIVATE miniaudio::miniaudio
)

add_executable(aiforge_audio_device_evaluation main.cpp)
target_compile_features(aiforge_audio_device_evaluation PRIVATE cxx_std_23)
target_link_libraries(aiforge_audio_device_evaluation
  PRIVATE aiforge_audio_device_evaluation_support
)
add_dependencies(aiforge_audio_device_evaluation
  aiforge_audio_device_contract_probe
  aiforge_audio_device_rtaudio_probe
  aiforge_audio_device_miniaudio_probe
)

if (${PROJECT_NAME}_TESTS)
  add_executable(audio-device-evaluation-test
    ${PROJECT_SOURCE_DIR}/test/main.cpp
    test_contract.cpp
    test_evidence.cpp
    test_integration.cpp
    test_runner.cpp
  )
  target_compile_features(audio-device-evaluation-test PRIVATE cxx_std_23)
  target_link_libraries(audio-device-evaluation-test
    PRIVATE Catch2::Catch2
    PRIVATE aiforge_audio_device_contract
    PRIVATE aiforge_audio_device_evaluation_support
  )
  add_dependencies(audio-device-evaluation-test
    aiforge_audio_device_evaluation
    aiforge_audio_device_contract_probe
    aiforge_audio_device_rtaudio_probe
    aiforge_audio_device_miniaudio_probe
  )
  add_test(NAME audio-device-evaluation COMMAND audio-device-evaluation-test)
  set_tests_properties(audio-device-evaluation PROPERTIES TIMEOUT 30)
endif ()
