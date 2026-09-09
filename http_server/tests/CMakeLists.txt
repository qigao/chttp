add_executable(chttp_server_parser_test chttp_server_parser_test.c)
target_link_libraries(chttp_server_parser_test PRIVATE CHttp::Client CHttp::Server llhttp::llhttp_static Salts::TinyTest)
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
