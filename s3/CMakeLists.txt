cmake_add_source(CHTTP_S3_SOURCES DIRS src include/s3)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CHTTP_S3_SOURCES})
add_library(chttp_s3 SHARED ${CHTTP_S3_SOURCES})
cmake_config_target(chttp_s3 ALIAS CHttp::S3 EXPORT_NAME S3 FOLDER "s3")
set_target_properties(chttp_s3 PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON LINKER_LANGUAGE CXX)
target_compile_features(chttp_s3 PUBLIC c_std_11)
target_include_directories(chttp_s3
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chttp_s3 PUBLIC CHttp::Client
  PRIVATE Salts::Core Salts::XmlParser chttp_digest)
install(TARGETS chttp_s3 EXPORT ChttpTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/s3 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
