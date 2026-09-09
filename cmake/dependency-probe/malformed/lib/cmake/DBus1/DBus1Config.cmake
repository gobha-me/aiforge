add_library(dbus-1 INTERFACE IMPORTED)
set_target_properties(dbus-1 PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/missing-headers")
