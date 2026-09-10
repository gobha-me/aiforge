if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ${PROJECT_NAME}::adapters)
  add_executable(188linuxjournallogs-test ../main.cpp test.cpp native.cpp)
  target_link_libraries(188linuxjournallogs-test
    PRIVATE Catch2::Catch2 ${PROJECT_NAME}::adapters dbus-1 SystemdJournal::libsystemd)
  foreach(function IN ITEMS open close set_data_threshold add_match seek_realtime_usec
      next get_realtime_usec restart_data enumerate_data)
    target_link_options(188linuxjournallogs-test PRIVATE "LINKER:--wrap=sd_journal_${function}")
  endforeach()
endif ()
