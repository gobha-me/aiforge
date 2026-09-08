if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()
add_executable(164localsourcebrowserdialog-test ../main.cpp test.cpp)
target_link_libraries(164localsourcebrowserdialog-test PRIVATE Catch2::Catch2
  ${PROJECT_NAME}::adapters)
