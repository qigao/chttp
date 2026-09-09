cmake_add_benchmark(
  turbo_crypto_benchmark
  SOURCES benchmark_turbo_crypto.c
  LIBS turbo_crypto Salts::TinyTest
  FOLDER "chttp/internal/benchmarks")
