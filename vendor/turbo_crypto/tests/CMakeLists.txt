cmake_add_test(
  turbo_crypto_test
  SOURCES test_turbo_crypto.c
  LIBS turbo_crypto Salts::TinyTest
  FOLDER "chttp/internal/tests")
