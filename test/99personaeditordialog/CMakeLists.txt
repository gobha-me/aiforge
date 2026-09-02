if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(99personaeditordialog-test
  ../main.cpp
  test.cpp
)

target_include_directories(99personaeditordialog-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(99personaeditordialog-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
