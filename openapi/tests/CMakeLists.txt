add_executable(test_generator test_generator.c)
set_target_properties(test_generator PROPERTIES FOLDER "tests")
target_link_libraries(test_generator PRIVATE "${OPENAPI_C_GRAMMAR_LIBRARY}" openapi_generator)
target_compile_definitions(test_generator PRIVATE
  C_PLUGIN="$<TARGET_FILE:openapi_c>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(test_generator openapi_c)
add_test(NAME generator COMMAND test_generator)

add_executable(test_ui_model test_ui_model.c)
set_target_properties(test_ui_model PROPERTIES FOLDER "tests")
target_link_libraries(test_ui_model PRIVATE "${OPENAPI_C_GRAMMAR_LIBRARY}" openapi_generator)
target_include_directories(test_ui_model PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test_ui_model PRIVATE C_PLUGIN="$<TARGET_FILE:openapi_c>")
add_dependencies(test_ui_model openapi_c)
add_test(NAME openapi_ui_model COMMAND test_ui_model)

add_executable(test_ui_renderer test_ui_renderer.c)
set_target_properties(test_ui_renderer PROPERTIES FOLDER "tests")
target_link_libraries(test_ui_renderer PRIVATE openapi_ui_renderer Salts::JsonParser)
add_test(NAME openapi_ui_renderer COMMAND test_ui_renderer)

add_executable(test_ui_fragments test_ui_fragments.c)
set_target_properties(test_ui_fragments PROPERTIES FOLDER "tests")
target_link_libraries(test_ui_fragments PRIVATE openapi_ui_renderer Salts::JsonParser)
add_test(NAME openapi_ui_fragments COMMAND test_ui_fragments)

add_executable(test_ui_htmx test_ui_htmx.c)
set_target_properties(test_ui_htmx PROPERTIES FOLDER "tests")
target_link_libraries(test_ui_htmx PRIVATE openapi_ui_renderer Salts::JsonParser)
add_test(NAME openapi_ui_htmx COMMAND test_ui_htmx)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(OPENAPI_NODE_EXECUTABLE NAMES node REQUIRED)
add_test(NAME generator_conformance
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/conformance.py"
    --generator "$<TARGET_FILE:openapi-gen>"
    --plugin "$<TARGET_FILE:openapi_c>"
    --node "${OPENAPI_NODE_EXECUTABLE}")
add_dependencies(test_generator openapi-gen)
add_test(NAME openapi_ui_logic
  COMMAND "${OPENAPI_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/ui_test.cjs")
add_test(NAME openapi_ui_tryit_logic
  COMMAND "${OPENAPI_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/tryit_test.cjs")
if(BUILD_EXAMPLES)
  add_test(NAME openapi_ui_security_http
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui_security_http_test.py"
      "$<TARGET_FILE:openapi_ui_server>" "${CMAKE_CURRENT_SOURCE_DIR}/../ui")
  add_test(NAME openapi_ui_http
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui_http_test.py"
      "$<TARGET_FILE:openapi_ui_server>" "${CMAKE_CURRENT_SOURCE_DIR}/../ui"
      "${CMAKE_CURRENT_SOURCE_DIR}/../examples/openapi.json")
  add_test(NAME openapi_ui_fragment_http
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui_fragment_http_test.py"
      "$<TARGET_FILE:openapi_ui_server>" "${CMAKE_CURRENT_SOURCE_DIR}/../ui"
      "${CMAKE_CURRENT_SOURCE_DIR}/../examples/openapi.json")
  add_test(NAME openapi_ui_tryit_http
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui_tryit_http_test.py"
      "$<TARGET_FILE:openapi_ui_server>" "${CMAKE_CURRENT_SOURCE_DIR}/../ui"
      "${CMAKE_CURRENT_SOURCE_DIR}/../examples/openapi.json")
endif()
