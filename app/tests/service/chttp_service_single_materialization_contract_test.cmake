if(NOT DEFINED SERVICE_SOURCE OR SERVICE_SOURCE STREQUAL "")
  message(FATAL_ERROR "SERVICE_SOURCE is required")
endif()

file(READ "${SERVICE_SOURCE}" SOURCE_TEXT)

if(SOURCE_TEXT MATCHES "response_scratch")
  message(FATAL_ERROR "Service egress must not restore shared response_scratch")
endif()

foreach(REQUIRED
    "mem_get_buffer\\(mem_global\\(\\)"
    "cserde_writer_finish\\("
    "chttp_server_reply_buffer\\("
    "mem_buffer_release\\(")
  if(NOT SOURCE_TEXT MATCHES "${REQUIRED}")
    message(FATAL_ERROR "Missing retained single-materialization contract marker: ${REQUIRED}")
  endif()
endforeach()

if(SOURCE_TEXT MATCHES "record->owner->response_scratch")
  message(FATAL_ERROR "Generated Service success egress must not publish copied scratch bytes")
endif()
