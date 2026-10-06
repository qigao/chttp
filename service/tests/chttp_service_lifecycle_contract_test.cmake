if(NOT DEFINED SERVICE_SOURCE OR SERVICE_SOURCE STREQUAL "")
  message(FATAL_ERROR "SERVICE_SOURCE is required")
endif()
file(READ "${SERVICE_SOURCE}" SOURCE_TEXT)

foreach(REQUIRED IN ITEMS
    "data_bind_binding_plan_bind_call"
    "data_bind_binding_call_is_live"
    "data_bind_binding_call_restore_zero"
    "deferred_in_flight"
    "atomic_fetch_add_explicit"
    "atomic_fetch_sub_explicit"
    "return SALTS_EBUSY")
  string(FIND "${SOURCE_TEXT}" "${REQUIRED}" REQUIRED_POS)
  if(REQUIRED_POS LESS 0)
    message(FATAL_ERROR "Missing Service lifecycle contract marker: ${REQUIRED}")
  endif()
endforeach()

foreach(FORBIDDEN IN ITEMS "frame_live" "data_bind_binding_plan_bind_inputs"
    "data_bind_service_native_error_restore_zero" "cmeta_data_value_restore_zero")
  string(FIND "${SOURCE_TEXT}" "${FORBIDDEN}" FORBIDDEN_POS)
  if(NOT FORBIDDEN_POS LESS 0)
    message(FATAL_ERROR "Service must use producer-owned whole-frame lifecycle: ${FORBIDDEN}")
  endif()
endforeach()

string(FIND "${SOURCE_TEXT}" "chttp_service_invocation_release(invocation);" RELEASE_POS)
string(FIND "${SOURCE_TEXT}" "atomic_fetch_sub_explicit" DEFERRED_DROP_POS)
if(RELEASE_POS LESS 0 OR DEFERRED_DROP_POS LESS 0 OR
   DEFERRED_DROP_POS LESS RELEASE_POS)
  message(FATAL_ERROR
    "deferred method lifetime must be dropped after native invocation teardown")
endif()
