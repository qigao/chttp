file(GLOB CHTTP_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${CHTTP_APP_SOURCES})
add_library(chttp_app SHARED ${CHTTP_APP_SOURCES})
add_library(CHttp::App ALIAS chttp_app)
set_target_properties(chttp_app PROPERTIES
  EXPORT_NAME App FOLDER "app"
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  LINKER_LANGUAGE C)
target_compile_features(chttp_app PUBLIC c_std_11 cxx_std_17)
target_include_directories(chttp_app PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(chttp_app
  PUBLIC CHttp::Server CMeta::ACE CMeta::CMeta Salts::DataBindPlugin CMeta::CFlow
  PRIVATE Salts::JinjaCMeta Salts::DataBind Salts::Core Salts::CSerde chttp_digest
  INTERFACE
    $<LINK_ONLY:Salts::JinjaCMeta>
    $<LINK_ONLY:Salts::DataBind>)
install(TARGETS chttp_app EXPORT ChttpTargets)
install(DIRECTORY include/chttp_app DESTINATION include)

if(BUILD_EXAMPLES OR BUILD_TESTS)
  add_subdirectory(examples)
endif()
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(BUILD_BENCHMARK)
  add_subdirectory(benchmarks)
endif()
