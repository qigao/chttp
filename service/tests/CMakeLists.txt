salts_idl_target(
  TARGET chttp_service_http_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service.schema"
  ARTIFACT_NAME "chttp_service"
  TRANSPORTS HTTP
  PROJECTION_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service.projection.json")

salts_idl_target(
  TARGET chttp_service_plugin_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_plugin.schema"
  COMPONENT "CHttpPlugin.CalcPlugin"
  VERSION "1.0.0"
  ARTIFACT_NAME "chttp_service_plugin"
  ARTIFACTS NATIVE PLUGIN
  TRANSPORTS HTTP
  PROJECTION_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_plugin.projection.json"
  SOURCES
    chttp_service_plugin_business.c)

# NATIVE and PLUGIN share one business implementation. The public salts_idl
# SOURCES contract feeds PLUGIN; attach the same source explicitly to the local
# NATIVE fixture so its exact generated execution descriptor is link-complete.
target_sources(chttp_service_plugin_fixture_native PRIVATE
  chttp_service_plugin_business.c)

salts_idl_target(
  TARGET chttp_service_cflow_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_cflow.schema"
  ARTIFACT_NAME "chttp_service_cflow"
  ARTIFACTS NATIVE
  TRANSPORTS HTTP
  PROJECTION_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_cflow.projection.json")

target_sources(chttp_service_cflow_fixture_native PRIVATE
  chttp_service_cflow_business.c)

get_target_property(CHTTP_SERVICE_CFLOW_LINKS
  chttp_service_cflow_fixture_native INTERFACE_LINK_LIBRARIES)
if(NOT CHTTP_SERVICE_CFLOW_LINKS)
  message(FATAL_ERROR
    "generated CFlow native target has no public dependency contract")
endif()
list(FIND CHTTP_SERVICE_CFLOW_LINKS "Salts::DataBind"
  CHTTP_SERVICE_CFLOW_DATABIND_INDEX)
list(FIND CHTTP_SERVICE_CFLOW_LINKS "Salts::CFlow"
  CHTTP_SERVICE_CFLOW_CFLOW_INDEX)
if(CHTTP_SERVICE_CFLOW_DATABIND_INDEX LESS 0 OR
   CHTTP_SERVICE_CFLOW_CFLOW_INDEX LESS 0)
  message(FATAL_ERROR
    "generated NATIVE target must publicly expose Salts::DataBind + Salts::CFlow")
endif()

salts_idl_target(
  TARGET chttp_service_cflow_unsupported_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_cflow_unsupported.schema"
  ARTIFACT_NAME "chttp_service_cflow_unsupported"
  ARTIFACTS NATIVE)

target_sources(chttp_service_cflow_unsupported_fixture_native PRIVATE
  chttp_service_cflow_unsupported_business.c)

add_executable(chttp_service_generated_cflow_http_test
  chttp_service_generated_cflow_http_test.c)

target_link_libraries(chttp_service_generated_cflow_http_test PRIVATE
  CHttp::Service
  CHttp::Client
  chttp_service_cflow_fixture_native
  chttp_service_cflow_unsupported_fixture_native
  Salts::TinyTest)

target_include_directories(chttp_service_generated_cflow_http_test PRIVATE
  "${chttp_service_cflow_fixture_GENERATED_DIR}"
  "${chttp_service_cflow_unsupported_fixture_GENERATED_DIR}")

add_dependencies(chttp_service_generated_cflow_http_test
  chttp_service_cflow_fixture_idl_codegen
  chttp_service_cflow_unsupported_fixture_idl_codegen)

add_test(NAME chttp_service_generated_cflow_http_test
  COMMAND chttp_service_generated_cflow_http_test)

set_target_properties(chttp_service_generated_cflow_http_test PROPERTIES
  FOLDER "service/tests"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

if(DEFINED ENV{TURBOWASM_ROOT} AND NOT "$ENV{TURBOWASM_ROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{TURBOWASM_ROOT}" CHTTP_TURBOWASM_ROOT)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_package(TurboWasm CONFIG REQUIRED
    PATHS "${CHTTP_TURBOWASM_ROOT}"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT TARGET TurboWasm::Component)
    message(FATAL_ERROR
      "released TurboWasm package is missing TurboWasm::Component")
  endif()

  set(CHTTP_SERVICE_WASM_CORE
      "${CMAKE_CURRENT_BINARY_DIR}/chttp_service_wasm_core.wasm")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_wasm_core.py"
            "${CHTTP_SERVICE_WASM_CORE}"
    RESULT_VARIABLE CHTTP_SERVICE_WASM_CORE_STATUS)
  if(NOT CHTTP_SERVICE_WASM_CORE_STATUS EQUAL 0 OR
     NOT EXISTS "${CHTTP_SERVICE_WASM_CORE}")
    message(FATAL_ERROR "failed to generate CHTTP Service WASM Core fixture")
  endif()

  salts_idl_target(
    TARGET chttp_service_wasm_fixture
    IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_wasm.schema"
    COMPONENT "CHttpWasm.CalcWasm"
    ARTIFACT_NAME "chttp_service_wasm"
    ARTIFACTS WASM
    TRANSPORTS HTTP
    PROJECTION_CONFIG
      "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_wasm.projection.json"
    WASM_CORE_MODULE "${CHTTP_SERVICE_WASM_CORE}")

  file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/chttp_service_wasm_path.h"
    CONTENT
      "#define GENERATED_CHTTP_SERVICE_WASM_PATH \"${chttp_service_wasm_fixture_WASM_ARTIFACT}\"\n")

  add_executable(chttp_service_wasm_http_test
    chttp_service_wasm_http_test.c)

  target_link_libraries(chttp_service_wasm_http_test PRIVATE
    CHttp::Service
    CHttp::Client
    chttp_service_wasm_fixture_wasm
    Salts::TinyTest)

  target_include_directories(chttp_service_wasm_http_test PRIVATE
    "${chttp_service_wasm_fixture_GENERATED_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}")

  add_dependencies(chttp_service_wasm_http_test
    chttp_service_wasm_fixture_idl_codegen)

  add_test(NAME chttp_service_wasm_http_test
    COMMAND chttp_service_wasm_http_test)

  set_target_properties(chttp_service_wasm_http_test PROPERTIES
    FOLDER "service/tests"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
endif()

add_executable(chttp_service_http_test
  chttp_service_http_test.c)

target_link_libraries(chttp_service_http_test PRIVATE
  CHttp::Service
  CHttp::Client
  chttp_service_plugin_fixture_native
  Salts::TinyTest)

target_include_directories(chttp_service_http_test PRIVATE
  "${chttp_service_http_fixture_GENERATED_DIR}"
  "${chttp_service_plugin_fixture_GENERATED_DIR}")

add_dependencies(chttp_service_http_test
  chttp_service_http_fixture_idl_codegen
  chttp_service_plugin_fixture_plugin
  chttp_service_plugin_fixture_native)

target_compile_definitions(chttp_service_http_test PRIVATE
  GENERATED_CHTTP_SERVICE_PLUGIN_PATH="$<TARGET_FILE:chttp_service_plugin_fixture_plugin>")

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
  NAME chttp_service_authority_contract
  COMMAND ${CMAKE_COMMAND}
    -DIDL_FILE=${CMAKE_CURRENT_SOURCE_DIR}/chttp_service.schema
    -DPROJECTION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/chttp_service.projection.json
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_authority_contract_test.cmake)


add_test(
  NAME chttp_service_single_materialization_contract
  COMMAND ${CMAKE_COMMAND}
    -DSERVICE_SOURCE=${PROJECT_SOURCE_DIR}/service/src/chttp_service.c
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_single_materialization_contract_test.cmake)


add_test(
  NAME chttp_service_hot_path_boundary_contract
  COMMAND ${CMAKE_COMMAND}
    -DSERVICE_SOURCE=${PROJECT_SOURCE_DIR}/service/src/chttp_service.c
    -DRPC_SERVICE_SOURCE=${PROJECT_SOURCE_DIR}/rpc_service/src/chttp_rpc_service.c
    -DSERVICE_HEADER=${PROJECT_SOURCE_DIR}/service/include/chttp_service/service.h
    -DSERVER_HEADER=${PROJECT_SOURCE_DIR}/http_server/include/http_server/http.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_hot_path_boundary_contract_test.cmake)
