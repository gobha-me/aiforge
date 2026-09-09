add_executable(178linuxopshealth-test ../main.cpp test.cpp)
target_link_libraries(178linuxopshealth-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::adapters)
