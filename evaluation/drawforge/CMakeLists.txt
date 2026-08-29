add_library(aiforge_drawforge_evaluation_metadata STATIC
  run_metadata.cpp
)

target_compile_features(aiforge_drawforge_evaluation_metadata PRIVATE cxx_std_23)
target_include_directories(aiforge_drawforge_evaluation_metadata
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(aiforge_drawforge_evaluation_metadata
  PRIVATE nlohmann_json::nlohmann_json
)

add_executable(aiforge_drawforge_evaluation
  main.cpp
)

target_compile_features(aiforge_drawforge_evaluation PRIVATE cxx_std_23)
target_link_libraries(aiforge_drawforge_evaluation
  PRIVATE aiforge::adapters
  PRIVATE aiforge_drawforge_evaluation_metadata
  PRIVATE nlohmann_json::nlohmann_json
)

if (${PROJECT_NAME}_TESTS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_executable(drawforge-evaluation-metadata-test
    ${PROJECT_SOURCE_DIR}/test/main.cpp
    test_metadata.cpp
  )
  target_link_libraries(drawforge-evaluation-metadata-test
    PRIVATE Catch2::Catch2
    PRIVATE aiforge_drawforge_evaluation_metadata
    PRIVATE nlohmann_json::nlohmann_json
  )

  add_test(
    NAME drawforge-evaluation-metadata
    COMMAND drawforge-evaluation-metadata-test
  )
  add_test(
    NAME drawforge-evaluation-tool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_tool.py
      --runner $<TARGET_FILE:aiforge_drawforge_evaluation>
  )
endif ()
