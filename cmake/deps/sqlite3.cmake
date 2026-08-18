# SQLite: durable session-event storage behind the neutral SessionStore port.
# 3.45.1 is the minimum implementation baseline exercised by ADR 0005. The
# fallback is the current 3.53 maintenance release, pinned to SQLite's official
# amalgamation and published SHA3-256 digest.
if (NOT TARGET SQLite::SQLite3)
  find_package(SQLite3 3.45.1 QUIET)
endif ()

if (NOT TARGET SQLite::SQLite3)
  include(FetchContent)
  FetchContent_Declare(sqlite3_amalgamation
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
  FetchContent_MakeAvailable(sqlite3_amalgamation)

  add_library(aiforge_sqlite3 STATIC
    ${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c)
  add_library(SQLite::SQLite3 ALIAS aiforge_sqlite3)
  target_include_directories(aiforge_sqlite3
    PUBLIC ${sqlite3_amalgamation_SOURCE_DIR})
  target_compile_definitions(aiforge_sqlite3
    PRIVATE SQLITE_DQS=0
    PRIVATE SQLITE_OMIT_LOAD_EXTENSION)
  set_target_properties(aiforge_sqlite3 PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif ()

if (NOT TARGET SQLite::SQLite3)
  message(FATAL_ERROR "SQLite did not provide SQLite::SQLite3")
endif ()
