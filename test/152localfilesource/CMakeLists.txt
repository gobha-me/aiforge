if (NOT TARGET aiforge::adapters)
  return()
endif ()

add_executable(152localfilesource-test ../main.cpp test.cpp open_race.cpp)
target_link_libraries(152localfilesource-test
  PRIVATE Catch2::Catch2 aiforge::adapters)
target_link_options(152localfilesource-test PRIVATE "-Wl,--wrap=openat")
