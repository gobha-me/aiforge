if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(85transcriptview-test
  ../main.cpp
  test.cpp
)

target_include_directories(85transcriptview-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(85transcriptview-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
