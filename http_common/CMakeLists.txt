add_library(chttp_digest STATIC http/chttp_digest.c)
set_target_properties(chttp_digest PROPERTIES FOLDER "http_common" C_VISIBILITY_PRESET hidden)
target_include_directories(chttp_digest PUBLIC http)
target_link_libraries(chttp_digest PRIVATE GmSSL::GmSSL Salts::Core)

add_library(chttp_common OBJECT
  http/chttp_file_transfer.c
  http/chttp_h2_frame.c
  http/chttp_h2_hpack.c
  http/chttp_h2_proto.c
  http/chttp_tls.c
  http/chttp_websocket_handshake.c
  http/chttp_jwt_token.c
  rpc/crpc_json.c
  rpc/crpc_callable.c)
set_target_properties(chttp_common PROPERTIES FOLDER "http_common")
target_include_directories(chttp_common PRIVATE
  include ${PROJECT_SOURCE_DIR}/http_server/include
  ${PROJECT_SOURCE_DIR}/vendor/cjwt/include)
target_link_libraries(chttp_common PRIVATE
  llhttp::llhttp_static Salts::CNet Salts::Core CMeta::CFlow CMeta::CMeta Salts::CSerde
  Salts::JsonCSerdeAdapter Salts::JsonParser chttp_cjwt chttp_digest)
install(DIRECTORY include/http_common DESTINATION include)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
