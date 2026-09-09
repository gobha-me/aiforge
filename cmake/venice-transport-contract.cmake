# AIForge admission of the single coordinated transport. The source/installed
# dependency usually defines its checker; keep the exact reviewed helper for
# caller-preexisting targets too, without another HTTP library/header variant.
function(aiforge_check_venice_transport)
  get_target_property(venice_target venice-cpp::lib ALIASED_TARGET)
  if (NOT venice_target)
    set(venice_target venice-cpp::lib)
  endif ()
  get_target_property(contract ${venice_target} VENICE_CPP_HTTP_TRANSPORT_CONTRACT)
  if (NOT contract STREQUAL "httplib-0.51-openssl3-header-only-v1")
    message(FATAL_ERROR
      "Venice transport contract failure: selected venice-cpp::lib lacks the delivered HTTP capability")
  endif ()
  get_target_property(include_directories ${venice_target} INTERFACE_INCLUDE_DIRECTORIES)
  set(contract_include "")
  foreach(directory IN LISTS include_directories)
    if (directory MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
      set(directory "${CMAKE_MATCH_1}")
    elseif (directory MATCHES "^\\$<INSTALL_INTERFACE:")
      continue()
    endif ()
    if (IS_ABSOLUTE "${directory}" AND EXISTS "${directory}/venice/detail/httplib_contract.hpp")
      set(contract_include "${directory}")
      break()
    endif ()
  endforeach()
  if (NOT contract_include)
    message(FATAL_ERROR
      "Venice transport contract failure: selected target does not export its HTTP contract header")
  endif ()
  if (NOT COMMAND venice_cpp_check_httplib)
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/vendor/venice-httplib-contract.cmake")
  endif ()
  # An installed Venice include root can also contain httplib.h. Passing that
  # root as an extra probe include would shadow the HTTP target's own selected
  # header. Expose only a forwarding guard; its angle-bracket HTTP include must
  # resolve through the copied canonical target usage requirements.
  get_filename_component(contract_header
    "${contract_include}/venice/detail/httplib_contract.hpp" REALPATH)
  string(FIND "${contract_header}" "\\" backslash)
  if (NOT backslash EQUAL -1 OR contract_header MATCHES "[\"\r\n]")
    message(FATAL_ERROR "Venice transport contract failure: unsupported contract header path")
  endif ()
  set(probe_include "${CMAKE_CURRENT_BINARY_DIR}/aiforge-venice-contract")
  file(MAKE_DIRECTORY "${probe_include}/venice/detail")
  file(WRITE "${probe_include}/venice/detail/httplib_contract.hpp"
    "#include \"${contract_header}\"\n")
  # The upstream helper clears its cache. Also clear any inherited normal
  # variable before calling it; neither can substitute for current proof.
  unset(_venice_cpp_httplib_contract_works)
  unset(_venice_cpp_httplib_contract_works CACHE)
  venice_cpp_check_httplib(valid reason "${probe_include}")
  if (NOT valid)
    message(FATAL_ERROR "Venice transport contract failure: ${reason}")
  endif ()
endfunction()
aiforge_check_venice_transport()
