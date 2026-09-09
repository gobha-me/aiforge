if (NOT TARGET aiforge::adapters)
  return()
endif ()

add_executable(159foldergrant-test ../main.cpp test.cpp)
target_link_libraries(159foldergrant-test
  PRIVATE Catch2::Catch2 aiforge::adapters)
