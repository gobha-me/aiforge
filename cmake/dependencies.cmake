foreach(DEP IN LISTS ${PROJECT_NAME}_DEPS)
  set(DEP_RECIPE "${CMAKE_CURRENT_LIST_DIR}/deps/${DEP}.cmake")
  if (NOT EXISTS "${DEP_RECIPE}")
    message(FATAL_ERROR "Dependency '${DEP}' has no recipe at ${DEP_RECIPE}")
  endif ()

  message(STATUS "Including dependency ${DEP}")
  include("${DEP_RECIPE}")
endforeach()
