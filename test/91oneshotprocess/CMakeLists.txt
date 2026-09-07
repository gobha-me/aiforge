if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(91oneshotprocess-test ../main.cpp test.cpp)
target_link_libraries(91oneshotprocess-test
  PRIVATE Catch2::Catch2 PRIVATE ${PROJECT_NAME}::adapters)

if (UNIX)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME 91oneshotprocess-subprocess
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stdin_cancellation.py
            $<TARGET_FILE:${PROJECT_NAME}>)
  set_tests_properties(91oneshotprocess-subprocess PROPERTIES TIMEOUT 30)
endif ()
