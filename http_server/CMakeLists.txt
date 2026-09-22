add_library(chttp_server SHARED

  src/chttp_h2_server.c
  src/chttp_jwt.c
  src/chttp_server.c
  src/chttp_server_parser.c
  src/chttp_server_response.c
  src/chttp_server_route.c
  src/chttp_server_cors.c
  src/chttp_rate_limit.c
  src/chttp_session.c
  src/chttp_websocket_server.c
  src/chttp_server_files.c
  src/chttp_server_file_service.c

  rpc/crpc_server.c

  $<TARGET_OBJECTS:chttp_common>)
add_library(CHttp::Server ALIAS chttp_server)
set_target_properties(chttp_server PROPERTIES
  EXPORT_NAME Server FOLDER "http_server"
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON LINKER_LANGUAGE CXX)
target_compile_features(chttp_server PUBLIC c_std_11)
target_compile_definitions(chttp_server PRIVATE CSTL_NO_LEGACY_STACK_T=1)
target_include_directories(chttp_server
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/http_common/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/vendor/cjwt/include)
target_link_libraries(chttp_server
  PUBLIC Salts::CNet Salts::CMeta Salts::CSerde
  PRIVATE llhttp::llhttp_static Salts::Platform Salts::Core Salts::CFlow
          Salts::UriParser Salts::JsonParser Salts::JsonCSerdeAdapter
          OpenSSL::Crypto chttp_cjwt Salts::DateTimeParser Salts::CSTL)
install(TARGETS chttp_server EXPORT ChttpTargets)
install(DIRECTORY include/http_server DESTINATION include)
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
