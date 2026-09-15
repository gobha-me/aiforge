# ADR 0024: Linux journal observation needs the platform client library, not a
# source build of a system manager. This required prerequisite has no fallback.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The private system journal reader requires Linux")
endif ()
function(aiforge_find_systemd_journal)
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../modules")
  find_package(SystemdJournal 233 REQUIRED MODULE)
endfunction()
aiforge_find_systemd_journal()
