if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(109processchat-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  test.cpp
)
target_compile_features(109processchat-test PRIVATE cxx_std_23)
target_link_libraries(109processchat-test
  PRIVATE Catch2::Catch2
  PRIVATE aiforge::adapters
)
