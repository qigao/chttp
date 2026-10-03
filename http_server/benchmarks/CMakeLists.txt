add_executable(chttp_server_owner_benchmark
  chttp_server_owner_benchmark.c)
target_link_libraries(chttp_server_owner_benchmark PRIVATE
  CHttp::Server Salts::Platform)
set_target_properties(chttp_server_owner_benchmark PROPERTIES
  FOLDER "http_server/benchmarks"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
