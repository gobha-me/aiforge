# yaml-cpp 0.9.0: private event parser for the ADR 0024 static kubeconfig subset.
# Official tag commit: 56e3bb550c91fd7005566f19c079cb7a503223cf.
if (NOT TARGET yaml-cpp::yaml-cpp)
  find_package(yaml-cpp 0.9 CONFIG QUIET)
endif ()

if (NOT TARGET yaml-cpp::yaml-cpp)
  if (yaml-cpp_FOUND)
    message(FATAL_ERROR "yaml-cpp package did not provide yaml-cpp::yaml-cpp")
  endif ()
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "Build yaml-cpp tests" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "Build yaml-cpp tools" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "Build yaml-cpp contrib" FORCE)
  set(YAML_CPP_INSTALL OFF CACHE BOOL "Install embedded yaml-cpp" FORCE)
  set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "Format yaml-cpp source" FORCE)
  set(YAML_CPP_DISABLE_UNINSTALL ON CACHE BOOL "Disable yaml-cpp uninstall" FORCE)
  set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared yaml-cpp" FORCE)
  set(YAML_ENABLE_PIC ON CACHE BOOL "Build position independent yaml-cpp" FORCE)
  include(FetchContent)
  FetchContent_Declare(yaml_cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.tar.gz
    URL_HASH SHA256=25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(yaml_cpp)
endif ()

if (NOT TARGET yaml-cpp::yaml-cpp)
  message(FATAL_ERROR "yaml-cpp did not provide yaml-cpp::yaml-cpp")
endif ()
