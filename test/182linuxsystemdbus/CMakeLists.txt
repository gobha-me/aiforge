if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ${PROJECT_NAME}::adapters)
  add_executable(182linuxsystemdbus-test ../main.cpp test.cpp transport.cpp)
  target_link_libraries(182linuxsystemdbus-test
    PRIVATE Catch2::Catch2 ${PROJECT_NAME}::adapters dbus-1)
endif ()
