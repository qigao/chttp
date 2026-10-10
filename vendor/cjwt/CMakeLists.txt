add_library(chttp_cjwt STATIC
  "src/cjwt.c"
  "src/jwe_salts.c"
  "src/jws_salts.c"
  "src/print.c"
  "src/utils.c")
set_target_properties(chttp_cjwt PROPERTIES FOLDER "vendor/cjwt")
target_compile_features(chttp_cjwt PRIVATE c_std_11)
target_include_directories(
  chttp_cjwt
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
          "${CMAKE_CURRENT_SOURCE_DIR}/include/cjwt"
          "${CHTTP_CJWT_ROOT}/src")
target_link_libraries(
  chttp_cjwt
  PRIVATE Salts::JsonParser Salts::Crypto Salts::Core)


if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
