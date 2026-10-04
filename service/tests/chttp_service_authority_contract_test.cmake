if(NOT DEFINED IDL_FILE OR IDL_FILE STREQUAL "")
  message(FATAL_ERROR "IDL_FILE is required")
endif()
if(NOT DEFINED PROJECTION_FILE OR PROJECTION_FILE STREQUAL "")
  message(FATAL_ERROR "PROJECTION_FILE is required")
endif()

file(READ "${IDL_FILE}" IDL_TEXT)
file(READ "${PROJECTION_FILE}" PROJECTION_TEXT)

foreach(REQUIRED_IDL
    "@Min(1)"
    "optional uint32 scale default 1;"
    "service Calc"
    "Add: AddRequest -> AddResponse;")
  string(FIND "${IDL_TEXT}" "${REQUIRED_IDL}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "Canonical IDL must own logical Service/schema semantics: ${REQUIRED_IDL}")
  endif()
endforeach()

foreach(FORBIDDEN_IDL
    "GET"
    "/add"
    "success_status"
    "response_body"
    "@route"
    "@http"
    "http.")
  string(FIND "${IDL_TEXT}" "${FORBIDDEN_IDL}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "Canonical Service IDL must remain transport-neutral: ${FORBIDDEN_IDL}")
  endif()
endforeach()

foreach(REQUIRED_HTTP
    "\"service\": \"Calc\""
    "\"operation\": \"Add\""
    "\"method\": \"GET\""
    "\"route\": \"/add/{left}\""
    "\"success_status\": 201"
    "\"location\": \"path\""
    "\"location\": \"query\""
    "\"location\": \"response_body\"")
  string(FIND "${PROJECTION_TEXT}" "${REQUIRED_HTTP}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR
      "HTTP projection must own transport mapping: ${REQUIRED_HTTP}")
  endif()
endforeach()

foreach(FORBIDDEN_HTTP
    "@Min"
    "\"optional\""
    "\"default\""
    "\"nullable\"")
  string(FIND "${PROJECTION_TEXT}" "${FORBIDDEN_HTTP}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR
      "HTTP projection must not become a second logical Schema authority: ${FORBIDDEN_HTTP}")
  endif()
endforeach()
