add_executable(chttp_web_renderer_test test_chttp_web_renderer.c)
target_link_libraries(chttp_web_renderer_test PRIVATE CHttp::Web Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_renderer_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_renderer_test COMMAND chttp_web_renderer_test)

add_executable(chttp_web_context_test test_chttp_web_context.c)
target_link_libraries(chttp_web_context_test PRIVATE CHttp::Web Salts::TinyTest)
set_target_properties(chttp_web_context_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_context_test COMMAND chttp_web_context_test)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME chttp_web_http_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_chttp_web_http.py"
          "$<TARGET_FILE:chttp_web_example>")
add_test(NAME chttp_web_context_http_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_chttp_web_context_http.py"
          "$<TARGET_FILE:chttp_web_context_example>")
