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

string(JSON HTTP_OPERATION_COUNT LENGTH "${PROJECTION_TEXT}" http operations)
if(NOT HTTP_OPERATION_COUNT EQUAL 1)
  message(FATAL_ERROR
    "HTTP projection fixture must contain exactly one operation")
endif()

string(JSON HTTP_SERVICE GET "${PROJECTION_TEXT}" http operations 0 service)
string(JSON HTTP_OPERATION GET "${PROJECTION_TEXT}" http operations 0 operation)
string(JSON HTTP_METHOD GET "${PROJECTION_TEXT}" http operations 0 method)
string(JSON HTTP_ROUTE GET "${PROJECTION_TEXT}" http operations 0 route)
string(JSON HTTP_STATUS GET "${PROJECTION_TEXT}" http operations 0 success_status)
if(NOT HTTP_SERVICE STREQUAL "Calc" OR
   NOT HTTP_OPERATION STREQUAL "Add" OR
   NOT HTTP_METHOD STREQUAL "GET" OR
   NOT HTTP_ROUTE STREQUAL "/add/{left}" OR
   NOT HTTP_STATUS EQUAL 201)
  message(FATAL_ERROR
    "HTTP projection must own Calc.Add GET /add/{left} -> 201")
endif()

string(JSON HTTP_FIELD_COUNT LENGTH
  "${PROJECTION_TEXT}" http operations 0 fields)
if(NOT HTTP_FIELD_COUNT EQUAL 4)
  message(FATAL_ERROR
    "HTTP projection fixture must contain four field mappings")
endif()

string(JSON HTTP_LEFT_LOCATION GET
  "${PROJECTION_TEXT}" http operations 0 fields 0 location)
string(JSON HTTP_RIGHT_LOCATION GET
  "${PROJECTION_TEXT}" http operations 0 fields 1 location)
string(JSON HTTP_SCALE_LOCATION GET
  "${PROJECTION_TEXT}" http operations 0 fields 2 location)
string(JSON HTTP_SUM_LOCATION GET
  "${PROJECTION_TEXT}" http operations 0 fields 3 location)
if(NOT HTTP_LEFT_LOCATION STREQUAL "path" OR
   NOT HTTP_RIGHT_LOCATION STREQUAL "query" OR
   NOT HTTP_SCALE_LOCATION STREQUAL "query" OR
   NOT HTTP_SUM_LOCATION STREQUAL "response_body")
  message(FATAL_ERROR
    "HTTP projection field locations do not match the canonical fixture")
endif()

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
