if(NOT DEFINED PROJECT_SOURCE_DIR OR PROJECT_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "PROJECT_SOURCE_DIR is required")
endif()

set(scan_roots
  http_common
  http_client
  http_server
  web
  s3
  vendor/cjwt)

set(production_files "${PROJECT_SOURCE_DIR}/CMakeLists.txt")
foreach(root IN LISTS scan_roots)
  file(GLOB_RECURSE root_files
    "${PROJECT_SOURCE_DIR}/${root}/*.c"
    "${PROJECT_SOURCE_DIR}/${root}/*.cc"
    "${PROJECT_SOURCE_DIR}/${root}/*.cpp"
    "${PROJECT_SOURCE_DIR}/${root}/*.h"
    "${PROJECT_SOURCE_DIR}/${root}/CMakeLists.txt")
  list(APPEND production_files ${root_files})
endforeach()

set(forbidden_patterns
  "<openssl/"
  "OpenSSL::"
  "find_package(OpenSSL"
  "EVP_"
  "OPENSSL_"
  "CRYPTO_memcmp"
  "HMAC(")

foreach(path IN LISTS production_files)
  file(READ "${path}" text)
  foreach(pattern IN LISTS forbidden_patterns)
    string(FIND "${text}" "${pattern}" pos)
    if(NOT pos EQUAL -1)
      message(FATAL_ERROR
        "Direct OpenSSL/provider dependency '${pattern}' is forbidden in ${path}; use Salts provider-neutral crypto APIs")
    endif()
  endforeach()
endforeach()
