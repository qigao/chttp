set(LIBECC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libecc")

file(
  GLOB LIBECC_ARITHMETIC_SOURCES CONFIGURE_DEPENDS
  "${LIBECC_ROOT}/src/fp/fp*.c"
  "${LIBECC_ROOT}/src/nn/n*.c")

file(
  GLOB LIBECC_CURVE_SOURCES CONFIGURE_DEPENDS
  "${LIBECC_ROOT}/src/curves/*.c")

set(
  LIBECC_ED448_SOURCES
  ${LIBECC_ARITHMETIC_SOURCES}
  ${LIBECC_CURVE_SOURCES}
  "${LIBECC_ROOT}/src/hash/hash_algs.c"
  "${LIBECC_ROOT}/src/hash/shake.c"
  "${LIBECC_ROOT}/src/hash/shake256.c"
  "${LIBECC_ROOT}/src/sig/ec_key.c"
  "${LIBECC_ROOT}/src/sig/eddsa.c"
  "${LIBECC_ROOT}/src/sig/sig_algs.c"
  "${LIBECC_ROOT}/src/utils/utils.c"
  "${LIBECC_ROOT}/src/utils/utils_rand.c")

add_library(
  turbo_crypto STATIC
  turbo_crypto.c
  turbo_crypto_monocypher.c
  turbo_crypto_ed448.c
  turbo_crypto_xxtea.c
  turbo_crypto.h
  monocypher/monocypher.c
  monocypher/monocypher.h
  sha2/sha-256.c
  sha2/sha-256.h
  xxtea/xxtea.c
  xxtea/xxtea.h
  ${LIBECC_ED448_SOURCES})

target_include_directories(
  turbo_crypto
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  PRIVATE "${LIBECC_ROOT}/include")

# Keep libecc private and compile only the RFC 8032 Ed448 feature set. An
# explicit word size is required by MSVC and matches libecc's default on the
# repository's other supported desktop and mobile targets.
set_source_files_properties(
  turbo_crypto_ed448.c
  ${LIBECC_ED448_SOURCES}
  PROPERTIES COMPILE_DEFINITIONS
             "WITH_STDLIB;WITH_LIBECC_CONFIG_OVERRIDE;WITH_CURVE_WEI448;WITH_HASH_SHAKE256;WITH_SIG_EDDSA448;WORDSIZE=64")

target_compile_features(turbo_crypto PRIVATE c_std_11)
target_link_libraries(turbo_crypto PRIVATE Salts::Platform)

set_target_properties(
  turbo_crypto
  PROPERTIES FOLDER "chttp/internal"
             C_VISIBILITY_PRESET hidden)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
