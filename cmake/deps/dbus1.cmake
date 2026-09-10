# ADR 0024: private Linux systemd read transport. Upstream's maintained stable
# release and published archive digest were checked on 2026-09-09.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The private systemd transport requires Linux")
endif ()

if (TARGET dbus-1)
  return()
endif ()

find_package(DBus1 1.16.2 CONFIG QUIET)
if (NOT TARGET dbus-1)
  foreach(option IN ITEMS
      DBUS_BUILD_TESTS DBUS_ENABLE_INTRUSIVE_TESTS DBUS_ENABLE_VERBOSE_MODE
      DBUS_BUILD_X11 DBUS_WITH_GLIB DBUS_ENABLE_DOXYGEN_DOCS
      DBUS_ENABLE_XML_DOCS DBUS_ENABLE_MODULAR_TESTS)
    set(${option} OFF CACHE BOOL "Disable embedded libdbus auxiliary features" FORCE)
  endforeach()
  set(ENABLE_QT_HELP OFF CACHE STRING "Disable embedded libdbus Qt help" FORCE)
  set(ENABLE_SYSTEMD OFF CACHE STRING "Disable libdbus daemon systemd integration" FORCE)
  set(DBUS_DISABLE_CHECKS OFF CACHE BOOL "Retain libdbus public argument checks" FORCE)

  include(FetchContent)
  FetchContent_Declare(aiforge_dbus1
    URL https://dbus.freedesktop.org/releases/dbus/dbus-1.16.2.tar.xz
    URL_HASH SHA256=0ba2a1a4b16afe7bceb2c07e9ce99a8c2c3508e5dec290dbb643384bd6beb7e2
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    PATCH_COMMAND ${CMAKE_COMMAND}
      -DDBUS_SOURCE_DIR=<SOURCE_DIR>
      -P ${CMAKE_CURRENT_LIST_DIR}/dbus1-subproject.cmake
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(aiforge_dbus1)

  if (TARGET dbus-1)
    # Upstream 1.16.2 only declares INSTALL_INTERFACE include directories.
    target_include_directories(dbus-1 SYSTEM INTERFACE
      $<BUILD_INTERFACE:${aiforge_dbus1_SOURCE_DIR}>
      $<BUILD_INTERFACE:${aiforge_dbus1_BINARY_DIR}>)
  endif ()
endif ()

if (NOT TARGET dbus-1)
  message(FATAL_ERROR "DBus1 did not provide the dbus-1 client target")
endif ()
