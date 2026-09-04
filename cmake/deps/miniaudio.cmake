# miniaudio: evaluation-only device I/O candidate for the Linux PCM WAV seam.
# Prefer an exact installed package, then the immutable 0.11.25 release source.
if (TARGET miniaudio::miniaudio OR TARGET miniaudio)
  message(FATAL_ERROR
    "miniaudio evidence requires an exact discoverable package or the controlled source fallback; a preprovided target cannot be verified")
endif ()

set(_aiforge_miniaudio_dependency_source installed_package)
find_package(miniaudio 0.11.25 EXACT CONFIG QUIET)

if (TARGET miniaudio AND NOT TARGET miniaudio::miniaudio)
  add_library(aiforge_miniaudio_contract INTERFACE)
  add_library(miniaudio::miniaudio ALIAS aiforge_miniaudio_contract)
  target_link_libraries(aiforge_miniaudio_contract INTERFACE miniaudio)
endif ()

if (NOT TARGET miniaudio::miniaudio)
  set(_aiforge_miniaudio_dependency_source controlled_source_fallback)
  set(MINIAUDIO_BUILD_EXAMPLES OFF CACHE BOOL
    "Build the miniaudio examples" FORCE)
  set(MINIAUDIO_BUILD_TESTS OFF CACHE BOOL
    "Build the miniaudio test suite" FORCE)
  set(MINIAUDIO_BUILD_TOOLS OFF CACHE BOOL
    "Build the miniaudio developer tools" FORCE)
  set(MINIAUDIO_INSTALL OFF CACHE BOOL
    "Generate miniaudio install rules" FORCE)
  set(MINIAUDIO_FORCE_CXX OFF CACHE BOOL "Compile miniaudio as C++" FORCE)
  set(MINIAUDIO_NO_EXTRA_NODES ON CACHE BOOL
    "Disable miniaudio extra nodes" FORCE)
  set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL
    "Disable miniaudio libvorbis integration" FORCE)
  set(MINIAUDIO_NO_LIBOPUS ON CACHE BOOL
    "Disable miniaudio libopus integration" FORCE)
  set(MINIAUDIO_NO_DECODING ON CACHE BOOL
    "Disable miniaudio decoding" FORCE)
  set(MINIAUDIO_NO_ENCODING ON CACHE BOOL
    "Disable miniaudio encoding" FORCE)
  set(MINIAUDIO_NO_WAV ON CACHE BOOL "Disable miniaudio WAV codecs" FORCE)
  set(MINIAUDIO_NO_FLAC ON CACHE BOOL "Disable miniaudio FLAC codecs" FORCE)
  set(MINIAUDIO_NO_MP3 ON CACHE BOOL "Disable miniaudio MP3 codecs" FORCE)
  set(MINIAUDIO_NO_RESOURCE_MANAGER ON CACHE BOOL
    "Disable the miniaudio resource manager" FORCE)
  set(MINIAUDIO_NO_NODE_GRAPH ON CACHE BOOL
    "Disable the miniaudio node graph" FORCE)
  set(MINIAUDIO_NO_ENGINE ON CACHE BOOL
    "Disable the miniaudio engine" FORCE)
  set(MINIAUDIO_NO_GENERATION ON CACHE BOOL
    "Disable miniaudio signal generation" FORCE)

  # Compile only the two viable Linux backends plus the inert null backend.
  # Runtime loading keeps system audio development packages out of this
  # candidate; automated evidence always supplies only ma_backend_null.
  set(MINIAUDIO_NO_RUNTIME_LINKING OFF CACHE BOOL
    "Disable miniaudio runtime backend loading" FORCE)
  set(MINIAUDIO_ENABLE_ONLY_SPECIFIC_BACKENDS ON CACHE BOOL
    "Build only explicitly enabled miniaudio backends" FORCE)
  set(MINIAUDIO_ENABLE_ALSA ON CACHE BOOL
    "Enable the miniaudio ALSA backend" FORCE)
  set(MINIAUDIO_ENABLE_PULSEAUDIO ON CACHE BOOL
    "Enable the miniaudio PulseAudio backend" FORCE)
  set(MINIAUDIO_ENABLE_NULL ON CACHE BOOL
    "Enable the miniaudio null backend" FORCE)

  if (DEFINED BUILD_SHARED_LIBS)
    set(_aiforge_miniaudio_had_build_shared_libs ON)
    set(_aiforge_miniaudio_build_shared_libs "${BUILD_SHARED_LIBS}")
  else ()
    set(_aiforge_miniaudio_had_build_shared_libs OFF)
  endif ()
  set(BUILD_SHARED_LIBS OFF)

  include(FetchContent)
  FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d.tar.gz
    URL_HASH SHA256=1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(miniaudio)

  if (_aiforge_miniaudio_had_build_shared_libs)
    set(BUILD_SHARED_LIBS "${_aiforge_miniaudio_build_shared_libs}")
  else ()
    unset(BUILD_SHARED_LIBS)
  endif ()
  unset(_aiforge_miniaudio_had_build_shared_libs)
  unset(_aiforge_miniaudio_build_shared_libs)

  if (TARGET miniaudio AND NOT TARGET miniaudio::miniaudio)
    add_library(aiforge_miniaudio_contract INTERFACE)
    add_library(miniaudio::miniaudio ALIAS aiforge_miniaudio_contract)
    target_link_libraries(aiforge_miniaudio_contract INTERFACE miniaudio)
  endif ()
  if (TARGET miniaudio)
    set_property(TARGET miniaudio PROPERTY
      AIFORGE_AUDIO_DEVICE_EVIDENCE_PROFILE
      "miniaudio-0.11.25-linux-null-device-only-static-v1")
  endif ()
