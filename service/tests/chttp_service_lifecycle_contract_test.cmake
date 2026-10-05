if(NOT DEFINED SERVICE_SOURCE OR SERVICE_SOURCE STREQUAL "")
  message(FATAL_ERROR "SERVICE_SOURCE is required")
endif()
file(READ "${SERVICE_SOURCE}" SOURCE_TEXT)

foreach(REQUIRED IN ITEMS
    "data_bind_binding_plan_bind_inputs"
    "if (bind_status == DATA_BIND_OK)"
    "invocation->frame_live = 1"
    "if (invocation->frame_live && native != NULL)"
    "deferred_in_flight"
    "atomic_fetch_add_explicit"
    "atomic_fetch_sub_explicit"
    "return SALTS_EBUSY")
  if(NOT SOURCE_TEXT MATCHES "${REQUIRED}")
    message(FATAL_ERROR "Missing Service lifecycle contract marker: ${REQUIRED}")
  endif()
endforeach()

string(FIND "${SOURCE_TEXT}" "invocation->frame_live = 1" FRAME_LIVE_POS)
string(FIND "${SOURCE_TEXT}" "bind_status = data_bind_binding_plan_bind_inputs" BIND_POS)
if(FRAME_LIVE_POS LESS BIND_POS)
  message(FATAL_ERROR "frame_live must only be established after DataBind ingress binding")
endif()

string(FIND "${SOURCE_TEXT}" "chttp_service_invocation_release(invocation);" RELEASE_POS)
string(FIND "${SOURCE_TEXT}" "atomic_fetch_sub_explicit" DEFERRED_DROP_POS)
if(DEFERRED_DROP_POS LESS RELEASE_POS)
  message(FATAL_ERROR "deferred method lifetime must be dropped after native invocation teardown")
endif()
