salts_idl_target(
  TARGET http_example_contract
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/http_example.schema"
  ARTIFACT_NAME http_example
  COMPONENT "HttpExample.CalculatorPlugin"
  VERSION "1.0.0"
  ARTIFACTS NATIVE PLUGIN
  BINARY_CODEC
  TRANSPORTS HTTP
  PROJECTION_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/http_example.projection.json"
  SOURCES http_example_business.c)
target_sources(http_example_contract_native PRIVATE http_example_business.c)
set_target_properties(http_example_contract_native http_example_contract_idl_codegen
  http_example_contract_plugin http_example_contract_plugin_client
  PROPERTIES FOLDER "http_server/examples")

# Freeze editable Jinja sources into the binary, without a runtime file loader.
foreach(name layout page)
  set(path "${CMAKE_CURRENT_SOURCE_DIR}/templates/${name}.html.jinja")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
  file(READ "${path}" source)
  string(REPLACE "\\" "\\\\" source "${source}")
  string(REPLACE "\"" "\\\"" source "${source}")
  string(REPLACE "\r" "" source "${source}")
  string(REPLACE "\n" "\\n\"\n\"" source "${source}")
  set(EXAMPLE_${name}_SOURCE "${source}")
endforeach()
configure_file(http_example_templates.h.in http_example_templates.h @ONLY)

# Private application composition shared by the CLI and real HTTP tests.
add_library(http_example_app STATIC
  http_example_configurator.c http_example_interceptor.c http_example_handlers.c)
target_compile_features(http_example_app PUBLIC c_std_11)
target_include_directories(http_example_app PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}" "${http_example_contract_GENERATED_DIR}"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(http_example_app PUBLIC
  CHttp::App http_example_contract_native
  http_example_contract_plugin_client Salts::Plugin Salts::CFlow Salts::Core)
add_dependencies(http_example_app http_example_contract_idl_codegen)
set_target_properties(http_example_app PROPERTIES FOLDER "http_server/examples")

if(BUILD_EXAMPLES)
  add_executable(crpc_server_example crpc_server_example.c)
  target_link_libraries(crpc_server_example PRIVATE http_example_app)
  add_dependencies(crpc_server_example http_example_contract_plugin)
  set_target_properties(crpc_server_example PROPERTIES FOLDER "http_server/examples")
endif()
