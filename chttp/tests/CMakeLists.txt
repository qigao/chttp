cmake_add_test(chttp_request_test
  SOURCES chttp_request_test.c
  LIBS salts_chttp llhttp::llhttp_static Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_request_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_response_test
  SOURCES chttp_response_test.c
  LIBS salts_chttp llhttp::llhttp_static Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_response_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_file_transfer_test
  SOURCES chttp_file_transfer_test.c
  LIBS salts_chttp Salts::CFlow Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_file_transfer_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_file_sink_test
  SOURCES chttp_file_sink_test.c
  LIBS salts_chttp Salts::CFlow Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_file_sink_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_server_parser_test
  SOURCES chttp_server_parser_test.c
  LIBS salts_chttp llhttp::llhttp_static Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_server_parser_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_websocket_handshake_test
  SOURCES chttp_websocket_handshake_test.c
  LIBS salts_chttp Salts::Core Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_websocket_handshake_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_websocket_test
  SOURCES chttp_websocket_test.c
  LIBS salts_chttp Salts::TinyTest
  FOLDER "chttp/tests")
set_target_properties(chttp_websocket_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_server_test
  SOURCES chttp_server_test.c
  LIBS salts_chttp Salts::TinyTest
  FOLDER "chttp/tests")
set_target_properties(chttp_server_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_server_deferred_token_test
  SOURCES chttp_server_deferred_token_test.c
  LIBS salts_chttp Salts::CFlow Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_server_deferred_token_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_api_test
  SOURCES chttp_api_test.c
  LIBS salts_chttp Salts::TinyTest
  FOLDER "chttp/tests")
set_target_properties(chttp_api_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_requests_test
  SOURCES chttp_requests_test.c
  LIBS salts_chttp Salts::TinyTest
  FOLDER "chttp/tests")
set_target_properties(chttp_requests_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_header_cpp_test
  SOURCES chttp_header_cpp_test.cpp
  LIBS salts_chttp
  FOLDER "chttp/tests")
set_target_properties(chttp_header_cpp_test PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

cmake_add_test(chttp_tls_test
  SOURCES chttp_tls_test.c
  LIBS salts_chttp Salts::TinyTest
  FOLDER "chttp/tests")
set_target_properties(chttp_tls_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_h2_frame_test
  SOURCES chttp_h2_frame_test.c
  LIBS salts_chttp Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_h2_frame_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_h2_hpack_test
  SOURCES chttp_h2_hpack_test.c
  LIBS salts_chttp Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_h2_hpack_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_h2_proto_test
  SOURCES chttp_h2_proto_test.c
  LIBS salts_chttp Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_h2_proto_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_h2_client_test
  SOURCES chttp_h2_client_test.c
  LIBS salts_chttp Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_h2_client_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_h2_server_test
  SOURCES chttp_h2_server_test.c
  LIBS salts_chttp Salts::CFlow Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_h2_server_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_h2_jwt_isolation_test
  SOURCES chttp_h2_jwt_isolation_test.c
  LIBS salts_chttp Salts::TinyTest
  FOLDER "chttp/tests")
set_target_properties(chttp_h2_jwt_isolation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(chttp_jwt_test
  SOURCES chttp_jwt_test.c
  LIBS salts_chttp Salts::CFlow Salts::TinyTest
  INCLUDES ../src
  FOLDER "chttp/tests")
set_target_properties(chttp_jwt_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
