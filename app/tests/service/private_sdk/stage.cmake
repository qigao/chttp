cmake_minimum_required(VERSION 3.20)
foreach(required IN ITEMS EXT_STAGE_ROOT EXT_SOURCE_DIR EXT_SALTS_ROOT EXT_UTILS_ROOT
    EXT_HEADERS EXT_CONFIG_ARCHIVE EXT_HOST_ARCHIVE EXT_PROVIDER1 EXT_PROVIDER2
    EXT_C_COMPILER EXT_CXX_COMPILER EXT_GENERATOR EXT_CTEST)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Staged SDK consumer input ${required} absent")
  endif()
endforeach()
foreach(artifact IN ITEMS "${EXT_CONFIG_ARCHIVE}" "${EXT_HOST_ARCHIVE}"
    "${EXT_PROVIDER1}" "${EXT_PROVIDER2}")
  if(NOT EXISTS "${artifact}")
    message(FATAL_ERROR "Built candidate artifact absent: ${artifact}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${EXT_STAGE_ROOT}/include" "${EXT_STAGE_ROOT}/lib"
     "${EXT_STAGE_ROOT}/plugin")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory
  "${EXT_HEADERS}" "${EXT_STAGE_ROOT}/include" RESULT_VARIABLE copy_status)
if(NOT copy_status EQUAL 0)
  message(FATAL_ERROR "Failed to stage private candidate headers")
endif()
foreach(src IN ITEMS "${EXT_CONFIG_ARCHIVE}" "${EXT_HOST_ARCHIVE}")
  get_filename_component(filename "${src}" NAME)
  file(COPY_FILE "${src}" "${EXT_STAGE_ROOT}/lib/${filename}" ONLY_IF_DIFFERENT)
endforeach()
foreach(src IN ITEMS "${EXT_PROVIDER1}" "${EXT_PROVIDER2}")
  get_filename_component(filename "${src}" NAME)
  file(COPY_FILE "${src}" "${EXT_STAGE_ROOT}/plugin/${filename}" ONLY_IF_DIFFERENT)
endforeach()
if(WIN32)
  # The Salts loader intentionally uses DLL_LOAD_DIR|DEFAULT_DIRS for
  # absolute Provider paths. Reproduce correct *deployment* here by staging
  # the exact published dependency DLLs beside each Provider, rather than
  # expanding the loader's search scope to PATH/cwd or inventing a fallback.
  file(GLOB published_dlls
    "${EXT_SALTS_ROOT}/bin/*.dll"
    "${EXT_UTILS_ROOT}/bin/*.dll")
  if(NOT published_dlls)
    message(FATAL_ERROR "No published SDK DLLs available for strict Provider deployment")
  endif()
  foreach(dll IN LISTS published_dlls)
    get_filename_component(dll_name "${dll}" NAME)
    file(COPY_FILE "${dll}" "${EXT_STAGE_ROOT}/plugin/${dll_name}" ONLY_IF_DIFFERENT)
  endforeach()
  list(LENGTH published_dlls dll_count)
  message(STATUS "Staged ${dll_count} original published dependency DLLs beside the two Provider DSOs")
endif()
get_filename_component(conf_name "${EXT_CONFIG_ARCHIVE}" NAME)
get_filename_component(host_name "${EXT_HOST_ARCHIVE}" NAME)
get_filename_component(provider1_name "${EXT_PROVIDER1}" NAME)
get_filename_component(provider2_name "${EXT_PROVIDER2}" NAME)
set(build "${EXT_STAGE_ROOT}/consumer-build")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${EXT_SOURCE_DIR}" -B "${build}"
  -G "${EXT_GENERATOR}"
  "-DCMAKE_C_COMPILER=${EXT_C_COMPILER}"
  "-DCMAKE_CXX_COMPILER=${EXT_CXX_COMPILER}"
  "-DCMAKE_BUILD_TYPE=${EXT_BUILD_TYPE}"
  "-DPRIVATE_ROOT=${EXT_STAGE_ROOT}"
  "-DSALTS_INSTALLED_ROOT=${EXT_SALTS_ROOT}"
  "-DCONFIGURATOR_FILENAME=${conf_name}"
  "-DHOST_FILENAME=${host_name}"
  "-DPROVIDER1_FILENAME=${provider1_name}"
  "-DPROVIDER2_FILENAME=${provider2_name}"
  "-DPRIVATE_ASAN=${EXT_ASAN}" "-DPRIVATE_UBSAN=${EXT_UBSAN}"
  RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "Out-of-tree staged C11/C++ consumer configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel 2
  RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "Out-of-tree staged C11/C++ consumer link failed")
endif()
execute_process(COMMAND "${EXT_CTEST}" --test-dir "${build}"
  --output-on-failure --no-tests=error
  RESULT_VARIABLE test_result)
if(NOT test_result EQUAL 0)
  message(FATAL_ERROR "Out-of-tree staged C11/C++ real DSO ownership test failed")
endif()
message(STATUS "PASS: staged private candidate headers + original static libraries + two DSOs, independent installed Salts C11/C++ consumers")
