add_executable(turbo_crypto_test test_turbo_crypto.c)
target_link_libraries(turbo_crypto_test PRIVATE turbo_crypto Salts::TinyTest)
add_test(NAME turbo_crypto_test COMMAND turbo_crypto_test)
set_target_properties(turbo_crypto_test PROPERTIES FOLDER "vendor/turbo_crypto/tests")
