if(NOT DEFINED SERVICE_SOURCE OR SERVICE_SOURCE STREQUAL "")
  message(FATAL_ERROR "SERVICE_SOURCE is required")
endif()
if(NOT DEFINED SERVER_HEADER OR SERVER_HEADER STREQUAL "")
  message(FATAL_ERROR "SERVER_HEADER is required")
endif()

file(READ "${SERVICE_SOURCE}" SERVICE_TEXT)
file(READ "${SERVER_HEADER}" SERVER_TEXT)

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

string(FIND "${SERVICE_TEXT}" "chttp_server_response_defer" DEFER_POS)
if(NOT DEFER_POS EQUAL -1)
  message(FATAL_ERROR "Current synchronous Service epoch must not silently introduce deferred request ownership")
endif()

foreach(FORBIDDEN "DataBind" "cmeta_" "cflow_" "salts_plugin")
  string(FIND "${SERVER_TEXT}" "${FORBIDDEN}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "Base Server public API must remain semantic-runtime neutral: ${FORBIDDEN}")
  endif()
endforeach()
