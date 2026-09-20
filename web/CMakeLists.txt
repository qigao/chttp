add_library(chttp_web SHARED
  src/chttp_web.c
  src/chttp_web_context.c
  src/chttp_web_form.c
  src/chttp_web_session.c
  src/chttp_web_security.c
  src/chttp_web_sse.c)
add_library(CHttp::Web ALIAS chttp_web)
set_target_properties(chttp_web PROPERTIES
  EXPORT_NAME Web FOLDER "web"
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_compile_features(chttp_web PUBLIC c_std_11)
target_include_directories(chttp_web
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(chttp_web
  PUBLIC CHttp::Server Salts::CMeta
  PRIVATE Salts::JinjaCMeta Salts::DataBind Salts::Core OpenSSL::Crypto)

get_target_property(_chttp_server_links chttp_server LINK_LIBRARIES)
if("${_chttp_server_links}" MATCHES "Jinja|jinja")
  message(FATAL_ERROR "CHttp::Server must remain independent of Jinja")
endif()
get_target_property(_chttp_web_links chttp_web LINK_LIBRARIES)
if(NOT "${_chttp_web_links}" MATCHES "Salts::JinjaCMeta")
  message(FATAL_ERROR "CHttp::Web must own the Jinja integration dependency")
endif()

if(BUILD_EXAMPLES OR (BUILD_TESTS AND BUILD_TESTING))
  add_subdirectory(examples)
endif()

if(BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()
