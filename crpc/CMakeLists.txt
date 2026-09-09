set(TARGET_NAME salts_crpc)
set(CRPC_ABI_VERSION 2)

add_library(${TARGET_NAME} STATIC
  src/crpc_common.c
  src/crpc_client.c
  src/crpc_json.c
  src/crpc_request_reply.c
  src/crpc_server.c)
cmake_config_target(${TARGET_NAME}
  ALIAS Salts::CRPC
  FOLDER "crpc"
  EXPORT_NAME CRPC)
set_target_properties(${TARGET_NAME} PROPERTIES
  OUTPUT_NAME "salts_crpc-${CRPC_ABI_VERSION}")
target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Salts::CHTTP Salts::CMeta Salts::CSerde
  PRIVATE Salts::JsonCSerdeAdapter Salts::Core)

install(TARGETS ${TARGET_NAME} EXPORT HTTPServicesTargets)
install(
  DIRECTORY include/crpc
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
