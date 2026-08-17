# nlohmann/json: private JSON adapter for configuration files.
# v3.11.3 matches the compatible pin used by venice-cpp.
if (NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.11 CONFIG QUIET)
endif ()

if (NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_BuildTests OFF CACHE BOOL "Build nlohmann/json tests" FORCE)
  set(JSON_Install OFF CACHE BOOL "Generate nlohmann/json install rules" FORCE)

  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif ()

if (NOT TARGET nlohmann_json::nlohmann_json)
  message(FATAL_ERROR "nlohmann/json did not provide nlohmann_json::nlohmann_json")
endif ()
