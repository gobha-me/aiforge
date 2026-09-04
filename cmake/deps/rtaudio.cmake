# RtAudio: evaluation-only device I/O candidate for the Linux PCM WAV seam.
# Prefer an exact installed package, then the immutable 6.0.1 release source.
if (TARGET RtAudio::rtaudio OR TARGET rtaudio)
  message(FATAL_ERROR
    "RtAudio evidence requires an exact discoverable package or the controlled source fallback; a preprovided target cannot be verified")
endif ()

set(_aiforge_rtaudio_dependency_source installed_package)
find_package(RtAudio 6.0.1 EXACT CONFIG QUIET)

if (TARGET rtaudio AND NOT TARGET RtAudio::rtaudio)
  add_library(RtAudio::rtaudio ALIAS rtaudio)
endif ()

if (NOT TARGET RtAudio::rtaudio)
  set(_aiforge_rtaudio_dependency_source controlled_source_fallback)
  set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL
    "Build RtAudio as a shared library" FORCE)
  set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL
    "Build the RtAudio test suite" FORCE)
  set(RTAUDIO_BUILD_PYTHON OFF CACHE BOOL
    "Build the RtAudio Python bindings" FORCE)
  set(RTAUDIO_TARGETNAME_UNINSTALL rtaudio-uninstall CACHE STRING
    "Name of the RtAudio uninstall target" FORCE)

  # Freeze the compiled production candidate to ALSA. Evidence executables
  # still select RtAudio::RTAUDIO_DUMMY explicitly and never access a device.
  set(RTAUDIO_API_ALSA ON CACHE BOOL "Build the RtAudio ALSA API" FORCE)
  set(RTAUDIO_API_PULSE OFF CACHE BOOL "Build the RtAudio Pulse API" FORCE)
  set(RTAUDIO_API_JACK OFF CACHE BOOL "Build the RtAudio JACK API" FORCE)
  set(RTAUDIO_API_OSS OFF CACHE BOOL "Build the RtAudio OSS API" FORCE)
  set(RTAUDIO_API_CORE OFF CACHE BOOL "Build the RtAudio CoreAudio API" FORCE)
  set(RTAUDIO_API_DS OFF CACHE BOOL "Build the RtAudio DirectSound API" FORCE)
  set(RTAUDIO_API_ASIO OFF CACHE BOOL "Build the RtAudio ASIO API" FORCE)
  set(RTAUDIO_API_WASAPI OFF CACHE BOOL "Build the RtAudio WASAPI API" FORCE)

  # RtAudio 6.0.1 calls export(PACKAGE). Do not let an evaluation dependency
  # mutate the user's CMake package registry during an AIForge configure.
  if (DEFINED CMAKE_EXPORT_NO_PACKAGE_REGISTRY)
    set(_aiforge_rtaudio_had_package_registry_setting ON)
    set(_aiforge_rtaudio_package_registry_setting
      "${CMAKE_EXPORT_NO_PACKAGE_REGISTRY}")
  else ()
    set(_aiforge_rtaudio_had_package_registry_setting OFF)
  endif ()
  set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)

  include(FetchContent)
  FetchContent_Declare(rtaudio
    URL https://github.com/thestk/rtaudio/archive/b4f04903312e0e0efffbe77655172e0f060dc085.tar.gz
    URL_HASH SHA256=8f6119875e28e3a7a6c5bc8ba8aa2d2644a8d6166bd1ff7ee07ca6f0d29b88cf
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(rtaudio)

  # The released ALSA implementation uses two bounded stack VLAs. AIForge's
  # Debug flags make that upstream extension fatal through RtAudio's own
  # Debug-only -Werror; keep the suppression private to the dependency.
  if (TARGET rtaudio AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtaudio PRIVATE -Wno-vla)
  endif ()
  if (TARGET rtaudio)
    # RtAudio otherwise compiles its inert dummy implementation only when no
    # production API is enabled. Keep ALSA available for comparison while
    # giving the evaluator an explicitly selectable hardware-free backend.
    target_compile_definitions(rtaudio PRIVATE __RTAUDIO_DUMMY__)
    set_property(TARGET rtaudio PROPERTY
      AIFORGE_AUDIO_DEVICE_EVIDENCE_PROFILE
      "rtaudio-6.0.1-alsa-dummy-static-v1")
  endif ()

  if (_aiforge_rtaudio_had_package_registry_setting)
    set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY
      "${_aiforge_rtaudio_package_registry_setting}")
  else ()
    unset(CMAKE_EXPORT_NO_PACKAGE_REGISTRY)
  endif ()
  unset(_aiforge_rtaudio_had_package_registry_setting)
  unset(_aiforge_rtaudio_package_registry_setting)

  if (TARGET rtaudio AND NOT TARGET RtAudio::rtaudio)
    add_library(RtAudio::rtaudio ALIAS rtaudio)
  endif ()
endif ()

if (NOT TARGET RtAudio::rtaudio)
  message(FATAL_ERROR
    "RtAudio 6.0.1 did not provide the canonical RtAudio::rtaudio target")
endif ()

set(AIFORGE_RTAUDIO_DEPENDENCY_SOURCE
  "${_aiforge_rtaudio_dependency_source}" CACHE INTERNAL
  "Resolved RtAudio evidence source" FORCE)
get_target_property(_aiforge_rtaudio_target RtAudio::rtaudio ALIASED_TARGET)
if (NOT _aiforge_rtaudio_target)
  set(_aiforge_rtaudio_target RtAudio::rtaudio)
endif ()
get_target_property(_aiforge_rtaudio_type ${_aiforge_rtaudio_target} TYPE)
if (NOT _aiforge_rtaudio_type STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "RtAudio evidence requires static library linkage")
endif ()
get_target_property(_aiforge_rtaudio_profile ${_aiforge_rtaudio_target}
  AIFORGE_AUDIO_DEVICE_EVIDENCE_PROFILE)
if (NOT _aiforge_rtaudio_profile STREQUAL
    "rtaudio-6.0.1-alsa-dummy-static-v1")
  message(FATAL_ERROR
    "Installed RtAudio does not prove the required ALSA and dummy evidence profile; disable package discovery to use the controlled fallback")
endif ()
target_compile_definitions(${_aiforge_rtaudio_target} INTERFACE
  AIFORGE_RTAUDIO_DEPENDENCY_SOURCE="${_aiforge_rtaudio_dependency_source}")
unset(_aiforge_rtaudio_target)
unset(_aiforge_rtaudio_type)
unset(_aiforge_rtaudio_profile)
unset(_aiforge_rtaudio_dependency_source)
