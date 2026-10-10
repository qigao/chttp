add_library(chttp_app SHARED
  ${PROJECT_SOURCE_DIR}/http_common/service/chttp_document.c
  src/chttp_service.c
  src/chttp_application.c
  src/chttp_service_accept.c
  src/chttp_web.c
  src/chttp_web_context.c
  src/chttp_web_form.c
  src/chttp_web_form_plan.c
  src/chttp_web_multipart.c
  src/chttp_web_upload.c
  src/chttp_web_auth.c
  src/chttp_web_assets.c
  src/chttp_web_validation.c
  src/chttp_web_session.c
  src/chttp_web_security.c
  src/chttp_web_sse.c)
add_library(CHttp::App ALIAS chttp_app)
set_target_properties(chttp_app PROPERTIES
  EXPORT_NAME App FOLDER "app"
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  LINKER_LANGUAGE C)
target_compile_features(chttp_app PUBLIC c_std_11 cxx_std_17)
target_include_directories(chttp_app PRIVATE ${PROJECT_SOURCE_DIR}/http_common/service)
target_include_directories(chttp_app PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(chttp_app
  PUBLIC CHttp::Server Salts::CMeta Salts::DataBindPlugin Salts::CFlow Salts::Component
  PRIVATE Salts::JinjaCMeta Salts::DataBind Salts::Core Salts::CSerde
  INTERFACE
    $<LINK_ONLY:Salts::JinjaCMeta>
    $<LINK_ONLY:Salts::DataBind>)
install(TARGETS chttp_app EXPORT ChttpTargets)
install(DIRECTORY include/chttp_app DESTINATION include)

if(BUILD_EXAMPLES OR (BUILD_TESTS AND BUILD_TESTING))
  add_subdirectory(examples)
endif()
if(BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()
if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
