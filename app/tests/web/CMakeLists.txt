add_executable(chttp_web_renderer_test test_chttp_web_renderer.c)
target_link_libraries(chttp_web_renderer_test PRIVATE CHttp::App Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_renderer_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_renderer_test COMMAND chttp_web_renderer_test)

add_executable(chttp_web_context_test test_chttp_web_context.c)
target_link_libraries(chttp_web_context_test PRIVATE CHttp::App Salts::TinyTest)
set_target_properties(chttp_web_context_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_context_test COMMAND chttp_web_context_test)


add_executable(chttp_web_form_test test_chttp_web_form.c)
target_link_libraries(chttp_web_form_test PRIVATE
  CHttp::App Salts::DataBind Salts::TinyTest)
set_target_properties(chttp_web_form_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_form_test COMMAND chttp_web_form_test)


salts_idl_target(
  TARGET chttp_web_form_plan_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_web_form_plan.schema"
  ARTIFACT_NAME "chttp_web_form_plan"
  ARTIFACTS NATIVE
  BINARY_CODEC
  TRANSPORTS HTTP
  PROJECTION_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/chttp_web_form_plan.projection.json")

add_executable(chttp_web_form_plan_test test_chttp_web_form_plan.c)
target_link_libraries(chttp_web_form_plan_test PRIVATE
  CHttp::App chttp_web_form_plan_fixture_native Salts::DataBind Salts::TinyTest)
target_include_directories(chttp_web_form_plan_test PRIVATE
  "${chttp_web_form_plan_fixture_GENERATED_DIR}")
add_dependencies(chttp_web_form_plan_test
  chttp_web_form_plan_fixture_idl_codegen)
set_target_properties(chttp_web_form_plan_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_form_plan_test COMMAND chttp_web_form_plan_test)

add_test(
  NAME chttp_web_form_plan_boundary_contract
  COMMAND ${CMAKE_COMMAND}
    -DFORM_PLAN_SOURCE=${PROJECT_SOURCE_DIR}/app/src/chttp_web_form_plan.c
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_web_form_plan_contract_test.cmake)

add_executable(chttp_web_validation_test test_chttp_web_validation.c)
target_link_libraries(chttp_web_validation_test PRIVATE
  CHttp::App Salts::TinyTest)
set_target_properties(chttp_web_validation_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_validation_test COMMAND chttp_web_validation_test)

add_executable(chttp_web_multipart_test test_chttp_web_multipart.c)
target_link_libraries(chttp_web_multipart_test PRIVATE
  CHttp::App Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_multipart_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_multipart_test COMMAND chttp_web_multipart_test)

add_executable(chttp_web_upload_test test_chttp_web_upload.c)
target_link_libraries(chttp_web_upload_test PRIVATE
  CHttp::App Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_upload_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_upload_test COMMAND chttp_web_upload_test)

add_executable(chttp_web_deferred_test test_chttp_web_deferred.c)
target_link_libraries(chttp_web_deferred_test PRIVATE
  CHttp::App CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_deferred_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_deferred_test COMMAND chttp_web_deferred_test)

add_executable(chttp_web_lifecycle_test test_chttp_web_lifecycle.c)
target_link_libraries(chttp_web_lifecycle_test PRIVATE
  CHttp::App CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_lifecycle_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_lifecycle_test COMMAND chttp_web_lifecycle_test)

add_executable(chttp_web_sse_test test_chttp_web_sse.c)
target_link_libraries(chttp_web_sse_test PRIVATE
  CHttp::App CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_sse_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_sse_test COMMAND chttp_web_sse_test)

add_executable(chttp_web_upload_http_test test_chttp_web_upload_http.c)
target_link_libraries(chttp_web_upload_http_test PRIVATE
  CHttp::App CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_upload_http_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_upload_http_test COMMAND chttp_web_upload_http_test)
set_tests_properties(chttp_web_upload_http_test PROPERTIES TIMEOUT 30)

add_executable(chttp_web_auth_http_test test_chttp_web_auth_http.c)
target_link_libraries(chttp_web_auth_http_test PRIVATE
  CHttp::App CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_auth_http_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_auth_http_test COMMAND chttp_web_auth_http_test)

add_executable(chttp_web_assets_http_test test_chttp_web_assets_http.c)
target_link_libraries(chttp_web_assets_http_test PRIVATE
  CHttp::App CHttp::Client Salts::Core Salts::TinyTest)
set_target_properties(chttp_web_assets_http_test PROPERTIES
  FOLDER "app/tests/web"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_web_assets_http_test COMMAND chttp_web_assets_http_test)

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

add_test(NAME chttp_web_form_upload_http_test
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/test_chttp_web_form_upload_http.py"
          "$<TARGET_FILE:chttp_web_form_upload_example>")
