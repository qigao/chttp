add_executable(
  chttp_server_owner_benchmark
  chttp_server_owner_benchmark.c)

target_link_libraries(
  chttp_server_owner_benchmark
  PRIVATE CHttp::Server Salts::Platform Salts::Core)

target_include_directories(
  chttp_server_owner_benchmark
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/http_server/src
          ${PROJECT_SOURCE_DIR}/http_server/rpc)

set_target_properties(
  chttp_server_owner_benchmark
  PROPERTIES
    FOLDER "http_server/benchmarks"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

add_executable(
  chttp_server_ws_owner_benchmark
  chttp_server_ws_owner_benchmark.c)

target_link_libraries(
  chttp_server_ws_owner_benchmark
  PRIVATE CHttp::Server CHttp::Client
          Salts::Platform Salts::Core Salts::TinyTest)

target_include_directories(
  chttp_server_ws_owner_benchmark
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/http_common/tests/support
          ${PROJECT_SOURCE_DIR}/http_server/src
          ${PROJECT_SOURCE_DIR}/http_server/rpc)

set_target_properties(
  chttp_server_ws_owner_benchmark
  PROPERTIES
    FOLDER "http_server/benchmarks"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

add_executable(
  chttp_server_h2_tls_owner_benchmark
  chttp_server_h2_tls_owner_benchmark.c)

target_link_libraries(
  chttp_server_h2_tls_owner_benchmark
  PRIVATE CHttp::Server CHttp::Client
          Salts::Platform Salts::Core Salts::TinyTest)

target_include_directories(
  chttp_server_h2_tls_owner_benchmark
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/http_common/tests/support
          ${PROJECT_SOURCE_DIR}/http_server/src
          ${PROJECT_SOURCE_DIR}/http_server/rpc)

set_target_properties(
  chttp_server_h2_tls_owner_benchmark
  PROPERTIES
    FOLDER "http_server/benchmarks"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
