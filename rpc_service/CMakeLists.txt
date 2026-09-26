add_library(chttp_rpc_service SHARED
  src/chttp_rpc_service.c)

add_library(CHttp::RpcService ALIAS chttp_rpc_service)

set_target_properties(chttp_rpc_service PROPERTIES
  EXPORT_NAME RpcService
  FOLDER "rpc_service"
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  LINKER_LANGUAGE C)

target_compile_features(chttp_rpc_service PUBLIC c_std_11)

target_include_directories(chttp_rpc_service
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(chttp_rpc_service
  PUBLIC CHttp::Server Salts::DataBind
  PRIVATE Salts::Core Salts::CSerde)

install(TARGETS chttp_rpc_service EXPORT ChttpTargets)
install(DIRECTORY include/chttp_rpc_service DESTINATION include)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
