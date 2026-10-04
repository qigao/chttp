if(NOT DEFINED SERVICE_SOURCE OR SERVICE_SOURCE STREQUAL "")
  message(FATAL_ERROR "SERVICE_SOURCE is required")
endif()
if(NOT DEFINED RPC_SERVICE_SOURCE OR RPC_SERVICE_SOURCE STREQUAL "")
  message(FATAL_ERROR "RPC_SERVICE_SOURCE is required")
endif()
if(NOT DEFINED SERVICE_HEADER OR SERVICE_HEADER STREQUAL "")
  message(FATAL_ERROR "SERVICE_HEADER is required")
endif()
if(NOT DEFINED SERVER_HEADER OR SERVER_HEADER STREQUAL "")
  message(FATAL_ERROR "SERVER_HEADER is required")
endif()

file(READ "${SERVICE_SOURCE}" SERVICE_TEXT)
file(READ "${RPC_SERVICE_SOURCE}" RPC_SERVICE_TEXT)
file(READ "${SERVICE_HEADER}" SERVICE_HEADER_TEXT)
file(READ "${SERVER_HEADER}" SERVER_TEXT)

foreach(SEMANTIC_SOURCE IN ITEMS SERVICE_TEXT RPC_SERVICE_TEXT)
  foreach(REQUIRED_SEMANTIC
      "CMETA_RESULT_VALUE"
      "CMETA_PARAM_IN | CMETA_PARAM_BORROWED"
      "CMETA_PARAM_OUT | CMETA_PARAM_BORROWED")
    string(FIND "${${SEMANTIC_SOURCE}}" "${REQUIRED_SEMANTIC}" POS)
    if(POS EQUAL -1)
      message(FATAL_ERROR
        "Generated Service admission must consume canonical CMeta ownership semantics: ${REQUIRED_SEMANTIC}")
    endif()
  endforeach()
endforeach()

string(FIND "${SERVICE_TEXT}" "static int chttp_service_http_execute(" EXEC_START)
string(FIND "${SERVICE_TEXT}" "static int chttp_service_http_handler(" EXEC_END)
if(EXEC_START LESS 0 OR EXEC_END LESS 0 OR EXEC_END LESS_EQUAL EXEC_START)
  message(FATAL_ERROR "Could not isolate Service request execution hot path")
