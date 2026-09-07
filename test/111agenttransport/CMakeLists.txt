if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(111agenttransport-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  test.cpp
)
target_compile_features(111agenttransport-test PRIVATE cxx_std_23)
target_include_directories(111agenttransport-test
  PRIVATE ${PROJECT_SOURCE_DIR}/test/include
)
target_link_libraries(111agenttransport-test
  PRIVATE Catch2::Catch2
  PRIVATE aiforge::adapters
)

if (UNIX)
  add_executable(111agent-session-fixture session_fixture.cpp)
  target_link_libraries(111agent-session-fixture PRIVATE aiforge::adapters)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME 111agent-subprocess
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/subprocess.py
            $<TARGET_FILE:${PROJECT_NAME}> $<TARGET_FILE:111agent-session-fixture>)
  set_tests_properties(111agent-subprocess PROPERTIES TIMEOUT 30)
endif ()
