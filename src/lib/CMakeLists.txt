add_library(${PROJECT_NAME}_lib STATIC
  bootstrap.cpp
  cli11_parser.cpp
  command_registry.cpp
  config.cpp
  context_builder.cpp
  event_log.cpp
  json_config_file_store.cpp
  run_kernel.cpp
  run_projection.cpp
  scripted_backend.cpp
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
)
