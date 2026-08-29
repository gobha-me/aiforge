add_executable(aiforge_drawforge_evaluation
  main.cpp
)

target_compile_features(aiforge_drawforge_evaluation PRIVATE cxx_std_23)
target_link_libraries(aiforge_drawforge_evaluation
  PRIVATE aiforge::adapters
  PRIVATE nlohmann_json::nlohmann_json
)

if (${PROJECT_NAME}_TESTS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME drawforge-evaluation-tool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_tool.py
      --runner $<TARGET_FILE:aiforge_drawforge_evaluation>
  )
endif ()
