add_executable(chttp_request_test chttp_request_test.c)
target_link_libraries(chttp_request_test PRIVATE CHttp::Client CHttp::Server llhttp::llhttp_static Salts::TinyTest)
target_include_directories(chttp_request_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME chttp_request_test COMMAND chttp_request_test)
set_target_properties(chttp_request_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(chttp_request_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_response_test chttp_response_test.c)
target_link_libraries(chttp_response_test PRIVATE CHttp::Client CHttp::Server llhttp::llhttp_static Salts::TinyTest)
target_include_directories(chttp_response_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME chttp_response_test COMMAND chttp_response_test)
set_target_properties(chttp_response_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(chttp_response_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_file_sink_test chttp_file_sink_test.c)
target_link_libraries(chttp_file_sink_test PRIVATE CHttp::Client CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_file_sink_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME chttp_file_sink_test COMMAND chttp_file_sink_test)
set_target_properties(chttp_file_sink_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(chttp_file_sink_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_api_test chttp_api_test.c)
target_link_libraries(chttp_api_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_api_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME chttp_api_test COMMAND chttp_api_test)
set_target_properties(chttp_api_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(chttp_api_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_requests_test chttp_requests_test.c)
target_link_libraries(chttp_requests_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_requests_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME chttp_requests_test COMMAND chttp_requests_test)
set_target_properties(chttp_requests_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(chttp_requests_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_tls_test chttp_tls_test.c)
target_link_libraries(chttp_tls_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_tls_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME chttp_tls_test COMMAND chttp_tls_test)
set_target_properties(chttp_tls_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(chttp_tls_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_h2_client_test chttp_h2_client_test.c)
target_link_libraries(chttp_h2_client_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_h2_client_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME chttp_h2_client_test COMMAND chttp_h2_client_test)
set_target_properties(chttp_h2_client_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(chttp_h2_client_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(crpc_api_test crpc_api_test.c)
target_link_libraries(crpc_api_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(crpc_api_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME crpc_api_test COMMAND crpc_api_test)
set_target_properties(crpc_api_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(crpc_api_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(crpc_transport_test crpc_transport_test.c)
target_link_libraries(crpc_transport_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(crpc_transport_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
add_test(NAME crpc_transport_test COMMAND crpc_transport_test)
set_target_properties(crpc_transport_test PROPERTIES FOLDER "http_client/tests")
set_target_properties(crpc_transport_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
