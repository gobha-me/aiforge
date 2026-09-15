# Test-owned installed-client fixture using upstream's actual client install
# rules and generated package metadata. Never install a daemon or system config.
if (NOT DBUS_BUILD_DIR OR NOT CMAKE_INSTALL_PREFIX)
  message(FATAL_ERROR "DBUS_BUILD_DIR and a test-owned CMAKE_INSTALL_PREFIX are required")
endif ()
include("${DBUS_BUILD_DIR}/dbus/cmake_install.cmake")
file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/DBus1"
  TYPE FILE FILES
  "${DBUS_BUILD_DIR}/DBus1Config.cmake"
  "${DBUS_BUILD_DIR}/DBus1ConfigVersion.cmake")
file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig"
  TYPE FILE FILES "${DBUS_BUILD_DIR}/dbus-1.pc")
