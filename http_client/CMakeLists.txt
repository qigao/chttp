file(GLOB CHTTP_CLIENT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES ${CHTTP_CLIENT_SOURCES})
add_library(chttp_client SHARED
  ${CHTTP_CLIENT_SOURCES}

  rpc/crpc_prepare_call.c
  rpc/crpc_request_reply.c
  rpc/crpc_direct_async.c
  rpc/chttp_client_rpc_service.c

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
  PUBLIC Salts::CNet CMeta::CMeta Salts::CSerde Salts::DataBind
  PRIVATE llhttp::llhttp_static Salts::Core Salts::Core CMeta::CFlow
          Salts::URI Salts::JsonParser Salts::JsonCSerdeAdapter
          chttp_cjwt chttp_digest)
install(TARGETS chttp_client EXPORT ChttpTargets)
install(DIRECTORY include/http_client DESTINATION include)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
