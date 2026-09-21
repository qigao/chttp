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
