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


# One-off versioned dependency-closure qualification. Never enable this
# test listener in normal release or application targets.
option(CHTTP_GMSSL_PORT14_QUALIFICATION
  "Test source-built CHttp/CNet against independent OpenSSL TLS1.3 HRR/CCS" OFF)
if(CHTTP_GMSSL_PORT14_QUALIFICATION)
  add_executable(chttp_tls14_server_probe chttp_tls14_server_probe.c)
  target_link_libraries(chttp_tls14_server_probe PRIVATE
    CHttp::Server Salts::CNet Salts::Core)
  set_target_properties(chttp_tls14_server_probe PROPERTIES
    FOLDER "http_client/tests" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()

add_executable(chttp_tls_test chttp_tls_test.c)
target_link_libraries(chttp_tls_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_tls_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc ${PROJECT_SOURCE_DIR}/http_client/src ${PROJECT_SOURCE_DIR}/http_client/rpc)
if(CHTTP_P384_SHA384_ACCEPTANCE_TESTS)
  target_compile_definitions(chttp_tls_test PRIVATE
    CHTTP_P384_SHA384_ACCEPTANCE_TESTS=1)
endif()
if(CHTTP_X509_MATRIX_FIXTURE_DIR)
  set(_chttp_x509_required
    p384-chain-root-cert.pem
    p384-chain-intermediate-cert.pem
    p384-chain-server-cert.pem
    p384-chain-server-key.pem
    invalid-ca-root-cert.pem
    invalid-ca-issuer-cert.pem
    invalid-ca-server-cert.pem
    invalid-ca-server-key.pem
    p256-test-root-cert.pem
    p256-test-server-cert.pem
    p256-test-server-key.pem
    rsa3072-test-root-cert.pem
    rsa3072-test-server-cert.pem
    rsa3072-test-server-key.pem
    rsa3072-test-server-pss-cert.pem
    sha512-mixed-root-cert.pem
    sha512-mixed-server-cert.pem
    sha512-mixed-server-key.pem
    unsupported-p521-root-cert.pem
    unsupported-p521-server-cert.pem
    unsupported-ed25519-server-cert.pem
    unsupported-ed25519-server-key.pem)
  foreach(_fixture IN LISTS _chttp_x509_required)
    if(NOT EXISTS "${CHTTP_X509_MATRIX_FIXTURE_DIR}/${_fixture}")
      message(FATAL_ERROR "Missing CHttp X.509 matrix fixture: ${_fixture}")
    endif()
  endforeach()
  file(READ "${CHTTP_X509_MATRIX_FIXTURE_DIR}/p384-chain-server-cert.pem"
    _chttp_p384_chain_leaf)
  file(READ "${CHTTP_X509_MATRIX_FIXTURE_DIR}/p384-chain-intermediate-cert.pem"
    _chttp_p384_chain_issuer)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/chttp-p384-server-fullchain.pem"
    "${_chttp_p384_chain_leaf}${_chttp_p384_chain_issuer}")
  file(READ "${CHTTP_X509_MATRIX_FIXTURE_DIR}/invalid-ca-server-cert.pem"
    _chttp_invalid_ca_leaf)
  file(READ "${CHTTP_X509_MATRIX_FIXTURE_DIR}/invalid-ca-issuer-cert.pem"
    _chttp_invalid_ca_issuer)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/chttp-invalid-ca-fullchain.pem"
    "${_chttp_invalid_ca_leaf}${_chttp_invalid_ca_issuer}")
  file(TO_CMAKE_PATH "${CHTTP_X509_MATRIX_FIXTURE_DIR}" _chttp_x509_fixture_dir)
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}/chttp-p384-server-fullchain.pem"
    _chttp_p384_fullchain)
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}/chttp-invalid-ca-fullchain.pem"
    _chttp_invalid_ca_fullchain)
  target_compile_definitions(chttp_tls_test PRIVATE
    CHTTP_X509_MATRIX_ACCEPTANCE_TESTS=1
    "CHTTP_X509_FIXTURE_DIR=\"${_chttp_x509_fixture_dir}\""
    "CHTTP_X509_P384_FULLCHAIN=\"${_chttp_p384_fullchain}\""
    "CHTTP_X509_INVALID_CA_FULLCHAIN=\"${_chttp_invalid_ca_fullchain}\"")
  unset(_chttp_x509_required)
  unset(_chttp_p384_chain_leaf)
  unset(_chttp_p384_chain_issuer)
  unset(_chttp_invalid_ca_leaf)
  unset(_chttp_invalid_ca_issuer)
  unset(_chttp_x509_fixture_dir)
  unset(_chttp_p384_fullchain)
  unset(_chttp_invalid_ca_fullchain)
endif()
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

# Native typed RPC API must be accessible from the Client target alone.
add_executable(chttp_client_rpc_header_cpp_test chttp_client_rpc_header_cpp_test.cpp)
target_link_libraries(chttp_client_rpc_header_cpp_test PRIVATE CHttp::Client)
set_target_properties(chttp_client_rpc_header_cpp_test PROPERTIES
  FOLDER "http_client/tests" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
add_test(NAME chttp_client_rpc_header_cpp_test COMMAND chttp_client_rpc_header_cpp_test)
