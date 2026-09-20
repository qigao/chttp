set(example_documents)
foreach(format IN ITEMS json yaml)
  set(document "${CMAKE_CURRENT_BINARY_DIR}/openapi.${format}")
  add_custom_command(
    OUTPUT "${document}"
    COMMAND openapi-gen --plugin "$<TARGET_FILE:openapi_c>"
      --format "${format}" --title "Pets API" --output "${document}"
      "${CMAKE_CURRENT_SOURCE_DIR}/pets.c"
    DEPENDS openapi-gen openapi_c "${CMAKE_CURRENT_SOURCE_DIR}/pets.c"
    COMMENT "Generating the Pets OpenAPI ${format} example"
    VERBATIM)
  list(APPEND example_documents "${document}")
endforeach()
add_custom_target(openapi_examples DEPENDS ${example_documents})
set_target_properties(openapi_examples PROPERTIES FOLDER "examples")

add_executable(openapi_ui_server ui_server.c)
target_link_libraries(openapi_ui_server PRIVATE
  CHttp::Web CHttp::Server Salts::Core Salts::JsonParser OpenAPI::UIModel)
set_target_properties(openapi_ui_server PROPERTIES FOLDER "examples")
