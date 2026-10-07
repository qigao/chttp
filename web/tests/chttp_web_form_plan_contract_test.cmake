if(NOT DEFINED FORM_PLAN_SOURCE OR NOT EXISTS "${FORM_PLAN_SOURCE}")
  message(FATAL_ERROR "FORM_PLAN_SOURCE is required")
endif()

file(READ "${FORM_PLAN_SOURCE}" source)

foreach(required
    "data_bind_http_method_plan_binding"
    "data_bind_binding_plan_bind_inputs"
    "DataBindBindingProvider"
    "entry->address.name")
  string(FIND "${source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "generated form adapter must contain: ${required}")
  endif()
endforeach()

foreach(forbidden
    "data_bind_create_from_text"
    "data_bind_create_from_file"
    "json_storage"
    "chttp_web_json_writer")
  string(FIND "${source}" "${forbidden}" found)
  if(NOT found EQUAL -1)
    message(FATAL_ERROR "generated form adapter must not use compatibility/schema bridge: ${forbidden}")
  endif()
endforeach()
