add_library(${PROJECT_NAME}_lib STATIC
  ask_user_tool.cpp
  bootstrap.cpp
  cli11_parser.cpp
  chat_session.cpp
  command_registry.cpp
  conversation_context.cpp
  config.cpp
  config_provenance.cpp
  context_builder.cpp
  context_parcel.cpp
  context_selection.cpp
  event_log.cpp
  exact_source_edit.cpp
  json_config_file_store.cpp
  language_analysis.cpp
  one_shot.cpp
  persona.cpp
  persona_context.cpp
  presentation_text.cpp
  project_instructions.cpp
  provenance.cpp
  repository_knowledge.cpp
  repository_snapshot.cpp
  review_gate.cpp
  review_receipt.cpp
  run_kernel.cpp
  run_projection.cpp
  scripted_artifact_store.cpp
  scripted_backend.cpp
  scripted_exact_source_editor.cpp
  scripted_hosted_review_check.cpp
  scripted_language_analysis_source.cpp
  scripted_policy_grant_store.cpp
  scripted_persona_source.cpp
  scripted_project_instruction_source.cpp
  scripted_repository_snapshot_source.cpp
  scripted_session_store.cpp
  scripted_tool_executor.cpp
  slash_commands.cpp
  sqlite_session_store.cpp
  transcript_projection.cpp
  tool_policy.cpp
  tool_registry.cpp
  verification_evidence.cpp
)
add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)

target_include_directories(${PROJECT_NAME}_lib
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>
)

target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)
target_link_libraries(${PROJECT_NAME}_lib
  PRIVATE CLI11::CLI11
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE SQLite::SQLite3
)
