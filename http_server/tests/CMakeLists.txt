add_executable(chttp_server_example_test chttp_server_example_test.c)
target_link_libraries(chttp_server_example_test PRIVATE
  http_example_app CHttp::Client Salts::TinyTest llhttp::llhttp_static)
target_include_directories(chttp_server_example_test PRIVATE
  ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc
  ${PROJECT_SOURCE_DIR}/http_server/src)
target_compile_definitions(chttp_server_example_test PRIVATE
  HTTP_EXAMPLE_TEST_FILE="${PROJECT_SOURCE_DIR}/http_server/examples/http_example.schema"
  HTTP_EXAMPLE_PLUGIN_PATH="$<TARGET_FILE:http_example_contract_plugin>"
  HTTP_EXAMPLE_BAD_PLUGIN_PATH="$<TARGET_FILE:http_example_incompatible_plugin>")
add_dependencies(chttp_server_example_test http_example_contract_plugin http_example_incompatible_plugin)
set_target_properties(chttp_server_example_test PROPERTIES
  FOLDER "http_server/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_server_example_test COMMAND chttp_server_example_test)
set_tests_properties(chttp_server_example_test PROPERTIES TIMEOUT 60)
add_test(NAME chttp_server_example_plugin_test COMMAND chttp_server_example_test)
set_tests_properties(chttp_server_example_plugin_test PROPERTIES TIMEOUT 60
  ENVIRONMENT "HTTP_EXAMPLE_PLUGIN=$<TARGET_FILE:http_example_contract_plugin>")

# Same operation ID but incompatible result width: reject before publication.
salts_idl_target(
  TARGET http_example_incompatible
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/http_example_incompatible.schema"
  ARTIFACT_NAME http_example_incompatible
  COMPONENT "HttpExample.CalculatorPlugin"
  VERSION "1.0.0"
  ARTIFACTS PLUGIN
  BINARY_CODEC
  SOURCES http_example_incompatible.c)
set_target_properties(http_example_incompatible_plugin http_example_incompatible_plugin_client
  http_example_incompatible_idl_codegen PROPERTIES FOLDER "http_server/tests")

add_executable(chttp_server_retry_test chttp_server_retry_test.c)
target_link_libraries(chttp_server_retry_test PRIVATE CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_server_retry_test PRIVATE
  ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc
  ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
set_target_properties(chttp_server_retry_test PROPERTIES
  FOLDER "http_server/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_server_retry_test COMMAND chttp_server_retry_test)
set_tests_properties(chttp_server_retry_test PROPERTIES TIMEOUT 10)

add_executable(chttp_server_manager_test chttp_server_manager_test.c)
target_link_libraries(chttp_server_manager_test PRIVATE CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_server_manager_test PRIVATE
  ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc
  ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
set_target_properties(chttp_server_manager_test PROPERTIES
  FOLDER "http_server/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_server_manager_test COMMAND chttp_server_manager_test)
set_tests_properties(chttp_server_manager_test PROPERTIES TIMEOUT 10)

add_executable(chttp_server_parser_test chttp_server_parser_test.c)
target_link_libraries(chttp_server_parser_test PRIVATE CHttp::Client CHttp::Server llhttp::llhttp_static Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_server_parser_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_server_parser_test COMMAND chttp_server_parser_test)
set_target_properties(chttp_server_parser_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(chttp_server_parser_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_server_test chttp_server_test.c)
target_link_libraries(chttp_server_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_server_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_server_test COMMAND chttp_server_test)
set_target_properties(chttp_server_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(chttp_server_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_server_execution_options_test chttp_server_execution_options_test.c)
target_link_libraries(chttp_server_execution_options_test PRIVATE CHttp::Server Salts::TinyTest)
add_test(NAME chttp_server_execution_options_test COMMAND chttp_server_execution_options_test)
set_target_properties(chttp_server_execution_options_test PROPERTIES
  FOLDER "http_server/tests"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_executable(chttp_server_configurator_test chttp_server_configurator_test.c)
target_link_libraries(chttp_server_configurator_test PRIVATE
  CHttp::Server Salts::CFlow Salts::TinyTest llhttp::llhttp_static)
target_include_directories(chttp_server_configurator_test PRIVATE
  ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc
  ${PROJECT_SOURCE_DIR}/http_server/src)
set_target_properties(chttp_server_configurator_test PROPERTIES
  FOLDER "http_server/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_server_configurator_test COMMAND chttp_server_configurator_test)

add_executable(chttp_websocket_commands_test chttp_websocket_commands_test.c)
target_link_libraries(chttp_websocket_commands_test PRIVATE
  CHttp::Server Salts::CFlow Salts::TinyTest llhttp::llhttp_static)
target_include_directories(chttp_websocket_commands_test PRIVATE
  ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc
  ${PROJECT_SOURCE_DIR}/http_server/src)
set_target_properties(chttp_websocket_commands_test PROPERTIES
  FOLDER "http_server/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_websocket_commands_test COMMAND chttp_websocket_commands_test)
set_tests_properties(chttp_websocket_commands_test PROPERTIES TIMEOUT 15)


add_executable(chttp_server_owner_topology_test chttp_server_owner_topology_test.c)
target_link_libraries(chttp_server_owner_topology_test PRIVATE CHttp::Client CHttp::Server Salts::CNet Salts::CFlow Salts::TinyTest)
target_include_directories(
  chttp_server_owner_topology_test
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/http_server/src
          ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_server_owner_topology_test COMMAND chttp_server_owner_topology_test)
set_target_properties(chttp_server_owner_topology_test PROPERTIES
  FOLDER "http_server/tests"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_session_snapshot_test chttp_session_snapshot_test.c)
target_link_libraries(chttp_session_snapshot_test PRIVATE CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(
  chttp_session_snapshot_test
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/http_server/src
          ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_session_snapshot_test COMMAND chttp_session_snapshot_test)
set_target_properties(chttp_session_snapshot_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(chttp_session_snapshot_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_server_deferred_token_test chttp_server_deferred_token_test.c)
target_link_libraries(chttp_server_deferred_token_test PRIVATE CHttp::Client CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_server_deferred_token_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_server_deferred_token_test COMMAND chttp_server_deferred_token_test)
set_target_properties(chttp_server_deferred_token_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(chttp_server_deferred_token_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_h2_server_test chttp_h2_server_test.c)
target_link_libraries(chttp_h2_server_test PRIVATE CHttp::Client CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_h2_server_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_h2_server_test COMMAND chttp_h2_server_test)
set_target_properties(chttp_h2_server_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(chttp_h2_server_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_h2_jwt_isolation_test chttp_h2_jwt_isolation_test.c)
target_link_libraries(chttp_h2_jwt_isolation_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_h2_jwt_isolation_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_h2_jwt_isolation_test COMMAND chttp_h2_jwt_isolation_test)
set_target_properties(chttp_h2_jwt_isolation_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(chttp_h2_jwt_isolation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_jwt_test chttp_jwt_test.c)
target_link_libraries(chttp_jwt_test PRIVATE CHttp::Client CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_jwt_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME chttp_jwt_test COMMAND chttp_jwt_test)
set_target_properties(chttp_jwt_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(chttp_jwt_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_executable(crpc_server_test crpc_server_test.c)
target_link_libraries(crpc_server_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(crpc_server_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_server/src ${PROJECT_SOURCE_DIR}/http_server/rpc)
add_test(NAME crpc_server_test COMMAND crpc_server_test)
set_target_properties(crpc_server_test PROPERTIES FOLDER "http_server/tests")
set_target_properties(crpc_server_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_executable(chttp_cors_test chttp_cors_test.c)
target_link_libraries(chttp_cors_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
add_test(NAME chttp_cors_test COMMAND chttp_cors_test)
set_target_properties(chttp_cors_test PROPERTIES FOLDER "http_server/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_executable(chttp_rate_limit_test chttp_rate_limit_test.c)
target_link_libraries(chttp_rate_limit_test PRIVATE CHttp::Server Salts::TinyTest)
target_include_directories(chttp_rate_limit_test PRIVATE ${PROJECT_SOURCE_DIR}/http_server/src)
add_test(NAME chttp_rate_limit_test COMMAND chttp_rate_limit_test)
set_target_properties(chttp_rate_limit_test PROPERTIES FOLDER "http_server/tests"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
