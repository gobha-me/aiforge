add_library(${PROJECT_NAME}_lib STATIC
  bootstrap.cpp
  event_log.cpp
  run_projection.cpp
  scripted_backend.cpp
)
add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)

target_include_directories(${PROJECT_NAME}_lib
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)
