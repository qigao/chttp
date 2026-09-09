set(TARGET_NAME salts_chttp)
set(CHTTP_LIBRARY_VERSION 2.2.0)
set(CHTTP_ABI_VERSION 2)

find_package(llhttp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

set(CHTTP_CJWT_ROOT "${PROJECT_SOURCE_DIR}/vendor/cjwt")
add_library(chttp_cjwt STATIC
  "${CHTTP_CJWT_ROOT}/src/cjwt.c"
  "${CHTTP_CJWT_ROOT}/src/jwe_openssl_3.c"
  "${CHTTP_CJWT_ROOT}/src/jws_openssl_3.c"
  "${CHTTP_CJWT_ROOT}/src/print.c"
  "${CHTTP_CJWT_ROOT}/src/utils.c")
set_target_properties(chttp_cjwt PROPERTIES FOLDER "chttp/internal")
target_compile_features(chttp_cjwt PRIVATE c_std_11)
target_include_directories(
  chttp_cjwt
  PRIVATE "${CHTTP_CJWT_ROOT}/include"
          "${CHTTP_CJWT_ROOT}/include/cjwt"
          "${CHTTP_CJWT_ROOT}/src")
target_link_libraries(
  chttp_cjwt
  PRIVATE Salts::JsonParser OpenSSL::Crypto turbo_crypto)

add_library(${TARGET_NAME} SHARED
  src/chttp_client.c
  src/chttp_file_sink.c
  src/chttp_file_transfer.c
  src/chttp_files.c
  src/chttp_h2_frame.c
  src/chttp_h2_hpack.c
  src/chttp_h2_proto.c
  src/chttp_h2_session.c
  src/chttp_h2_server.c
  src/chttp_jwt.c
  src/chttp_requests.c
  src/chttp_request.c
  src/chttp_response.c
  src/chttp_server.c
  src/chttp_server_parser.c
  src/chttp_server_response.c
  src/chttp_server_route.c
  src/chttp_session.c
  src/chttp_tls.c)
target_sources(${TARGET_NAME} PRIVATE src/chttp_websocket_handshake.c)
target_sources(${TARGET_NAME} PRIVATE src/chttp_websocket_server.c)
target_sources(${TARGET_NAME} PRIVATE src/chttp_websocket_client.c)
target_sources(${TARGET_NAME} PRIVATE src/chttp_websocket_pool.c)
cmake_config_target(${TARGET_NAME}
  ALIAS Salts::CHTTP
  FOLDER "chttp"
  EXPORT_NAME CHTTP
  VERSION ${CHTTP_LIBRARY_VERSION}
  SOVERSION ${CHTTP_ABI_VERSION})
if(WIN32)
  set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME "salts_chttp-${CHTTP_ABI_VERSION}")
endif()
target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
# BoringSSL archives contain C++ objects even though CHTTP exposes a C API.
set_target_properties(${TARGET_NAME} PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  LINKER_LANGUAGE CXX)
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CHTTP_CJWT_ROOT}/include)
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Salts::CNet
  PRIVATE llhttp::llhttp_static Salts::Platform Salts::Core Salts::CFlow Salts::UriParser Salts::JsonParser
          OpenSSL::Crypto chttp_cjwt)

install(TARGETS ${TARGET_NAME} EXPORT HTTPServicesTargets)
install(FILES "${llhttp_DIR}/copyright"
        DESTINATION share/licenses/HTTPServices/llhttp)
install(
  DIRECTORY include/chttp
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

if(BUILD_TESTS)
  add_subdirectory(tests)
  add_subdirectory("${CHTTP_CJWT_ROOT}/tests" "${CMAKE_CURRENT_BINARY_DIR}/cjwt-tests")
endif()
