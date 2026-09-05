add_library(aiforge_process_isolation_evaluation_support STATIC
  evidence.cpp
  probes.cpp
  runner.cpp
)

target_compile_features(aiforge_process_isolation_evaluation_support
  PUBLIC cxx_std_23
)
target_include_directories(aiforge_process_isolation_evaluation_support
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(aiforge_process_isolation_evaluation_support
  PRIVATE nlohmann_json::nlohmann_json
)

add_executable(aiforge_process_isolation_probe
  child_main.cpp
)
target_compile_features(aiforge_process_isolation_probe PRIVATE cxx_std_23)
target_link_libraries(aiforge_process_isolation_probe
  PRIVATE aiforge_process_isolation_evaluation_support
)

add_executable(aiforge_process_isolation_evaluation
  main.cpp
)
target_compile_features(aiforge_process_isolation_evaluation
  PRIVATE cxx_std_23
)
target_link_libraries(aiforge_process_isolation_evaluation
  PRIVATE aiforge_process_isolation_evaluation_support
)
add_dependencies(aiforge_process_isolation_evaluation
  aiforge_process_isolation_probe
)

add_library(aiforge_process_isolation_evaluation_v2_support STATIC
  evidence_v2.cpp
  probes_v2.cpp
  runner_v2.cpp
)
target_compile_features(aiforge_process_isolation_evaluation_v2_support
  PUBLIC cxx_std_23
)
target_include_directories(aiforge_process_isolation_evaluation_v2_support
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(aiforge_process_isolation_evaluation_v2_support
  PRIVATE aiforge_process_isolation_evaluation_support
  PRIVATE nlohmann_json::nlohmann_json
)

add_executable(aiforge_process_isolation_probe_v2
  child_v2_main.cpp
)
target_compile_features(aiforge_process_isolation_probe_v2 PRIVATE cxx_std_23)
target_link_libraries(aiforge_process_isolation_probe_v2
  PRIVATE aiforge_process_isolation_evaluation_v2_support
)

add_executable(aiforge_process_isolation_evaluation_v2
  main_v2.cpp
)
target_compile_features(aiforge_process_isolation_evaluation_v2
  PRIVATE cxx_std_23
)
target_link_libraries(aiforge_process_isolation_evaluation_v2
  PRIVATE aiforge_process_isolation_evaluation_v2_support
)
add_dependencies(aiforge_process_isolation_evaluation_v2
  aiforge_process_isolation_probe_v2
)

if (${PROJECT_NAME}_TESTS)
  target_compile_definitions(aiforge_process_isolation_evaluation_support
    PUBLIC AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT=1
  )
  target_compile_definitions(aiforge_process_isolation_evaluation_v2_support
    PUBLIC AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT=1
  )
  add_executable(process-isolation-evaluation-test
    ${PROJECT_SOURCE_DIR}/test/main.cpp
    test_evidence.cpp
    test_integration.cpp
    test_integration_v2.cpp
    test_evidence_v2.cpp
    test_probes_v2.cpp
    test_runner.cpp
    test_runner_v2.cpp
  )
  target_compile_features(process-isolation-evaluation-test PRIVATE cxx_std_23)
  target_link_libraries(process-isolation-evaluation-test
    PRIVATE Catch2::Catch2
    PRIVATE aiforge_process_isolation_evaluation_support
    PRIVATE aiforge_process_isolation_evaluation_v2_support
  )
  add_dependencies(process-isolation-evaluation-test
    aiforge_process_isolation_evaluation
    aiforge_process_isolation_evaluation_v2
    aiforge_process_isolation_probe
    aiforge_process_isolation_probe_v2
  )

  add_test(
    NAME process-isolation-evaluation
    COMMAND process-isolation-evaluation-test
  )
endif ()
