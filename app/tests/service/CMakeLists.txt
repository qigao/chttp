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
  BINARY_CODEC
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
  BINARY_CODEC
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
  ARTIFACTS NATIVE
  BINARY_CODEC)

target_sources(chttp_service_cflow_unsupported_fixture_native PRIVATE
  chttp_service_cflow_unsupported_business.c)

add_executable(chttp_service_generated_cflow_http_test
  chttp_service_generated_cflow_http_test.c)

target_link_libraries(chttp_service_generated_cflow_http_test PRIVATE
  CHttp::App
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
  FOLDER "app/tests/service"
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
    BINARY_CODEC
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
    CHttp::App
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
    FOLDER "app/tests/service"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
endif()

add_executable(chttp_service_http_test
  chttp_service_http_test.c)

target_link_libraries(chttp_service_http_test PRIVATE
  CHttp::App
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

# The real original Provider ABI/Plugin loader is shared with the candidate,
# but each generation owns an independent scope and DSO lease.
if(CHTTP_ENABLE_SERVICE_HOST_CANDIDATE)
  add_library(chttp_service_host_provider_fixture SHARED
    chttp_service_host_provider.c)
  add_library(chttp_service_host_provider_second SHARED
    chttp_service_host_provider.c
    chttp_service_host_second_business.c)
  foreach(_chttp_host_provider IN ITEMS
      chttp_service_host_provider_fixture
      chttp_service_host_provider_second)
    target_link_libraries(${_chttp_host_provider} PRIVATE
      chttp_service_plugin_fixture_native Salts::ComponentPluginABI Salts::PluginABI)
    target_include_directories(${_chttp_host_provider} PRIVATE
      "${PROJECT_SOURCE_DIR}/app/src"
      "${chttp_service_plugin_fixture_GENERATED_DIR}")
    add_dependencies(${_chttp_host_provider}
      chttp_service_plugin_fixture_idl_codegen)
    set_target_properties(${_chttp_host_provider} PROPERTIES
      FOLDER "app/tests/service" C_STANDARD 11
      C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  endforeach()
  target_compile_definitions(chttp_service_host_provider_second PRIVATE
    CHTTP_COMPONENT_PLUGIN_ID="test.chttp.servicehost-provider.2")
  target_link_libraries(chttp_service_http_test PRIVATE
    ${CHTTP_SERVICE_HOST_LINK_TARGET})
  target_include_directories(chttp_service_http_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src")
  target_compile_definitions(chttp_service_http_test PRIVATE
    CHTTP_ENABLE_SERVICE_HOST_CANDIDATE=1
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_http_test
    chttp_service_host_provider_fixture chttp_service_host_provider_second)

  # A separate real HTTP server test with exactly one port/listener and two
  # generations. Binding admission is prepared once before generation publish;
  # in-flight scopes, worker terminal and reverse DSO unload are exercised.
  add_executable(chttp_service_host_single_listener_test
    chttp_service_host_single_listener_test.c)
  target_link_libraries(chttp_service_host_single_listener_test PRIVATE
    CHttp::App CHttp::Client ${CHTTP_SERVICE_HOST_LINK_TARGET}
    Salts::CFlow Salts::Platform)
  target_include_directories(chttp_service_host_single_listener_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src"
    "${chttp_service_plugin_fixture_GENERATED_DIR}")
  target_compile_definitions(chttp_service_host_single_listener_test PRIVATE
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_host_single_listener_test
    chttp_service_plugin_fixture_idl_codegen
    chttp_service_host_provider_fixture chttp_service_host_provider_second)
  set_target_properties(chttp_service_host_single_listener_test PROPERTIES
    FOLDER "app/tests/service" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME chttp_service_host_single_listener_test
    COMMAND chttp_service_host_single_listener_test)
  set_tests_properties(chttp_service_host_single_listener_test
    PROPERTIES TIMEOUT 90)

  # Exact CHttp Service MethodPlan/DataBind/Interceptor/deferred execution,
  # rather than the standalone test-only manually encoded HTTP handler.
  add_executable(chttp_service_host_methodplan_switch_test
    chttp_service_host_methodplan_switch_test.c)
  target_link_libraries(chttp_service_host_methodplan_switch_test PRIVATE
    CHttp::App CHttp::Client ${CHTTP_SERVICE_HOST_LINK_TARGET}
    chttp_service_plugin_fixture_native)
  target_include_directories(chttp_service_host_methodplan_switch_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src"
    "${chttp_service_plugin_fixture_GENERATED_DIR}")
  target_compile_definitions(chttp_service_host_methodplan_switch_test PRIVATE
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_host_methodplan_switch_test
    chttp_service_plugin_fixture_idl_codegen
    chttp_service_host_provider_fixture
    chttp_service_host_provider_second)
  set_target_properties(chttp_service_host_methodplan_switch_test PROPERTIES
    FOLDER "app/tests/service" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME chttp_service_host_methodplan_switch_test
    COMMAND chttp_service_host_methodplan_switch_test)
  set_tests_properties(chttp_service_host_methodplan_switch_test
    PROPERTIES TIMEOUT 90)

  # The very same MethodPlan/ServiceHost lifecycle on one explicit h2c
  # listener. H2 prior knowledge never falls back to HTTP/1.1.
  add_executable(chttp_service_host_methodplan_h2c_test
    chttp_service_host_methodplan_switch_test.c)
  target_link_libraries(chttp_service_host_methodplan_h2c_test PRIVATE
    CHttp::App CHttp::Client ${CHTTP_SERVICE_HOST_LINK_TARGET}
    chttp_service_plugin_fixture_native)
  target_include_directories(chttp_service_host_methodplan_h2c_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src"
    "${chttp_service_plugin_fixture_GENERATED_DIR}")
  target_compile_definitions(chttp_service_host_methodplan_h2c_test PRIVATE
    CHTTP_SERVICE_HOST_TEST_H2=1
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_host_methodplan_h2c_test
    chttp_service_plugin_fixture_idl_codegen
    chttp_service_host_provider_fixture
    chttp_service_host_provider_second)
  set_target_properties(chttp_service_host_methodplan_h2c_test PROPERTIES
    FOLDER "app/tests/service" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME chttp_service_host_methodplan_h2c_test
    COMMAND chttp_service_host_methodplan_h2c_test)
  set_tests_properties(chttp_service_host_methodplan_h2c_test
    PROPERTIES TIMEOUT 90)

  # One advanced H2 client, one physical-session capacity and two concurrent
  # H2 stream slots. Verify stream-local CANCEL, sibling isolation and bounded
  # FULL admission while the real MethodPlan Provider spans N+2 -> N+3.
  add_executable(chttp_service_host_h2_multiplex_test
    chttp_service_host_methodplan_switch_test.c)
  target_link_libraries(chttp_service_host_h2_multiplex_test PRIVATE
    CHttp::App CHttp::Client ${CHTTP_SERVICE_HOST_LINK_TARGET}
    chttp_service_plugin_fixture_native)
  target_include_directories(chttp_service_host_h2_multiplex_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src"
    "${chttp_service_plugin_fixture_GENERATED_DIR}")
  target_compile_definitions(chttp_service_host_h2_multiplex_test PRIVATE
    CHTTP_SERVICE_HOST_TEST_H2=1
    CHTTP_SERVICE_HOST_TEST_MULTIPLEX=1
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_host_h2_multiplex_test
    chttp_service_plugin_fixture_idl_codegen
    chttp_service_host_provider_fixture
    chttp_service_host_provider_second)
  set_target_properties(chttp_service_host_h2_multiplex_test PROPERTIES
    FOLDER "app/tests/service" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME chttp_service_host_h2_multiplex_test
    COMMAND chttp_service_host_h2_multiplex_test)
  set_tests_properties(chttp_service_host_h2_multiplex_test
    PROPERTIES TIMEOUT 120)

  # Force generated 2/3-byte service responses through one-byte HTTP/2 stream
  # receive credit. Progress now requires actual per-stream WINDOW_UPDATE.
  # Keep the same single-session, two-request, CANCEL/sibling test invariants.
  add_executable(chttp_service_host_h2_stream_window_test
    chttp_service_host_methodplan_switch_test.c)
  target_link_libraries(chttp_service_host_h2_stream_window_test PRIVATE
    CHttp::App CHttp::Client ${CHTTP_SERVICE_HOST_LINK_TARGET}
    chttp_service_plugin_fixture_native)
  target_include_directories(chttp_service_host_h2_stream_window_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src"
    "${chttp_service_plugin_fixture_GENERATED_DIR}")
  target_compile_definitions(chttp_service_host_h2_stream_window_test PRIVATE
    CHTTP_SERVICE_HOST_TEST_H2=1
    CHTTP_SERVICE_HOST_TEST_MULTIPLEX=1
    CHTTP_SERVICE_HOST_TEST_WINDOW=1
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_host_h2_stream_window_test
    chttp_service_plugin_fixture_idl_codegen
    chttp_service_host_provider_fixture
    chttp_service_host_provider_second)
  set_target_properties(chttp_service_host_h2_stream_window_test PROPERTIES
    FOLDER "app/tests/service" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME chttp_service_host_h2_stream_window_test
    COMMAND chttp_service_host_h2_stream_window_test)
  set_tests_properties(chttp_service_host_h2_stream_window_test
    PROPERTIES TIMEOUT 120)

  # Two 48-KiB H2 Source/Sink streams: each remains below initial per-Stream
  # window, while aggregate response bytes exceed 65,535 connection credit.
  # Retain a real ServiceHost Scope through the Source's terminal callback.
  add_executable(chttp_service_host_h2_connection_window_test
    chttp_service_host_h2_connection_window_test.c)
  target_link_libraries(chttp_service_host_h2_connection_window_test PRIVATE
    CHttp::App CHttp::Client ${CHTTP_SERVICE_HOST_LINK_TARGET}
    chttp_service_plugin_fixture_native)
  target_include_directories(chttp_service_host_h2_connection_window_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src"
    "${chttp_service_plugin_fixture_GENERATED_DIR}")
  target_compile_definitions(chttp_service_host_h2_connection_window_test PRIVATE
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_host_h2_connection_window_test
    chttp_service_plugin_fixture_idl_codegen
    chttp_service_host_provider_fixture
    chttp_service_host_provider_second)
  set_target_properties(chttp_service_host_h2_connection_window_test PROPERTIES
    FOLDER "app/tests/service" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME chttp_service_host_h2_connection_window_test
    COMMAND chttp_service_host_h2_connection_window_test)
  set_tests_properties(chttp_service_host_h2_connection_window_test
    PROPERTIES TIMEOUT 120)

  # Keep normal 2x48KiB positive intact; isolate RST_STREAM cancellation
  # with 1-byte Stream receive credit and same-session sibling validation.
  add_executable(chttp_service_host_h2_connection_cancel_test
    chttp_service_host_h2_connection_window_test.c)
  target_link_libraries(chttp_service_host_h2_connection_cancel_test PRIVATE
    CHttp::App CHttp::Client ${CHTTP_SERVICE_HOST_LINK_TARGET}
    chttp_service_plugin_fixture_native)
  target_include_directories(chttp_service_host_h2_connection_cancel_test PRIVATE
    "${PROJECT_SOURCE_DIR}/app/src"
    "${chttp_service_plugin_fixture_GENERATED_DIR}")
  target_compile_definitions(chttp_service_host_h2_connection_cancel_test PRIVATE
    CHTTP_SERVICE_HOST_FLOW_CANCEL=1
    GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH="$<TARGET_FILE:chttp_service_host_provider_fixture>"
    GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH="$<TARGET_FILE:chttp_service_host_provider_second>")
  add_dependencies(chttp_service_host_h2_connection_cancel_test
    chttp_service_plugin_fixture_idl_codegen
    chttp_service_host_provider_fixture
    chttp_service_host_provider_second)
  set_target_properties(chttp_service_host_h2_connection_cancel_test PROPERTIES
    FOLDER "app/tests/service" C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME chttp_service_host_h2_connection_cancel_test
    COMMAND chttp_service_host_h2_connection_cancel_test)
  set_tests_properties(chttp_service_host_h2_connection_cancel_test
    PROPERTIES TIMEOUT 120)

  if(NOT CHTTP_USE_INSTALLED_SERVICE_HOST_ABI_PROBE)
  # Private staging gate: compile original Salts candidate archives once, copy
  # only their built files/headers to a throwaway root, then configure two
  # entirely independent C11/C++17 consumers against the *installed* Salts
  # release and two real Provider DSOs. Never install/export the candidate.
  add_test(NAME chttp_service_host_staged_sdk_consumer
    COMMAND "${CMAKE_COMMAND}"
      "-DEXT_STAGE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/private-staged-sdk"
      "-DEXT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/private_sdk"
      "-DEXT_SALTS_ROOT=${SALTS_ROOT_PATH}"
      "-DEXT_UTILS_ROOT=${SALTS_UTILS_ROOT_PATH}"
      "-DEXT_HEADERS=${CHTTP_SALTS_CANDIDATE_SOURCE_DIR}/component-runtime/include"
      "-DEXT_CONFIG_ARCHIVE=$<TARGET_FILE:salts_configurator_candidate>"
      "-DEXT_HOST_ARCHIVE=$<TARGET_FILE:salts_service_host_candidate>"
      "-DEXT_PROVIDER1=$<TARGET_FILE:chttp_service_host_provider_fixture>"
      "-DEXT_PROVIDER2=$<TARGET_FILE:chttp_service_host_provider_second>"
      "-DEXT_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DEXT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DEXT_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      "-DEXT_GENERATOR=${CMAKE_GENERATOR}"
      "-DEXT_ASAN=${ENABLE_SANITIZER_ADDRESS}"
      "-DEXT_UBSAN=${ENABLE_SANITIZER_UNDEFINED}"
      "-DEXT_CTEST=${CMAKE_CTEST_COMMAND}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/private_sdk/stage.cmake")
  set_tests_properties(chttp_service_host_staged_sdk_consumer
    PROPERTIES TIMEOUT 120)
  endif()
  # The installed strict CMeta SDK introduced this validator after the
  # published Salts 2.3 CMeta ABI. Do not compile/link strict helper calls
  # against the old published interface in the private-stage comparison.
  if(CHTTP_USE_INSTALLED_SERVICE_HOST_ABI_PROBE)
  # Admission must use the exact reflected cross-DSO FR1 method ABI:
  # no legacy R1 inference for MethodPlan operations in the strict Host.
  foreach(_chttp_fr_lang IN ITEMS c cpp)
    if(_chttp_fr_lang STREQUAL "c")
      set(_chttp_fr_target chttp_service_host_strict_fr_abi_c)
      add_executable(${_chttp_fr_target}
        chttp_service_host_strict_fr_abi_test.c)
      set_target_properties(${_chttp_fr_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    else()
      set(_chttp_fr_target chttp_service_host_strict_fr_abi_cpp)
      add_executable(${_chttp_fr_target}
        chttp_service_host_strict_fr_abi_test.cpp)
      set_target_properties(${_chttp_fr_target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
      if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${_chttp_fr_target} PRIVATE -pedantic-errors)
      endif()
    endif()
    target_include_directories(${_chttp_fr_target} PRIVATE
      "${PROJECT_SOURCE_DIR}/app/src")
    target_link_libraries(${_chttp_fr_target} PRIVATE
      Salts::CMeta Salts::DataBind)
    add_test(NAME ${_chttp_fr_target} COMMAND ${_chttp_fr_target})
    set_target_properties(${_chttp_fr_target} PROPERTIES
      FOLDER "app/tests/service")
  endforeach()
  endif()
endif()

add_test(NAME chttp_service_http_test COMMAND chttp_service_http_test)
if(CHTTP_ENABLE_SERVICE_HOST_CANDIDATE)
  set_tests_properties(chttp_service_http_test PROPERTIES TIMEOUT 180)
endif()

set_target_properties(chttp_service_http_test PROPERTIES
  FOLDER "app/tests/service"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

salts_idl_target(
  TARGET chttp_service_generic_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_generic.schema"
  ARTIFACT_NAME "chttp_service_generic"
  ARTIFACTS MESSAGE
  BINARY_CODEC)

add_executable(chttp_service_generic_cross_tu_test
  chttp_service_generic_cross_tu_test.c
  chttp_service_generic_identity_a.c
  chttp_service_generic_identity_b.c)

target_link_libraries(chttp_service_generic_cross_tu_test PRIVATE
  chttp_service_generic_fixture_native
  Salts::CMeta
  Salts::DataBind)

target_include_directories(chttp_service_generic_cross_tu_test PRIVATE
  "${chttp_service_generic_fixture_GENERATED_DIR}")

add_dependencies(chttp_service_generic_cross_tu_test
  chttp_service_generic_fixture_idl_codegen)

add_test(NAME chttp_service_generic_cross_tu_test
  COMMAND chttp_service_generic_cross_tu_test)

set_target_properties(chttp_service_generic_cross_tu_test PROPERTIES
  FOLDER "app/tests/service"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_executable(chttp_service_header_cpp_test
  chttp_service_header_cpp_test.cpp)

target_link_libraries(chttp_service_header_cpp_test PRIVATE
  CHttp::App)

add_test(NAME chttp_service_header_cpp_test
  COMMAND chttp_service_header_cpp_test)

set_target_properties(chttp_service_header_cpp_test PROPERTIES
  FOLDER "app/tests/service"
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
    -DSERVICE_SOURCE=${PROJECT_SOURCE_DIR}/app/src/chttp_service.c
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_single_materialization_contract_test.cmake)


add_test(
  NAME chttp_service_hot_path_boundary_contract
  COMMAND ${CMAKE_COMMAND}
    -DSERVICE_SOURCE=${PROJECT_SOURCE_DIR}/app/src/chttp_service.c
    -DRPC_SERVICE_SOURCE=${PROJECT_SOURCE_DIR}/http_server/rpc/chttp_server_rpc_service.c
    -DSERVICE_HEADER=${PROJECT_SOURCE_DIR}/app/include/chttp_app/service.h
    -DSERVER_HEADER=${PROJECT_SOURCE_DIR}/http_server/include/http_server/http.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_hot_path_boundary_contract_test.cmake)

add_test(
  NAME chttp_service_lifecycle_contract
  COMMAND ${CMAKE_COMMAND}
    -DSERVICE_SOURCE=${PROJECT_SOURCE_DIR}/app/src/chttp_service.c
    -P ${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_lifecycle_contract_test.cmake)
