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

# Semantic identity belongs to CMeta. Service/RpcService admission must
# compare canonical descriptors semantically rather than by descriptor address.
string(FIND "${SERVICE_TEXT}" "static int chttp_service_capability_admit(" SERVICE_ADMIT_START)
string(FIND "${SERVICE_TEXT}" "static int chttp_service_direct_admit(" SERVICE_ADMIT_END)
if(SERVICE_ADMIT_START LESS 0 OR SERVICE_ADMIT_END LESS 0 OR
   SERVICE_ADMIT_END LESS_EQUAL SERVICE_ADMIT_START)
  message(FATAL_ERROR "Could not isolate Service semantic admission")
endif()
math(EXPR SERVICE_ADMIT_LENGTH "${SERVICE_ADMIT_END} - ${SERVICE_ADMIT_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${SERVICE_ADMIT_START}
       ${SERVICE_ADMIT_LENGTH} SERVICE_ADMIT_TEXT)
string(REGEX REPLACE "[ \t\r\n]+" " " SERVICE_ADMIT_NORMALIZED
       "${SERVICE_ADMIT_TEXT}")

string(FIND "${SERVICE_ADMIT_TEXT}" "cmeta_function_desc_equal" POS)
if(POS EQUAL -1)
  message(FATAL_ERROR
    "Service admission must use canonical FunctionDesc semantic equality")
endif()

foreach(FORBIDDEN_IDENTITY
    "function == native->function"
    "function != native->function"
    "native->function == function"
    "native->function != function"
    "function == capability_function"
    "function != capability_function"
    "capability_function == function"
    "capability_function != function"
    "native->function == capability_function"
    "native->function != capability_function"
    "capability_function == native->function"
    "capability_function != native->function")
  string(FIND "${SERVICE_ADMIT_NORMALIZED}" "${FORBIDDEN_IDENTITY}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Service semantic admission must not use descriptor address identity: ${FORBIDDEN_IDENTITY}")
  endif()
endforeach()

string(FIND "${RPC_SERVICE_TEXT}" "static int chttp_rpc_service_execution_admit(" RPC_ADMIT_START)
string(FIND "${RPC_SERVICE_TEXT}" "static void chttp_rpc_service_method_release(" RPC_ADMIT_END)
if(RPC_ADMIT_START LESS 0 OR RPC_ADMIT_END LESS 0 OR
   RPC_ADMIT_END LESS_EQUAL RPC_ADMIT_START)
  message(FATAL_ERROR "Could not isolate RpcService semantic admission")
endif()
math(EXPR RPC_ADMIT_LENGTH "${RPC_ADMIT_END} - ${RPC_ADMIT_START}")
string(SUBSTRING "${RPC_SERVICE_TEXT}" ${RPC_ADMIT_START}
       ${RPC_ADMIT_LENGTH} RPC_ADMIT_TEXT)
string(REGEX REPLACE "[ \t\r\n]+" " " RPC_ADMIT_NORMALIZED
       "${RPC_ADMIT_TEXT}")

string(FIND "${RPC_ADMIT_TEXT}" "cmeta_function_desc_equal" POS)
if(POS EQUAL -1)
  message(FATAL_ERROR
    "RpcService admission must use canonical FunctionDesc semantic equality")
endif()

