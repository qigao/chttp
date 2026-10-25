option(BUILD_TESTS "Build test suite" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
option(CHTTP_P384_SHA384_ACCEPTANCE_TESTS
       "Enable the candidate P-384/SHA-384 HTTPS compatibility gate" OFF)
set(CHTTP_X509_MATRIX_FIXTURE_DIR "" CACHE PATH
    "Optional Salts CNet fixture directory for the CHttp X.509 product matrix")
option(ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
option(ENABLE_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_SANITIZER_LEAK "Enable LeakSanitizer" OFF)
option(ENABLE_SANITIZER_THREAD "Enable ThreadSanitizer" OFF)
option(ENABLE_SANITIZER_MEMORY "Enable MemorySanitizer" OFF)

