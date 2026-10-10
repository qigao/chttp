add_executable(chttp_response_owner_test
  chttp_response_owner_test.c chttp_response_scope_peer.c)
target_link_libraries(chttp_response_owner_test PRIVATE
  Salts::CNet Salts::CMeta Salts::TinyTest)
target_include_directories(chttp_response_owner_test PRIVATE
  ${PROJECT_SOURCE_DIR}/http_client/include
  ${PROJECT_SOURCE_DIR}/http_common/include)
set_target_properties(chttp_response_owner_test PROPERTIES
  FOLDER "http_client/tests" C_EXTENSIONS OFF)
add_test(NAME chttp_response_owner_test COMMAND chttp_response_owner_test)

add_executable(chttp_response_scope_cpp_test chttp_response_scope_cpp_test.cpp)
target_link_libraries(chttp_response_scope_cpp_test PRIVATE CHttp::Client Salts::TinyTest)
set_target_properties(chttp_response_scope_cpp_test PROPERTIES
  FOLDER "http_client/tests" CXX_EXTENSIONS OFF)
add_test(NAME chttp_response_scope_cpp_test COMMAND chttp_response_scope_cpp_test)

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


add_executable(chttp_pool_lifecycle_test chttp_pool_lifecycle_test.c)
target_link_libraries(chttp_pool_lifecycle_test PRIVATE
  CHttp::Client CHttp::Server Salts::CFlow Salts::TinyTest llhttp::llhttp_static)
target_include_directories(chttp_pool_lifecycle_test PRIVATE
  ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
set_target_properties(chttp_pool_lifecycle_test PROPERTIES
  FOLDER "http_client/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_pool_lifecycle_test COMMAND chttp_pool_lifecycle_test)

add_executable(chttp_destination_test chttp_destination_test.c)
target_link_libraries(chttp_destination_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
set_target_properties(chttp_destination_test PROPERTIES
  FOLDER "http_client/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_destination_test COMMAND chttp_destination_test)

add_executable(chttp_managed_stream_test chttp_managed_stream_test.c)
target_link_libraries(chttp_managed_stream_test PRIVATE Salts::CNet Salts::Core Salts::TinyTest)
target_include_directories(chttp_managed_stream_test PRIVATE ${PROJECT_SOURCE_DIR}/http_client/src)
set_target_properties(chttp_managed_stream_test PROPERTIES
  FOLDER "http_client/tests" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME chttp_managed_stream_test COMMAND chttp_managed_stream_test)
set_tests_properties(chttp_managed_stream_test PROPERTIES TIMEOUT 15)

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

add_test(
  NAME chttp_retained_send_contract_test
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_retained_send_contract_test.cmake)
