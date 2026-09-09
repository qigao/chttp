add_library(chttp_client SHARED

  src/chttp_client.c
  src/chttp_file_sink.c
  src/chttp_files.c
  src/chttp_h2_session.c
  src/chttp_request.c
  src/chttp_requests.c
  src/chttp_response.c
  src/chttp_websocket_client.c
  src/chttp_websocket_pool.c
  src/chttp_auth.c

  rpc/crpc_client.c
  rpc/crpc_prepare_call.c
  rpc/crpc_request_reply.c

  $<TARGET_OBJECTS:chttp_common>)
add_library(CHttp::Client ALIAS chttp_client)
set_target_properties(chttp_client PROPERTIES
  EXPORT_NAME Client FOLDER "http_client"
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON LINKER_LANGUAGE CXX)
target_compile_features(chttp_client PUBLIC c_std_11)
target_include_directories(chttp_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/http_common/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/vendor/cjwt/include)
target_link_libraries(chttp_client
  PUBLIC Salts::CNet Salts::CMeta Salts::CSerde
  PRIVATE llhttp::llhttp_static Salts::Platform Salts::Core Salts::CFlow
          Salts::UriParser Salts::JsonParser Salts::JsonCSerdeAdapter
          OpenSSL::Crypto chttp_cjwt)
install(TARGETS chttp_client EXPORT HTTPServicesTargets)
install(DIRECTORY include/http_client DESTINATION include)
