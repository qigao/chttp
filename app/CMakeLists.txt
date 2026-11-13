cmake_add_source(CHTTP_APP_SOURCES DIRS src include/chttp_app)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CHTTP_APP_SOURCES})
add_library(chttp_app SHARED ${CHTTP_APP_SOURCES})
cmake_config_target(chttp_app ALIAS CHttp::App EXPORT_NAME App FOLDER "app")
set_target_properties(chttp_app PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  LINKER_LANGUAGE C)
target_compile_features(chttp_app PUBLIC c_std_11 cxx_std_17)
target_include_directories(chttp_app PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(chttp_app
  PUBLIC CHttp::Server CMeta::ACE CMeta::CMeta Salts::DataBindPlugin CMeta::CFlow
  PRIVATE Salts::JinjaCMeta Salts::DataBind Salts::Core Salts::CSerde chttp_digest
  INTERFACE
    $<LINK_ONLY:Salts::JinjaCMeta>
    $<LINK_ONLY:Salts::DataBind>)
install(TARGETS chttp_app EXPORT ChttpTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/chttp_app DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(BUILD_EXAMPLES OR BUILD_TESTS)
  add_subdirectory(examples)
endif()
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(BUILD_BENCHMARK)
  add_subdirectory(benchmarks)
endif()
