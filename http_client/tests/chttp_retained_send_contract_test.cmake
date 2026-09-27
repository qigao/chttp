if(NOT DEFINED PROJECT_SOURCE_DIR)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR is required")
endif()

set(production_files
  "http_server/src/chttp_server.c"
  "http_client/src/chttp_client.c"
  "http_client/src/chttp_h2_session.c"
  "http_client/src/chttp_websocket_client.c"
  "http_client/src/chttp_websocket_pool.c")

foreach(path IN LISTS production_files)
  file(READ "${PROJECT_SOURCE_DIR}/${path}" text)

  string(REGEX MATCH "(^|[^A-Za-z0-9_])cnet_send\\(" copied_send "${text}")
  if(NOT copied_send STREQUAL "")
    message(FATAL_ERROR "copied cnet_send() returned to production source: ${path}")
  endif()

  string(REGEX MATCH "(^|[^A-Za-z0-9_])cnet_send_and_close\\(" copied_close "${text}")
  if(NOT copied_close STREQUAL "")
    message(FATAL_ERROR "copied cnet_send_and_close() returned to production source: ${path}")
  endif()

  string(FIND "${text}" "chttp_cnet_retained_" retained_marker)
  if(retained_marker EQUAL -1)
    message(FATAL_ERROR "retained-send ownership marker missing from production source: ${path}")
  endif()
endforeach()

file(READ "${PROJECT_SOURCE_DIR}/http_common/http/chttp_cnet_retained.h" helper)
foreach(marker
    "cnet_send_buffer("
    "cnet_send_buffer_and_close("
    "cnet_send_slice("
    "mem_wrap_external("
    "mem_buffer_ref_count(")
  string(FIND "${helper}" "${marker}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "retained CNet helper marker missing: ${marker}")
  endif()
endforeach()

message(STATUS "CHTTP production CNet sends are retained/owned only")
