if (TARGET ${PROJECT_NAME})
  add_executable(login-pty-fixture
    fixture.cpp
  )

  target_link_libraries(login-pty-fixture PRIVATE util)

  add_test(
    NAME 89login-pty
    COMMAND login-pty-fixture $<TARGET_FILE:${PROJECT_NAME}>
  )
endif ()
