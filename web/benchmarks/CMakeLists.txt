add_executable(chttp_web_benchmark chttp_web_benchmark.c)
target_link_libraries(chttp_web_benchmark PRIVATE
  CHttp::Web Salts::Platform)
set_target_properties(chttp_web_benchmark PROPERTIES
  FOLDER "web/benchmarks"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
