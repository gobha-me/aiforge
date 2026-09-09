if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()
add_executable(199chatadmin-test ../main.cpp test.cpp)
target_link_libraries(199chatadmin-test PRIVATE Catch2::Catch2
  ${PROJECT_NAME}::adapters)
