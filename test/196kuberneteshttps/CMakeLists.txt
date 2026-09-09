if (NOT TARGET ${PROJECT_NAME}::adapters OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif ()
add_executable(${TEST_NAME} ../main.cpp test.cpp fixture.cpp worker.cpp)
target_include_directories(${TEST_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src/adapters)
target_link_libraries(${TEST_NAME} PRIVATE Catch2::Catch2 ${PROJECT_NAME}::adapters
  nlohmann_json::nlohmann_json venice-cpp::lib)
target_link_options(${TEST_NAME} PRIVATE
  -static-libstdc++ -Wl,--wrap=pthread_create -Wl,--wrap=SSL_CTX_new -Wl,--wrap=PEM_read_bio_X509 -Wl,--wrap=PEM_read_bio_PrivateKey)
