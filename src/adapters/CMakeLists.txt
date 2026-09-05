add_library(${PROJECT_NAME}_adapters STATIC
  audio_device_gate.cpp
  audio_backend.cpp
  capture_device.cpp
  ask_user_dialog.cpp
  filesystem_artifact_store.cpp
  filesystem_persona_source.cpp
  filesystem_user_global_instruction_source.cpp
  git_repository_snapshot_source.cpp
  json_model_catalog_cache.cpp
  model_picker_dialog.cpp
  persona_editor_dialog.cpp
  pinned_repository_root_authority.cpp
  provider_character_picker_dialog.cpp
  process_model_catalog.cpp
  process_audio.cpp
  process_credentials.cpp
  process_login.cpp
  process_models.cpp
  process_tool.cpp
  process_one_shot.cpp
  process_plan.cpp
  playback_device.cpp
  process_provenance.cpp
  process_repository.cpp
  process_draft_editor.cpp
  process_interactive.cpp
  process_image.cpp
  termforge_run_bridge.cpp
  termforge_image_renderer.cpp
  tool_approval_dialog.cpp
  transcript_view.cpp
  tui_scenario.cpp
  venice_backend.cpp
  venice_generation_options.cpp
  venice_audio_service.cpp
  venice_image_generator.cpp
  image_backend.cpp
  image_tool.cpp
  venice_model_catalog_source.cpp
)
add_library(${PROJECT_NAME}::adapters ALIAS ${PROJECT_NAME}_adapters)

if (${PROJECT_NAME}_AUDIO_PLAYBACK)
  target_sources(${PROJECT_NAME}_adapters PRIVATE rtaudio_playback.cpp)
  target_compile_definitions(${PROJECT_NAME}_adapters
    PRIVATE AIFORGE_HAS_RTAUDIO_PLAYBACK=1)
  target_link_libraries(${PROJECT_NAME}_adapters PRIVATE RtAudio::rtaudio)
endif ()

if (${PROJECT_NAME}_AUDIO_CAPTURE)
  target_sources(${PROJECT_NAME}_adapters PRIVATE rtaudio_capture.cpp)
  target_compile_definitions(${PROJECT_NAME}_adapters
    PRIVATE AIFORGE_HAS_RTAUDIO_CAPTURE=1)
  target_link_libraries(${PROJECT_NAME}_adapters PRIVATE RtAudio::rtaudio)
endif ()

target_include_directories(${PROJECT_NAME}_adapters
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(${PROJECT_NAME}_adapters PUBLIC cxx_std_23)
target_link_libraries(${PROJECT_NAME}_adapters
  PUBLIC ${PROJECT_NAME}::lib
  PUBLIC termforge::lib
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE rasterforge::lib
  PRIVATE venice-cpp::lib
)