endif()
math(EXPR EXEC_LENGTH "${EXEC_END} - ${EXEC_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${EXEC_START} ${EXEC_LENGTH} EXEC_TEXT)

foreach(FORBIDDEN
    "cmeta_function_desc_equal"
    "cmeta_function_abi_desc_valid"
    "data_bind_native_execution_valid"
    "data_bind_http_method_plan_binding"
    "cflow_function_projection"
    "salts_plugin"
    "FunctionMeta("
    "FunctionAbi("
    "registry")
  string(FIND "${EXEC_TEXT}" "${FORBIDDEN}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "Hot path must consume admitted state; found: ${FORBIDDEN}")
  endif()
endforeach()

foreach(REQUIRED
    "binding = record->binding"
    "data_bind_binding_plan_bind_inputs"
    "record->execution->invoke"
    "data_bind_binding_plan_write_outcome"
    "chttp_server_reply_buffer")
  string(FIND "${EXEC_TEXT}" "${REQUIRED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "Missing admitted hot-path contract marker: ${REQUIRED}")
  endif()
endforeach()

string(FIND "${SERVICE_TEXT}" "static void chttp_service_deferred_direct_run(" PLUGIN_RUN_START)
string(FIND "${SERVICE_TEXT}" "static void chttp_service_deferred_cflow_run(" PLUGIN_RUN_END)
if(PLUGIN_RUN_START LESS 0 OR PLUGIN_RUN_END LESS 0 OR
   PLUGIN_RUN_END LESS_EQUAL PLUGIN_RUN_START)
  message(FATAL_ERROR "Could not isolate deferred direct/Plugin worker path")
endif()
math(EXPR PLUGIN_RUN_LENGTH "${PLUGIN_RUN_END} - ${PLUGIN_RUN_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${PLUGIN_RUN_START} ${PLUGIN_RUN_LENGTH}
       PLUGIN_RUN_TEXT)

foreach(FORBIDDEN_PLUGIN_HOT
    "salts_plugin_registry_"
    "salts_plugin_manifest_"
    "data_bind_plugin_catalog_"
    "data_bind_plugin_operation_execution_admit"
    "FunctionMeta("
    "FunctionAbi(")
  string(FIND "${PLUGIN_RUN_TEXT}" "${FORBIDDEN_PLUGIN_HOT}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Deferred Plugin worker must consume cached exact execution only: ${FORBIDDEN_PLUGIN_HOT}")
  endif()
endforeach()

foreach(REQUIRED_PLUGIN_CONTROL
    "salts_plugin_registry_acquire"
    "DATA_BIND_PLUGIN_CATALOG_EXPORT_ID"
    "data_bind_plugin_operation_execution_admit"
    "salts_plugin_registry_release")
  string(FIND "${SERVICE_TEXT}" "${REQUIRED_PLUGIN_CONTROL}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "Plugin mount/release control plane is missing: ${REQUIRED_PLUGIN_CONTROL}")
  endif()
endforeach()

string(FIND "${SERVICE_TEXT}" "static void chttp_service_deferred_cflow_run(" CFLOW_RUN_START)
string(FIND "${SERVICE_TEXT}" "static int chttp_service_http_execute_deferred(" CFLOW_RUN_END)
if(CFLOW_RUN_START LESS 0 OR CFLOW_RUN_END LESS 0 OR
   CFLOW_RUN_END LESS_EQUAL CFLOW_RUN_START)
  message(FATAL_ERROR "Could not isolate deferred CFlow worker path")
endif()
math(EXPR CFLOW_RUN_LENGTH "${CFLOW_RUN_END} - ${CFLOW_RUN_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${CFLOW_RUN_START} ${CFLOW_RUN_LENGTH}
       CFLOW_RUN_TEXT)

foreach(FORBIDDEN_CFLOW_HOT
    "cmeta_function_desc_equal"
    "cmeta_function_abi_desc_valid"
    "data_bind_http_method_plan_binding"
    "cflow_graph_init"
    "cflow_graph_add_"
    "cflow_plan_compile"
    "cflow_function_typed_adapter_projection"
    "FunctionMeta("
    "FunctionAbi("
    "registry")
  string(FIND "${CFLOW_RUN_TEXT}" "${FORBIDDEN_CFLOW_HOT}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "CFlow worker must consume the mounted Plan without control-plane lookup/build: ${FORBIDDEN_CFLOW_HOT}")
  endif()
endforeach()

foreach(REQUIRED_CFLOW_HOT
    "cflow_plan_eval_array"
    "record->cflow_plan"
    "chttp_service_deferred_publish"
    "cflow_result_destroy")
  string(FIND "${CFLOW_RUN_TEXT}" "${REQUIRED_CFLOW_HOT}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "Deferred CFlow worker is missing immutable-Plan marker: ${REQUIRED_CFLOW_HOT}")
  endif()
endforeach()

foreach(REQUIRED_CFLOW_CONTROL
    "cflow_graph_add_function_typed_adapter_projection"
    "cflow_plan_compile_surface")
  string(FIND "${SERVICE_TEXT}" "${REQUIRED_CFLOW_CONTROL}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "CFlow mount must compile the producer projection once: ${REQUIRED_CFLOW_CONTROL}")
  endif()
endforeach()

foreach(REQUIRED_FAILURE_CLASS
    "Binding Error"
    "Validation Error"
    "Binding Limit Error"
    "Application Error")
  string(FIND "${SERVICE_TEXT}" "${REQUIRED_FAILURE_CLASS}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "Service HTTP failure classes must remain distinguishable: ${REQUIRED_FAILURE_CLASS}")
  endif()
endforeach()

string(FIND "${SERVICE_TEXT}" "\"Bad Request\"" COLLAPSED_BAD_REQUEST)
if(NOT COLLAPSED_BAD_REQUEST EQUAL -1)
  message(FATAL_ERROR "Generated Service binding failures must not collapse back into generic Bad Request")
endif()

string(FIND "${SERVICE_TEXT}" "typedef struct chttp_service_method_record {" RECORD_START)
string(FIND "${SERVICE_TEXT}" "} chttp_service_method_record;" RECORD_END)
if(RECORD_START LESS 0 OR RECORD_END LESS 0 OR RECORD_END LESS_EQUAL RECORD_START)
  message(FATAL_ERROR "Could not isolate mounted operation record")
endif()
math(EXPR RECORD_LENGTH "${RECORD_END} - ${RECORD_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${RECORD_START} ${RECORD_LENGTH} RECORD_TEXT)
string(FIND "${RECORD_TEXT}" "chttp_server_request_view" REQUEST_CAPTURE)
if(NOT REQUEST_CAPTURE EQUAL -1)
  message(FATAL_ERROR "Mounted operation must not retain callback-scoped HTTP request views")
endif()

foreach(FORBIDDEN_MUTABLE
    "request_storage"
    "response_storage"
    "error_storage"
    "DataBindBindingCallFrame"
    "scalar_scratch"
    "native_workspace")
  string(FIND "${RECORD_TEXT}" "${FORBIDDEN_MUTABLE}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Mounted operation must not own request-scoped mutable state: ${FORBIDDEN_MUTABLE}")
  endif()
endforeach()

string(FIND "${SERVICE_TEXT}" "typedef struct chttp_service_invocation {" INVOCATION_START)
string(FIND "${SERVICE_TEXT}" "} chttp_service_invocation;" INVOCATION_END)
if(INVOCATION_START LESS 0 OR INVOCATION_END LESS 0 OR
   INVOCATION_END LESS_EQUAL INVOCATION_START)
  message(FATAL_ERROR "Could not isolate per-call Service invocation state")
endif()
math(EXPR INVOCATION_LENGTH "${INVOCATION_END} - ${INVOCATION_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${INVOCATION_START} ${INVOCATION_LENGTH}
       INVOCATION_TEXT)
foreach(REQUIRED_OWNED
    "request_storage"
    "response_storage"
    "error_storage"
    "DataBindBindingCallFrame"
    "scalar_scratch"
    "native_workspace")
  string(FIND "${INVOCATION_TEXT}" "${REQUIRED_OWNED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "Per-call invocation must own native request state: ${REQUIRED_OWNED}")
  endif()
endforeach()

string(FIND "${SERVICE_TEXT}" "executing" SINGLE_FLIGHT_POS)
if(NOT SINGLE_FLIGHT_POS EQUAL -1)
  message(FATAL_ERROR
    "Service must not serialize independent invocations through a global executing gate")
endif()

foreach(REQUIRED_DEFERRED
    "chttp_server_response_defer"
    "cflow_executor_try_post_task"
    "chttp_server_deferred_reply_buffer"
    "cflow_executor_task")
  string(FIND "${SERVICE_TEXT}" "${REQUIRED_DEFERRED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "Deferred Service execution is missing canonical ownership/executor marker: ${REQUIRED_DEFERRED}")
  endif()
endforeach()

string(FIND "${SERVICE_TEXT}" "cflow_executor_control_post_task" BLOCKING_POST)
if(NOT BLOCKING_POST EQUAL -1)
  message(FATAL_ERROR
    "HTTP owner path must not wait for bounded executor capacity")
endif()

string(FIND "${INVOCATION_TEXT}" "chttp_server_request_view" INVOCATION_REQUEST_VIEW)
if(NOT INVOCATION_REQUEST_VIEW EQUAL -1)
  message(FATAL_ERROR
    "Deferred invocation must not retain callback-scoped HTTP request views")
endif()

foreach(FORBIDDEN "DataBind" "cmeta_" "cflow_" "salts_plugin" "turbowasm")
  string(FIND "${SERVER_TEXT}" "${FORBIDDEN}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "Base Server public API must remain semantic-runtime neutral: ${FORBIDDEN}")
  endif()
endforeach()

foreach(FORBIDDEN_WASM_RUNTIME "turbowasm" "TurboWasm")
  string(FIND "${SERVICE_TEXT}" "${FORBIDDEN_WASM_RUNTIME}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Service runtime must consume DataBindNativeExecution without a WASM runtime dependency: ${FORBIDDEN_WASM_RUNTIME}")
  endif()
  string(FIND "${SERVICE_HEADER_TEXT}" "${FORBIDDEN_WASM_RUNTIME}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Service public API must not expose a WASM runtime dependency: ${FORBIDDEN_WASM_RUNTIME}")
  endif()
endforeach()
