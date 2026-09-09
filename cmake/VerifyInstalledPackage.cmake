foreach(required IN ITEMS SOURCE_DIR BUILD_DIR BUILD_CONFIG BUILD_GENERATOR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "VerifyInstalledPackage requires ${required}")
  endif()
endforeach()
cmake_path(ABSOLUTE_PATH BUILD_DIR NORMALIZE OUTPUT_VARIABLE build_root)
set(smoke_root "${build_root}/package-smoke")
cmake_path(IS_PREFIX build_root "${smoke_root}" NORMALIZE smoke_in_build)
if(NOT smoke_in_build)
  message(FATAL_ERROR "Package smoke directory escaped the build tree")
endif()
file(REMOVE_RECURSE "${smoke_root}")
set(install_prefix "${smoke_root}/install")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${build_root}"
  --prefix "${install_prefix}" --config "${BUILD_CONFIG}"
  COMMAND_ERROR_IS_FATAL ANY)

file(GLOB target_files "${install_prefix}/lib/cmake/HTTPServices/HTTPServicesTargets*.cmake")
foreach(target_file IN LISTS target_files)
  file(READ "${target_file}" contents)
  if(contents MATCHES "(llhttp::|OpenSSL::|chttp_cjwt|turbo_crypto)")
    message(FATAL_ERROR "Installed metadata leaks a private dependency: ${target_file}")
  endif()
endforeach()

set(ENV{HTTP_SERVICES_ROOT} "${install_prefix}")
if(WIN32)
  set(ENV{PATH} "${install_prefix}/bin;$ENV{SALTS_ROOT}/bin;$ENV{PATH}")
else()
  set(ENV{LD_LIBRARY_PATH} "${install_prefix}/lib:$ENV{SALTS_ROOT}/lib:$ENV{LD_LIBRARY_PATH}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
  -S "${SOURCE_DIR}/tests/install_consumer" -B "${smoke_root}/consumer"
  -G "${BUILD_GENERATOR}" "-DCMAKE_BUILD_TYPE=${BUILD_CONFIG}"
  -DCMAKE_DISABLE_FIND_PACKAGE_llhttp=TRUE
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
  -DCMAKE_DISABLE_FIND_PACKAGE_c-ares=TRUE
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${smoke_root}/consumer"
  --config "${BUILD_CONFIG}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${smoke_root}/consumer"
  -C "${BUILD_CONFIG}" --output-on-failure COMMAND_ERROR_IS_FATAL ANY)

# A second metadata tree reproduces CMake's early return for preloaded targets;
# no second copy of the SDK's binaries is necessary to exercise that boundary.
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" salts_sdk_root)
set(other_salts_root "${smoke_root}/other-salts-sdk")
file(COPY "${salts_sdk_root}/lib/cmake/Salts"
  DESTINATION "${other_salts_root}/lib/cmake")
set(ENV{SALTS_ROOT} "${other_salts_root}")
execute_process(COMMAND "${CMAKE_COMMAND}"
  -S "${SOURCE_DIR}/tests/install_consumer" -B "${smoke_root}/conflicting-sdk"
  -G "${BUILD_GENERATOR}" "-DCMAKE_BUILD_TYPE=${BUILD_CONFIG}"
  "-DHTTP_SERVICES_PRELOAD_SALTS_ROOT=${salts_sdk_root}"
  RESULT_VARIABLE conflicting_sdk_result
  OUTPUT_VARIABLE conflicting_sdk_output ERROR_VARIABLE conflicting_sdk_error)
set(ENV{SALTS_ROOT} "${salts_sdk_root}")
if(conflicting_sdk_result EQUAL 0 OR
   NOT conflicting_sdk_error MATCHES "outside SALTS_ROOT")
  message(FATAL_ERROR
    "Preloaded SDK boundary was not enforced: ${conflicting_sdk_output}${conflicting_sdk_error}")
endif()
message(STATUS "Preloaded targets from a different Salts SDK were correctly rejected")
