cmake_add_source(CHTTP_CLIENT_SOURCES DIRS src rpc include/http_client)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CHTTP_CLIENT_SOURCES})
add_library(chttp_client SHARED
  ${CHTTP_CLIENT_SOURCES}
  $<TARGET_OBJECTS:chttp_common>)
cmake_config_target(chttp_client ALIAS CHttp::Client EXPORT_NAME Client FOLDER "http_client")
set_target_properties(chttp_client PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON LINKER_LANGUAGE CXX)
target_compile_features(chttp_client PUBLIC c_std_11)
target_include_directories(chttp_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/http_common/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${PROJECT_SOURCE_DIR}/http_common/http
          ${PROJECT_SOURCE_DIR}/http_common/rpc
          ${PROJECT_SOURCE_DIR}/vendor/cjwt/include)
target_link_libraries(chttp_client
  PUBLIC Salts::CNet CMeta::CMeta Salts::CSerde Salts::DataBind
  PRIVATE llhttp::llhttp_static Salts::Core CMeta::CFlow
          Salts::URI Salts::JsonParser Salts::JsonCSerdeAdapter
          chttp_cjwt chttp_digest)
install(TARGETS chttp_client EXPORT ChttpTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/http_client DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
