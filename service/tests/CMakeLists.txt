salts_idl_target(
  TARGET chttp_service_http_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service.schema"
  ARTIFACT_NAME "chttp_service"
  TRANSPORTS HTTP
  PROJECTION_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service.projection.json")

add_executable(chttp_service_http_test
  chttp_service_http_test.c)

target_link_libraries(chttp_service_http_test PRIVATE
  CHttp::Service
  CHttp::Client
  Salts::TinyTest)

target_include_directories(chttp_service_http_test PRIVATE
  "${chttp_service_http_fixture_GENERATED_DIR}")

add_dependencies(chttp_service_http_test
  chttp_service_http_fixture_idl_codegen)

add_test(NAME chttp_service_http_test COMMAND chttp_service_http_test)

set_target_properties(chttp_service_http_test PROPERTIES
  FOLDER "service/tests"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_executable(chttp_service_header_cpp_test
  chttp_service_header_cpp_test.cpp)

target_link_libraries(chttp_service_header_cpp_test PRIVATE
  CHttp::Service)

add_test(NAME chttp_service_header_cpp_test
  COMMAND chttp_service_header_cpp_test)

set_target_properties(chttp_service_header_cpp_test PROPERTIES
  FOLDER "service/tests"
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)


add_test(
  NAME chttp_service_single_materialization_contract
  COMMAND ${CMAKE_COMMAND}
    -DSERVICE_SOURCE=${PROJECT_SOURCE_DIR}/service/src/chttp_service.c
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_single_materialization_contract_test.cmake)


add_test(
  NAME chttp_service_hot_path_boundary_contract
  COMMAND ${CMAKE_COMMAND}
    -DSERVICE_SOURCE=${PROJECT_SOURCE_DIR}/service/src/chttp_service.c
    -DSERVER_HEADER=${PROJECT_SOURCE_DIR}/http_server/include/http_server/http.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_hot_path_boundary_contract_test.cmake)
