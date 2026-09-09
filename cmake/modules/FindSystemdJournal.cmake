# Normalize one private platform client target. Pre-existing targets must supply
# their package version as well as usable headers and symbols.
if (TARGET SystemdJournal::libsystemd)
  get_target_property(SystemdJournal_VERSION SystemdJournal::libsystemd
    SystemdJournal_VERSION)
else ()
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(AIFORGE_SYSTEMD_JOURNAL QUIET IMPORTED_TARGET GLOBAL
      "libsystemd>=${SystemdJournal_FIND_VERSION}")
    if (TARGET PkgConfig::AIFORGE_SYSTEMD_JOURNAL)
      set(SystemdJournal_VERSION "${AIFORGE_SYSTEMD_JOURNAL_VERSION}")
      add_library(SystemdJournal::libsystemd INTERFACE IMPORTED GLOBAL)
      set_target_properties(SystemdJournal::libsystemd PROPERTIES
        INTERFACE_LINK_LIBRARIES PkgConfig::AIFORGE_SYSTEMD_JOURNAL
        SystemdJournal_VERSION "${SystemdJournal_VERSION}")
    endif ()
  endif ()
endif ()

set(SystemdJournal_TARGET_FOUND FALSE)
if (TARGET SystemdJournal::libsystemd)
  set(SystemdJournal_TARGET_FOUND TRUE)
endif ()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SystemdJournal
  REQUIRED_VARS SystemdJournal_TARGET_FOUND SystemdJournal_VERSION
  VERSION_VAR SystemdJournal_VERSION
  REASON_FAILURE_MESSAGE "Linux adapters require libsystemd development files (version 233 or newer)")

if (SystemdJournal_FOUND)
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES SystemdJournal::libsystemd)
  # Target interfaces can change when an existing build tree is reconfigured.
  unset(SystemdJournal_CLIENT_LINKS)
  unset(SystemdJournal_CLIENT_LINKS CACHE)
  check_cxx_source_compiles("
    #include <systemd/sd-journal.h>
    int main() {
      auto volatile a = &sd_journal_open;
      auto volatile b = &sd_journal_close;
      auto volatile c = &sd_journal_add_match;
      auto volatile d = &sd_journal_seek_realtime_usec;
      auto volatile e = &sd_journal_next;
      auto volatile f = &sd_journal_get_realtime_usec;
      auto volatile g = &sd_journal_restart_data;
      auto volatile h = &sd_journal_enumerate_data;
      auto volatile i = &sd_journal_set_data_threshold;
      return !(a && b && c && d && e && f && g && h && i);
    }" SystemdJournal_CLIENT_LINKS)
  cmake_pop_check_state()
  if (NOT SystemdJournal_CLIENT_LINKS)
    message(FATAL_ERROR "SystemdJournal target lacks required client headers or symbols")
  endif ()
endif ()
