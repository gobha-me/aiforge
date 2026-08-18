if (NOT TARGET ${PROJECT_NAME}::adapters)
  return()
endif ()

add_executable(80adapters-test
  ../main.cpp
  test.cpp
)

target_include_directories(80adapters-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(80adapters-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::adapters
  PRIVATE httplib::httplib
  PRIVATE nlohmann_json::nlohmann_json
)
