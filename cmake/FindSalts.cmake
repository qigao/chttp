# One SDK root owns all foundational targets; cached package paths must not
# silently select a different SDK after a profile or root change.
if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL ""
   OR NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT must name the installed Salts SDK directory")
endif()
unset(Salts_DIR CACHE)
unset(Salts_DIR)
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" http_services_salts_root)
find_package(Salts CONFIG REQUIRED PATHS "${http_services_salts_root}" NO_DEFAULT_PATH)
file(REAL_PATH "${http_services_salts_root}" http_services_salts_root)
file(REAL_PATH "${Salts_DIR}" http_services_salts_config_dir)
cmake_path(IS_PREFIX http_services_salts_root
  "${http_services_salts_config_dir}" NORMALIZE http_services_salts_in_root)
if(NOT http_services_salts_in_root)
  message(FATAL_ERROR "Salts package escaped SALTS_ROOT: ${Salts_DIR}")
endif()
# CMake export files return early when their targets are already loaded. Check
# the actual imported artifacts, not just the newly located package metadata.
foreach(http_services_salts_target IN ITEMS
    CNet CFlow Core Platform UriParser JsonParser XmlParser CMeta CSerde JsonCSerdeAdapter)
  if(NOT TARGET Salts::${http_services_salts_target})
    message(FATAL_ERROR "Salts SDK is missing Salts::${http_services_salts_target}")
  endif()
  get_target_property(http_services_salts_configs Salts::${http_services_salts_target}
    IMPORTED_CONFIGURATIONS)
  set(http_services_salts_artifact_properties IMPORTED_LOCATION IMPORTED_IMPLIB)
  foreach(http_services_salts_config IN LISTS http_services_salts_configs)
    list(APPEND http_services_salts_artifact_properties
      IMPORTED_LOCATION_${http_services_salts_config}
      IMPORTED_IMPLIB_${http_services_salts_config})
  endforeach()
  foreach(http_services_salts_property IN LISTS http_services_salts_artifact_properties)
    get_target_property(http_services_salts_artifact Salts::${http_services_salts_target}
      ${http_services_salts_property})
    if(http_services_salts_artifact)
      file(REAL_PATH "${http_services_salts_artifact}" http_services_salts_artifact)
      cmake_path(IS_PREFIX http_services_salts_root "${http_services_salts_artifact}"
        NORMALIZE http_services_salts_artifact_in_root)
      if(NOT http_services_salts_artifact_in_root)
        message(FATAL_ERROR
          "Salts::${http_services_salts_target} was loaded from outside SALTS_ROOT: ${http_services_salts_artifact}")
      endif()
    endif()
  endforeach()
endforeach()
foreach(http_services_module IN ITEMS CHTTP S3 CRPC)
  if(TARGET Salts::${http_services_module})
    get_target_property(http_services_module_origin Salts::${http_services_module}
      HTTP_SERVICES_PACKAGE_DIR)
    if(NOT http_services_module_origin STREQUAL CMAKE_CURRENT_LIST_DIR)
      message(FATAL_ERROR
        "Conflicting Salts::${http_services_module} target; use the extracted Salts foundation SDK and one HTTPServices SDK")
    endif()
  endif()
endforeach()
