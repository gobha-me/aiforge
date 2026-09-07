if (NOT TARGET ${PROJECT_NAME}::adapters OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif ()

add_executable(agent-process-owned-fixture fixture.cpp)
target_compile_features(agent-process-owned-fixture PRIVATE cxx_std_23)

add_executable(113agentprocess-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  test.cpp
)
target_compile_features(113agentprocess-test PRIVATE cxx_std_23)
target_compile_definitions(113agentprocess-test PRIVATE
  AGENT_PROCESS_FIXTURE="$<TARGET_FILE:agent-process-owned-fixture>"
)
target_link_libraries(113agentprocess-test PRIVATE
  Catch2::Catch2
  aiforge::adapters
  nlohmann_json::nlohmann_json
)
add_dependencies(113agentprocess-test agent-process-owned-fixture)
