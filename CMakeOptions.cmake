option(BUILD_TESTS "Build test suite" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
option(ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
option(ENABLE_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_SANITIZER_LEAK "Enable LeakSanitizer" OFF)
option(ENABLE_SANITIZER_THREAD "Enable ThreadSanitizer" OFF)
option(ENABLE_SANITIZER_MEMORY "Enable MemorySanitizer" OFF)

option(CHTTP_ENABLE_SERVICE_HOST_CANDIDATE "Build isolated ServiceHost HTTP conformance candidate (no SDK publication)" OFF)

# Qualification-only path for the experimental, nonpublished Salts installed
# Configurator/ServiceHost ABI. No automatic candidate/library fallback.
option(CHTTP_USE_INSTALLED_SERVICE_HOST_ABI_PROBE
       "PROBE ONLY: link real installed Salts::ServiceHost instead of private staged candidate" OFF)
if(CHTTP_USE_INSTALLED_SERVICE_HOST_ABI_PROBE AND
   NOT CHTTP_ENABLE_SERVICE_HOST_CANDIDATE)
  message(FATAL_ERROR
    "Installed ServiceHost probe requires CHTTP_ENABLE_SERVICE_HOST_CANDIDATE=ON")
endif()
