add_executable(173opsobservationwork-test ../main.cpp test.cpp)
target_include_directories(173opsobservationwork-test
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(173opsobservationwork-test
  PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib)
# Interpose the public pthread_create symbol used by archived std::thread code;
# a shared libstdc++ implementation would bypass executable link wrapping.
target_link_options(173opsobservationwork-test
  PRIVATE -static-libstdc++ "-Wl,--wrap=pthread_create")
