cmake_add_source(CHTTP_SERVER_SOURCES DIRS src rpc include/http_server)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CHTTP_SERVER_SOURCES})
add_library(chttp_server SHARED ${CHTTP_SERVER_SOURCES}
  $<TARGET_OBJECTS:chttp_common>)
cmake_config_target(chttp_server ALIAS CHttp::Server EXPORT_NAME Server FOLDER "http_server")
set_target_properties(chttp_server PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON LINKER_LANGUAGE CXX)
target_compile_features(chttp_server PUBLIC c_std_11)
target_include_directories(chttp_server
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/http_common/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/vendor/cjwt/include)
target_link_libraries(chttp_server
  PUBLIC Salts::CNet CMeta::CMeta Salts::CSerde Salts::DataBind
  PRIVATE llhttp::llhttp_static Salts::Core CMeta::CFlow
          CMeta::Plugin Salts::URI Salts::JsonParser Salts::JsonCSerdeAdapter
          chttp_cjwt chttp_digest Salts::DateTimeParser CMeta::CSTL)
install(TARGETS chttp_server EXPORT ChttpTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/http_server DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(BUILD_BENCHMARK)
  add_subdirectory(benchmarks)
endif()

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
