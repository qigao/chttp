add_executable(chttp_file_transfer_test chttp_file_transfer_test.c)
target_link_libraries(chttp_file_transfer_test PRIVATE CHttp::Client CHttp::Server Salts::CFlow Salts::TinyTest)
target_include_directories(chttp_file_transfer_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME chttp_file_transfer_test COMMAND chttp_file_transfer_test)
set_target_properties(chttp_file_transfer_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(chttp_file_transfer_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_websocket_handshake_test chttp_websocket_handshake_test.c)
target_link_libraries(chttp_websocket_handshake_test PRIVATE CHttp::Client CHttp::Server Salts::Core Salts::TinyTest)
target_include_directories(chttp_websocket_handshake_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME chttp_websocket_handshake_test COMMAND chttp_websocket_handshake_test)
set_target_properties(chttp_websocket_handshake_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(chttp_websocket_handshake_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_header_cpp_test chttp_header_cpp_test.cpp)
target_link_libraries(chttp_header_cpp_test PRIVATE CHttp::Client CHttp::Server)
target_include_directories(chttp_header_cpp_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME chttp_header_cpp_test COMMAND chttp_header_cpp_test)
set_target_properties(chttp_header_cpp_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(chttp_header_cpp_test PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)


add_executable(chttp_h2_frame_test chttp_h2_frame_test.c)
target_link_libraries(chttp_h2_frame_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_h2_frame_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME chttp_h2_frame_test COMMAND chttp_h2_frame_test)
set_target_properties(chttp_h2_frame_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(chttp_h2_frame_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_h2_hpack_test chttp_h2_hpack_test.c)
target_link_libraries(chttp_h2_hpack_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_h2_hpack_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME chttp_h2_hpack_test COMMAND chttp_h2_hpack_test)
set_target_properties(chttp_h2_hpack_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(chttp_h2_hpack_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(chttp_h2_proto_test chttp_h2_proto_test.c)
target_link_libraries(chttp_h2_proto_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_h2_proto_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME chttp_h2_proto_test COMMAND chttp_h2_proto_test)
set_target_properties(chttp_h2_proto_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(chttp_h2_proto_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(crpc_json_test crpc_json_test.c)
target_link_libraries(crpc_json_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(crpc_json_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME crpc_json_test COMMAND crpc_json_test)
set_target_properties(crpc_json_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(crpc_json_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


add_executable(crpc_header_cpp_test crpc_header_cpp_test.cpp)
target_link_libraries(crpc_header_cpp_test PRIVATE CHttp::Client CHttp::Server)
target_include_directories(crpc_header_cpp_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME crpc_header_cpp_test COMMAND crpc_header_cpp_test)
set_target_properties(crpc_header_cpp_test PROPERTIES FOLDER "http_common/tests")
set_target_properties(crpc_header_cpp_test PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
