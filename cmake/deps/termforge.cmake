# termforge: terminal UI framework. Prefer an installed package, then a sibling
# checkout for coordinated development, then the compatible stable release.
if (NOT TARGET termforge::lib)
  find_package(termforge 0.57.22 CONFIG QUIET)
endif ()

if (NOT TARGET termforge::lib)
  set(termforge_TESTS OFF CACHE BOOL "Build the termforge test suite" FORCE)
  set(termforge_EXAMPLES OFF CACHE BOOL "Build the termforge examples" FORCE)
  set(termforge_BIN OFF CACHE BOOL "Build the termforge demo binary" FORCE)
  set(termforge_TOOLS OFF CACHE BOOL "Build termforge developer tools" FORCE)
  set(termforge_BENCH OFF CACHE BOOL "Build termforge benchmarks" FORCE)
  set(termforge_INSTALL OFF CACHE BOOL "Generate termforge install rules" FORCE)

  if (EXISTS ${PROJECT_SOURCE_DIR}/../termforge/CMakeLists.txt)
    message(STATUS "Using local termforge at ../termforge")
    add_subdirectory(${PROJECT_SOURCE_DIR}/../termforge ${CMAKE_BINARY_DIR}/_deps/termforge-build)
  else ()
    include(FetchContent)
    FetchContent_Declare(termforge
      GIT_REPOSITORY https://github.com/gobha-me/termforge.git
      # v0.57.22 retains the Composer, transcript, and choice-wizard seams while
      # keeping focused text cursors visible on the colorless fallback tier.
      GIT_TAG v0.57.22
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(termforge)
  endif ()
endif ()

if (NOT TARGET termforge::lib)
  message(FATAL_ERROR "termforge did not provide the canonical termforge::lib target")
endif ()

function(_aiforge_validate_termforge_headers target)
  # This is the complete TermForge surface compiled anywhere in AIForge. Keep
  # it explicit so package validation never searches ambient include paths.
  set(required_headers
    termforge/core/app.hpp
    termforge/core/byte_sink.hpp
    termforge/core/input.hpp
    termforge/core/screen.hpp
    termforge/core/terminal.hpp
    termforge/core/types.hpp
    termforge/drivers/fallback_driver.hpp
    termforge/drivers/kitty_driver.hpp
    termforge/drivers/terminal_driver.hpp
    termforge/widgets/choice_wizard_dialog.hpp
    termforge/widgets/composer.hpp
    termforge/widgets/detail/width.hpp
    termforge/widgets/dialog.hpp
    termforge/widgets/focus_ring.hpp
    termforge/widgets/list_widget.hpp
    termforge/widgets/text_box.hpp
    termforge/widgets/text_input.hpp
  )

  get_target_property(aliased_target ${target} ALIASED_TARGET)
  if (aliased_target)
    set(property_target ${aliased_target})
  else ()
    set(property_target ${target})
  endif ()

  get_target_property(exported_include_directories
    ${property_target} INTERFACE_INCLUDE_DIRECTORIES)
  set(exported_include_roots)
  foreach(include_directory IN LISTS exported_include_directories)
    if (include_directory MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
      set(include_root "${CMAKE_MATCH_1}")
    elseif (include_directory MATCHES "^\\$<INSTALL_INTERFACE:")
      continue()
    elseif (IS_ABSOLUTE "${include_directory}")
      set(include_root "${include_directory}")
    else ()
      continue()
    endif ()

    if (IS_ABSOLUTE "${include_root}")
      list(APPEND exported_include_roots "${include_root}")
    endif ()
  endforeach()
  list(REMOVE_DUPLICATES exported_include_roots)

  set(missing_headers)
  foreach(required_header IN LISTS required_headers)
    set(header_found OFF)
    foreach(include_root IN LISTS exported_include_roots)
      if (EXISTS "${include_root}/${required_header}")
        set(header_found ON)
        break()
      endif ()
    endforeach()
    if (NOT header_found)
      list(APPEND missing_headers "${required_header}")
    endif ()
  endforeach()

  if (missing_headers)
    list(JOIN missing_headers ", " missing_header_text)
    message(FATAL_ERROR
      "TermForge package contract failure: ${target} does not export required "
      "headers through its INTERFACE_INCLUDE_DIRECTORIES: ${missing_header_text}. "
      "Install a complete TermForge 0.57.22 package or adjust CMAKE_PREFIX_PATH "
      "so AIForge can use its controlled sibling/FetchContent fallback.")
  endif ()
endfunction()

_aiforge_validate_termforge_headers(termforge::lib)
