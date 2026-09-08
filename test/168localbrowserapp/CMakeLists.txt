if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()
add_executable(168localbrowserapp-test ../main.cpp test.cpp)
target_link_libraries(168localbrowserapp-test PRIVATE Catch2::Catch2
  ${PROJECT_NAME}::adapters)
