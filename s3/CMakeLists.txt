add_library(chttp_s3 SHARED
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
add_library(CHttp::S3 ALIAS chttp_s3)
set_target_properties(chttp_s3 PROPERTIES
  EXPORT_NAME S3 FOLDER "s3"
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON LINKER_LANGUAGE CXX)
target_compile_features(chttp_s3 PUBLIC c_std_11)
target_include_directories(chttp_s3
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chttp_s3 PUBLIC CHttp::Client
  PRIVATE Salts::Core Salts::XmlParser OpenSSL::Crypto)
install(TARGETS chttp_s3 EXPORT HTTPServicesTargets)
install(DIRECTORY include/s3 DESTINATION include)
