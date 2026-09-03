if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(101toolapprovaldialog-test
  ../main.cpp
  test.cpp
)

target_include_directories(101toolapprovaldialog-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(101toolapprovaldialog-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
)
