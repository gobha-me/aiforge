add_library(${PROJECT_NAME}_adapters STATIC
  termforge_run_bridge.cpp
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
  PRIVATE venice-cpp::lib
)
