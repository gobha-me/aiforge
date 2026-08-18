add_executable(90oneshot-test
  ../main.cpp
  test.cpp
)

target_include_directories(90oneshot-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(90oneshot-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::lib
)

add_executable(one-shot-subprocess-fixture
  fixture.cpp
)

target_include_directories(one-shot-subprocess-fixture
  PRIVATE ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(one-shot-subprocess-fixture
  PRIVATE ${PROJECT_NAME}::lib
)

if (TARGET ${PROJECT_NAME}::adapters)
  add_test(
    NAME 90oneshot-subprocess
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/subprocess.sh
            $<TARGET_FILE:one-shot-subprocess-fixture>
            $<TARGET_FILE:${PROJECT_NAME}>
  )
else ()
  add_test(
    NAME 90oneshot-subprocess
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/subprocess.sh
            $<TARGET_FILE:one-shot-subprocess-fixture>
  )
endif ()
