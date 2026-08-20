if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(86scenario-test
  ../main.cpp
  test.cpp
)

target_include_directories(86scenario-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(86scenario-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