foreach(FORBIDDEN_IDENTITY
    "function == native->function"
    "function != native->function"
    "native->function == function"
    "native->function != function"
    "function == execution->function"
    "function != execution->function"
    "execution->function == function"
    "execution->function != function"
    "native->function == execution->function"
    "native->function != execution->function"
    "execution->function == native->function"
    "execution->function != native->function")
  string(FIND "${RPC_ADMIT_NORMALIZED}" "${FORBIDDEN_IDENTITY}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "RpcService semantic admission must not use descriptor address identity: ${FORBIDDEN_IDENTITY}")
  endif()
endforeach()

# CFlow admission joins producer projection types to DataBind native storage by
# canonical type identity. It must never require TypeDesc address equality.
string(FIND "${SERVICE_TEXT}" "static int chttp_service_cflow_admit(" CFLOW_ADMIT_START)
string(FIND "${SERVICE_TEXT}" "static int chttp_service_component_resolve(" CFLOW_ADMIT_END)
if(CFLOW_ADMIT_START LESS 0 OR CFLOW_ADMIT_END LESS 0 OR
   CFLOW_ADMIT_END LESS_EQUAL CFLOW_ADMIT_START)
  message(FATAL_ERROR "Could not isolate CFlow type admission")
endif()
math(EXPR CFLOW_ADMIT_LENGTH "${CFLOW_ADMIT_END} - ${CFLOW_ADMIT_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${CFLOW_ADMIT_START}
       ${CFLOW_ADMIT_LENGTH} CFLOW_ADMIT_TEXT)
string(REGEX REPLACE "[ \t\r\n]+" " " CFLOW_ADMIT_NORMALIZED
       "${CFLOW_ADMIT_TEXT}")

string(FIND "${CFLOW_ADMIT_TEXT}" "cmeta_type_equal" POS)
if(POS EQUAL -1)
  message(FATAL_ERROR
    "CFlow Service admission must use canonical TypeDesc semantic equality")
endif()

foreach(FORBIDDEN_IDENTITY
    "projection->input_type == native->request->data->storage_type"
    "projection->input_type != native->request->data->storage_type"
    "native->request->data->storage_type == projection->input_type"
    "native->request->data->storage_type != projection->input_type"
    "projection->output_type == native->response->data->storage_type"
    "projection->output_type != native->response->data->storage_type"
    "native->response->data->storage_type == projection->output_type"
    "native->response->data->storage_type != projection->output_type")
  string(FIND "${CFLOW_ADMIT_NORMALIZED}" "${FORBIDDEN_IDENTITY}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "CFlow Service admission must not use TypeDesc address identity: ${FORBIDDEN_IDENTITY}")
  endif()
endforeach()

# Generic constructor identity is upstream CMeta truth. Until #854 exposes the
# final owner surface, CHttp must not introduce owner-name string semantics.
foreach(SEMANTIC_SOURCE IN ITEMS SERVICE_TEXT RPC_SERVICE_TEXT)
  string(FIND "${${SEMANTIC_SOURCE}}" "owner_name" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Service admission must not introduce generic owner-name string identity")
  endif()
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
    "salts_component_plugin_"
    "cmeta_plugin_"
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
  message(FATAL_ERROR "Could not isolate deferred direct/Component worker path")
endif()
math(EXPR PLUGIN_RUN_LENGTH "${PLUGIN_RUN_END} - ${PLUGIN_RUN_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${PLUGIN_RUN_START} ${PLUGIN_RUN_LENGTH}
       PLUGIN_RUN_TEXT)

foreach(FORBIDDEN_COMPONENT_HOT
    "salts_component_plugin_"
    "cmeta_plugin_"
    "data_bind_plugin_"
    "FunctionMeta("
    "FunctionAbi(")
  string(FIND "${PLUGIN_RUN_TEXT}" "${FORBIDDEN_COMPONENT_HOT}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Deferred Component worker must consume cached exact execution only: ${FORBIDDEN_COMPONENT_HOT}")
  endif()
endforeach()

foreach(REQUIRED_COMPONENT_CONTROL
    "salts_component_plugin_scope_acquire"
    "salts_component_plugin_scope_find_service_from"
    "chttp_service_operation_provider_borrow_from_object"
    "salts_component_plugin_scope_release")
  string(FIND "${SERVICE_TEXT}" "${REQUIRED_COMPONENT_CONTROL}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "Component mount/release control plane is missing: ${REQUIRED_COMPONENT_CONTROL}")
  endif()
endforeach()

string(FIND "${SERVICE_TEXT}" "static void chttp_service_method_release(" METHOD_RELEASE_START)
string(FIND "${SERVICE_TEXT}" "static void chttp_service_invocation_release(" METHOD_RELEASE_END)
if(METHOD_RELEASE_START LESS 0 OR METHOD_RELEASE_END LESS 0 OR
   METHOD_RELEASE_END LESS_EQUAL METHOD_RELEASE_START)
  message(FATAL_ERROR "Could not isolate mounted Service release ordering")
endif()
math(EXPR METHOD_RELEASE_LENGTH "${METHOD_RELEASE_END} - ${METHOD_RELEASE_START}")
string(SUBSTRING "${SERVICE_TEXT}" ${METHOD_RELEASE_START}
       ${METHOD_RELEASE_LENGTH} METHOD_RELEASE_TEXT)
string(FIND "${METHOD_RELEASE_TEXT}" "cflow_plan_destroy" PLAN_DESTROY_POS)
string(FIND "${METHOD_RELEASE_TEXT}"
       "salts_component_plugin_scope_release" SCOPE_RELEASE_POS)
if(PLAN_DESTROY_POS EQUAL -1 OR SCOPE_RELEASE_POS EQUAL -1 OR
   SCOPE_RELEASE_POS LESS_EQUAL PLAN_DESTROY_POS)
  message(FATAL_ERROR
    "Mounted Service teardown must destroy dependent Plan state before releasing the Component generation scope")
endif()

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
