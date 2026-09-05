if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(107approvalconfig-test
  ../main.cpp
  test.cpp
)

target_include_directories(107approvalconfig-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(107approvalconfig-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
