add_library(${PROJECT_NAME}_adapters STATIC
  ask_user_dialog.cpp
  filesystem_persona_source.cpp
  git_repository_snapshot_source.cpp
  process_tool.cpp
  process_one_shot.cpp
  process_provenance.cpp
  process_draft_editor.cpp
  process_interactive.cpp
  termforge_run_bridge.cpp
  transcript_view.cpp
  tui_scenario.cpp
  venice_backend.cpp
)
add_library(${PROJECT_NAME}::adapters ALIAS ${PROJECT_NAME}_adapters)

target_include_directories(${PROJECT_NAME}_adapters
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(${PROJECT_NAME}_adapters PUBLIC cxx_std_23)
target_link_libraries(${PROJECT_NAME}_adapters
  PUBLIC ${PROJECT_NAME}::lib
  PUBLIC termforge::lib
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE venice-cpp::lib
)
