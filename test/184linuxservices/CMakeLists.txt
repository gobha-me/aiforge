if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ${PROJECT_NAME}::adapters)
  add_executable(184linuxservices-test ../main.cpp test.cpp budget.cpp)
  target_link_libraries(184linuxservices-test
    PRIVATE Catch2::Catch2 ${PROJECT_NAME}::adapters dbus-1)
endif ()
