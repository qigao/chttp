add_library(chttp_cjwt STATIC
  "src/cjwt.c"
  "src/jwe_salts.c"
  "src/jws_salts.c"
  "src/print.c"
  "src/utils.c")
cmake_config_target(chttp_cjwt ALIAS CHttp::JWT EXPORT_NAME JWT FOLDER "vendor/cjwt")
set_target_properties(chttp_cjwt PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_features(chttp_cjwt PRIVATE c_std_11)
target_include_directories(
  chttp_cjwt
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/cjwt"
          "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  chttp_cjwt
  PUBLIC Salts::JsonParser
  PRIVATE Salts::Crypto Salts::Core)

install(TARGETS chttp_cjwt EXPORT ChttpTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/cjwt/cjwt.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cjwt)
install(FILES LICENSES/Apache-2.0.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Chttp/cjwt)


if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
