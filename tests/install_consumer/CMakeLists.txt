cmake_minimum_required(VERSION 3.25)
project(HTTPServicesInstallConsumer LANGUAGES C CXX)
enable_testing()

if(DEFINED HTTP_SERVICES_PRELOAD_SALTS_ROOT)
  find_package(Salts CONFIG REQUIRED
    PATHS "${HTTP_SERVICES_PRELOAD_SALTS_ROOT}" NO_DEFAULT_PATH)
endif()

if(NOT DEFINED ENV{HTTP_SERVICES_ROOT}
   OR NOT IS_DIRECTORY "$ENV{HTTP_SERVICES_ROOT}")
  message(FATAL_ERROR "HTTP_SERVICES_ROOT must name the HTTPServices SDK")
endif()
unset(HTTPServices_DIR CACHE)
unset(HTTPServices_DIR)
file(TO_CMAKE_PATH "$ENV{HTTP_SERVICES_ROOT}" http_services_sdk_root)
find_package(HTTPServices CONFIG REQUIRED COMPONENTS CHTTP S3 CRPC
  PATHS "${http_services_sdk_root}" NO_DEFAULT_PATH)
# Independent consumers may discover the same package more than once.
find_package(HTTPServices CONFIG REQUIRED COMPONENTS CHTTP
  PATHS "${http_services_sdk_root}" NO_DEFAULT_PATH)

foreach(module IN ITEMS CHTTP CRPC S3)
  string(TOLOWER "${module}" name)
  add_executable(consume_${name} consumer.c)
  target_compile_features(consume_${name} PRIVATE c_std_11)
  target_compile_definitions(consume_${name} PRIVATE CONSUME_${module}=1)
  target_link_libraries(consume_${name} PRIVATE Salts::${module})
  add_test(NAME consume_${name} COMMAND consume_${name})
endforeach()
foreach(contract IN ITEMS NETWORK S3)
  string(TOLOWER "${contract}" name)
  add_executable(consume_${name}_cpp consumer.cpp)
  target_compile_features(consume_${name}_cpp PRIVATE cxx_std_17)
  target_compile_definitions(consume_${name}_cpp PRIVATE CONSUME_${contract}_CPP=1)
  if(contract STREQUAL "NETWORK")
    target_link_libraries(consume_${name}_cpp PRIVATE Salts::CRPC)
  else()
    target_link_libraries(consume_${name}_cpp PRIVATE Salts::S3)
  endif()
  add_test(NAME consume_${name}_cpp COMMAND consume_${name}_cpp)
endforeach()
