set(TARGET_NAME salts_s3)
set(S3_LIBRARY_VERSION 1.0.0)
set(S3_ABI_VERSION 1)

find_package(OpenSSL REQUIRED)

add_library(${TARGET_NAME} SHARED
  src/s3_async.c
  src/s3_client.c
  src/s3_bucket.c
  src/s3_bucket_config.c
  src/s3_credentials.c
  src/s3_file.c
  src/s3_multipart.c
  src/s3_object.c
  src/s3_request.c
  src/s3_response.c
  src/s3_signer.c
  src/s3_sse.c
  src/s3_xml.c)
cmake_config_target(${TARGET_NAME}
  ALIAS Salts::S3
  FOLDER "s3"
  EXPORT_NAME S3
  VERSION ${S3_LIBRARY_VERSION}
  SOVERSION ${S3_ABI_VERSION})
target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
# BoringSSL archives contain C++ objects even though S3 exposes a C API.
set_target_properties(${TARGET_NAME} PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  LINKER_LANGUAGE CXX)
if(WIN32)
  set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME "salts_s3-${S3_ABI_VERSION}")
endif()
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Salts::CHTTP
  PRIVATE Salts::Core Salts::XmlParser OpenSSL::Crypto)

install(TARGETS ${TARGET_NAME} EXPORT HTTPServicesTargets)
install(
  DIRECTORY include/s3
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