endif ()

if (TARGET aiforge_miniaudio_contract)
  find_package(Threads REQUIRED)
  target_link_libraries(aiforge_miniaudio_contract
    INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)

  # Upstream applies these definitions privately to miniaudio.c. Consumers of
  # miniaudio.h must see the identical feature layout.
  target_compile_definitions(aiforge_miniaudio_contract INTERFACE
    MA_NO_EXTRA_NODES
    MA_NO_LIBVORBIS
    MA_NO_LIBOPUS
    MA_NO_DECODING
    MA_NO_ENCODING
    MA_NO_WAV
    MA_NO_FLAC
    MA_NO_MP3
    MA_NO_RESOURCE_MANAGER
    MA_NO_NODE_GRAPH
    MA_NO_ENGINE
    MA_NO_GENERATION
    MA_ENABLE_ONLY_SPECIFIC_BACKENDS
    MA_ENABLE_ALSA
    MA_ENABLE_PULSEAUDIO
    MA_ENABLE_NULL)
endif ()

if (NOT TARGET miniaudio::miniaudio)
  message(FATAL_ERROR
    "miniaudio 0.11.25 did not provide the canonical miniaudio::miniaudio target")
endif ()

set(AIFORGE_MINIAUDIO_DEPENDENCY_SOURCE
  "${_aiforge_miniaudio_dependency_source}" CACHE INTERNAL
  "Resolved miniaudio evidence source" FORCE)
get_target_property(_aiforge_miniaudio_target
  miniaudio::miniaudio ALIASED_TARGET)
if (NOT _aiforge_miniaudio_target)
  set(_aiforge_miniaudio_target miniaudio::miniaudio)
endif ()
if (TARGET miniaudio)
  get_target_property(_aiforge_miniaudio_type miniaudio TYPE)
else ()
  get_target_property(_aiforge_miniaudio_type
    ${_aiforge_miniaudio_target} TYPE)
endif ()
if (NOT _aiforge_miniaudio_type STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "miniaudio evidence requires static library linkage")
endif ()
if (TARGET miniaudio)
  set(_aiforge_miniaudio_profile_target miniaudio)
else ()
  set(_aiforge_miniaudio_profile_target ${_aiforge_miniaudio_target})
endif ()
get_target_property(_aiforge_miniaudio_profile
  ${_aiforge_miniaudio_profile_target}
  AIFORGE_AUDIO_DEVICE_EVIDENCE_PROFILE)
if (NOT _aiforge_miniaudio_profile STREQUAL
    "miniaudio-0.11.25-linux-null-device-only-static-v1")
  message(FATAL_ERROR
    "Installed miniaudio does not prove the required device-only evidence profile; disable package discovery to use the controlled fallback")
endif ()
target_compile_definitions(${_aiforge_miniaudio_target} INTERFACE
  AIFORGE_MINIAUDIO_DEPENDENCY_SOURCE="${_aiforge_miniaudio_dependency_source}")
unset(_aiforge_miniaudio_target)
unset(_aiforge_miniaudio_type)
unset(_aiforge_miniaudio_profile_target)
unset(_aiforge_miniaudio_profile)
unset(_aiforge_miniaudio_dependency_source)
