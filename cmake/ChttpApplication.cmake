include_guard(GLOBAL)
include(CMakeParseArguments)

# Generate a complete synchronous HTTP application from a schema and business
# implementations. No source is written into the user's source directory.
function(chttp_app_target)
  cmake_parse_arguments(PARSE_ARGV 0 APP "" "TARGET;IDL;CONFIGURE" "SOURCES;LIBRARIES;INCLUDES")
  if(APP_UNPARSED_ARGUMENTS OR APP_KEYWORDS_MISSING_VALUES OR
      NOT APP_TARGET OR NOT APP_IDL OR NOT APP_SOURCES)
    message(FATAL_ERROR "chttp_app_target requires TARGET, IDL and SOURCES; optional CONFIGURE, LIBRARIES and INCLUDES")
  endif()
  if(NOT APP_TARGET MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR
      (APP_CONFIGURE AND NOT APP_CONFIGURE MATCHES "^[A-Za-z_][A-Za-z0-9_]*$"))
    message(FATAL_ERROR "chttp_app_target TARGET and CONFIGURE must be C identifiers")
  endif()
  if(NOT SaltsUtils_IDL_APPLICATION_VERSION OR SaltsUtils_IDL_APPLICATION_VERSION LESS 2)
    message(FATAL_ERROR "chttp_app_target requires SaltsUtils_IDL_APPLICATION_VERSION >= 2 (not present in 4.3.0-rc.7)")
  endif()
  salts_idl_target(TARGET ${APP_TARGET}_contract IDL "${APP_IDL}"
    ARTIFACT_NAME ${APP_TARGET} BINARY_CODEC ARTIFACTS NATIVE
    TRANSPORTS HTTP SCHEMA_PROJECTION)
  target_sources(${APP_TARGET}_contract_native PRIVATE ${APP_SOURCES})
  target_include_directories(${APP_TARGET}_contract_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" ${APP_INCLUDES})
  target_link_libraries(${APP_TARGET}_contract_native PRIVATE CHttp::App ${APP_LIBRARIES})
  set(_generated "${${APP_TARGET}_contract_GENERATED_DIR}")
  foreach(_file application.h application.c main.c)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/application/${_file}.in"
      "${_generated}/${APP_TARGET}_${_file}" @ONLY)
  endforeach()
  add_library(${APP_TARGET}_application STATIC "${_generated}/${APP_TARGET}_application.c")
  target_link_libraries(${APP_TARGET}_application PUBLIC
    CHttp::App ${APP_TARGET}_contract_native)
  target_include_directories(${APP_TARGET}_application PUBLIC "${_generated}")
  add_dependencies(${APP_TARGET}_application ${APP_TARGET}_contract_idl_codegen)
  add_executable(${APP_TARGET} "${_generated}/${APP_TARGET}_main.c")
  target_link_libraries(${APP_TARGET} PRIVATE ${APP_TARGET}_application Salts::Core)
  set_target_properties(${APP_TARGET} ${APP_TARGET}_application PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endfunction()
