add_library(chttp_web STATIC src/chttp_web.c)
add_library(CHttp::Web ALIAS chttp_web)
set_target_properties(chttp_web PROPERTIES FOLDER "web")

target_compile_features(chttp_web PUBLIC c_std_11)
target_include_directories(chttp_web
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(chttp_web
  PUBLIC CHttp::Server Salts::CMeta
  PRIVATE Salts::JinjaCMeta)

if(BUILD_EXAMPLES OR (BUILD_TESTS AND BUILD_TESTING))
  add_subdirectory(examples)
endif()

if(BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()
