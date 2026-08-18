add_executable(${PROJECT_NAME}
  main.cpp
)

if (TARGET ${PROJECT_NAME}::adapters)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}::adapters)
  target_compile_definitions(${PROJECT_NAME} PRIVATE AIFORGE_HAS_ADAPTERS=1)
else ()
  target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}::lib)
endif ()
