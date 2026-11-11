set(LIBECC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libecc")

file(GLOB LIBECC_ARITHMETIC_SOURCES CONFIGURE_DEPENDS
     "${LIBECC_ROOT}/src/fp/fp*.c" "${LIBECC_ROOT}/src/nn/n*.c")
file(GLOB LIBECC_CURVE_SOURCES CONFIGURE_DEPENDS
     "${LIBECC_ROOT}/src/curves/*.c")

set(LIBECC_SOURCES
    ${LIBECC_ARITHMETIC_SOURCES}
    ${LIBECC_CURVE_SOURCES}
    "${LIBECC_ROOT}/src/hash/hash_algs.c"
    "${LIBECC_ROOT}/src/hash/shake.c"
    "${LIBECC_ROOT}/src/hash/shake256.c"
    "${LIBECC_ROOT}/src/hash/sha256.c"
    "${LIBECC_ROOT}/src/hash/sha384.c"
    "${LIBECC_ROOT}/src/hash/sha512.c"
    "${LIBECC_ROOT}/src/hash/sha512_core.c"
    "${LIBECC_ROOT}/src/sig/ecdsa.c"
    "${LIBECC_ROOT}/src/sig/ecdsa_common.c"
    "${LIBECC_ROOT}/src/sig/ec_key.c"
    "${LIBECC_ROOT}/src/sig/eddsa.c"
    "${LIBECC_ROOT}/src/sig/sig_algs.c"
    "${LIBECC_ROOT}/src/utils/utils.c"
    "${LIBECC_ROOT}/src/utils/utils_rand.c")

add_library(chttp_cjwt_crypto STATIC
  src/crypto_eddsa.c src/crypto_ecdsa.c src/crypto_key_wrap.c
  src/crypto_symmetric.c src/crypto_key.c src/crypto_key_pem.c
  ${LIBECC_SOURCES})
set_target_properties(chttp_cjwt_crypto PROPERTIES
  FOLDER "vendor/cjwt" C_VISIBILITY_PRESET hidden)
target_include_directories(chttp_cjwt_crypto PUBLIC src PRIVATE "${LIBECC_ROOT}/include")
target_compile_features(chttp_cjwt_crypto PRIVATE c_std_11)
target_link_libraries(chttp_cjwt_crypto PRIVATE GmSSL::GmSSL)

# Keep libecc private and compile the JOSE curve/signature feature set. The
# explicit word size is required by MSVC and matches supported 64-bit targets.
set_source_files_properties(
  src/crypto_eddsa.c src/crypto_ecdsa.c ${LIBECC_SOURCES}
  PROPERTIES
    COMPILE_DEFINITIONS
    "WITH_STDLIB;WITH_LIBECC_CONFIG_OVERRIDE;WITH_CURVE_WEI448;WITH_CURVE_WEI25519;WITH_CURVE_SECP256R1;WITH_CURVE_SECP384R1;WITH_CURVE_SECP521R1;WITH_CURVE_SECP256K1;WITH_HASH_SHAKE256;WITH_HASH_SHA256;WITH_HASH_SHA384;WITH_HASH_SHA512;WITH_SIG_EDDSA448;WITH_SIG_EDDSA25519;WITH_SIG_ECDSA;WORDSIZE=64"
)

# Both providers have global SHA symbols with different context ABIs. Namespace
# libecc's implementation at compilation so static consumers can link both.
foreach(_hash sha256 sha384 sha512)
  foreach(_operation init update final)
    set_property(SOURCE src/crypto_eddsa.c src/crypto_ecdsa.c ${LIBECC_SOURCES}
      APPEND PROPERTY COMPILE_DEFINITIONS
      "${_hash}_${_operation}=chttp_cjwt_libecc_${_hash}_${_operation}")
  endforeach()
endforeach()

install(FILES "${LIBECC_ROOT}/LICENSE" DESTINATION share/licenses/Chttp/libecc)
