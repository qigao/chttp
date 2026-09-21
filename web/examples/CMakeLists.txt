add_executable(chttp_web_example chttp_web_example.c)
target_link_libraries(chttp_web_example PRIVATE CHttp::Web Salts::Core)
set_target_properties(chttp_web_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(chttp_web_context_example chttp_web_context_example.c)
target_link_libraries(chttp_web_context_example PRIVATE CHttp::Web Salts::Core)
set_target_properties(chttp_web_context_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(chttp_web_session_example chttp_web_session_example.c)
target_link_libraries(chttp_web_session_example PRIVATE CHttp::Web Salts::Core)
set_target_properties(chttp_web_session_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(chttp_web_security_example chttp_web_security_example.c)
target_link_libraries(chttp_web_security_example PRIVATE CHttp::Web Salts::Core)
set_target_properties(chttp_web_security_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(chttp_web_deferred_example chttp_web_deferred_example.c)
target_link_libraries(
  chttp_web_deferred_example
  PRIVATE CHttp::Web Salts::Core Salts::Concurrency)
set_target_properties(chttp_web_deferred_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(chttp_web_crud_example chttp_web_crud_example.c)
target_link_libraries(chttp_web_crud_example PRIVATE CHttp::Web Salts::Core)
set_target_properties(chttp_web_crud_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(
  chttp_web_form_upload_example
  chttp_web_form_upload_example.c)
target_link_libraries(
  chttp_web_form_upload_example PRIVATE CHttp::Web Salts::Core)
set_target_properties(chttp_web_form_upload_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)


add_executable(
  chttp_web_authenticated_app_example
  chttp_web_authenticated_app.c)
target_link_libraries(
  chttp_web_authenticated_app_example
  PRIVATE CHttp::Web CHttp::Client Salts::Core)
target_compile_definitions(
  chttp_web_authenticated_app_example
  PRIVATE
    CHTTP_WEB_AUTH_APP_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets/authenticated_app")
set_target_properties(chttp_web_authenticated_app_example PROPERTIES
  FOLDER "web/examples"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

if(BUILD_TESTS AND BUILD_TESTING)
  add_test(
    NAME chttp_web_authenticated_app_test
    COMMAND $<TARGET_FILE:chttp_web_authenticated_app_example> --self-test)
endif()
