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


add_executable(chttp_web_form_test test_chttp_web_form.c)
target_link_libraries(chttp_web_form_test PRIVATE
  CHttp::Web Salts::DataBind Salts::TinyTest)
set_target_properties(chttp_web_form_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_form_test COMMAND chttp_web_form_test)

add_executable(chttp_web_validation_test test_chttp_web_validation.c)
target_link_libraries(chttp_web_validation_test PRIVATE
  CHttp::Web Salts::TinyTest)
set_target_properties(chttp_web_validation_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_validation_test COMMAND chttp_web_validation_test)

add_executable(chttp_web_multipart_test test_chttp_web_multipart.c)
target_link_libraries(chttp_web_multipart_test PRIVATE
  CHttp::Web Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_multipart_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_multipart_test COMMAND chttp_web_multipart_test)

add_executable(chttp_web_upload_test test_chttp_web_upload.c)
target_link_libraries(chttp_web_upload_test PRIVATE
  CHttp::Web Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_upload_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_upload_test COMMAND chttp_web_upload_test)

add_executable(chttp_web_deferred_test test_chttp_web_deferred.c)
target_link_libraries(chttp_web_deferred_test PRIVATE
  CHttp::Web CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_deferred_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_deferred_test COMMAND chttp_web_deferred_test)

add_executable(chttp_web_lifecycle_test test_chttp_web_lifecycle.c)
target_link_libraries(chttp_web_lifecycle_test PRIVATE
  CHttp::Web CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_lifecycle_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_lifecycle_test COMMAND chttp_web_lifecycle_test)

add_executable(chttp_web_sse_test test_chttp_web_sse.c)
target_link_libraries(chttp_web_sse_test PRIVATE
  CHttp::Web CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_sse_test PROPERTIES
  FOLDER "web/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_sse_test COMMAND chttp_web_sse_test)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME chttp_web_http_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_chttp_web_http.py"
          "$<TARGET_FILE:chttp_web_example>")
add_test(NAME chttp_web_context_http_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_chttp_web_context_http.py"
          "$<TARGET_FILE:chttp_web_context_example>")

add_test(NAME chttp_web_session_http_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_chttp_web_session_http.py"
          "$<TARGET_FILE:chttp_web_session_example>")

add_test(NAME chttp_web_security_http_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_chttp_web_security_http.py"
          "$<TARGET_FILE:chttp_web_security_example>"
          "${PROJECT_SOURCE_DIR}/openapi/ui/templates/layout.html")
