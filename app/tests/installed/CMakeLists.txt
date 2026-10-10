cmake_minimum_required(VERSION 3.25)
project(chttp_app_consumer LANGUAGES C CXX)
if(NOT DEFINED ENV{CHTTP_ROOT} OR NOT IS_DIRECTORY "$ENV{CHTTP_ROOT}")
  message(FATAL_ERROR "CHTTP_ROOT must name the installed Chttp SDK")
endif()
unset(Chttp_DIR CACHE)
unset(Chttp_DIR)
find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)
add_executable(chttp_app_consumer main.c)
target_link_libraries(chttp_app_consumer PRIVATE CHttp::App)
enable_testing()
add_test(NAME chttp_app_installed_test COMMAND chttp_app_consumer)

chttp_app_target(TARGET chttp_schema_example
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/schema_app/app.schema"
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/schema_app/services.c"
          generated_config.c
  CONFIGURE installed_configure)
add_executable(chttp_generated_consumer generated_main.c)
target_link_libraries(chttp_generated_consumer PRIVATE chttp_schema_example_application CHttp::Client)
add_test(NAME chttp_generated_installed_test COMMAND chttp_generated_consumer)
add_test(NAME chttp_generated_configure_test COMMAND chttp_schema_example)
set_tests_properties(chttp_generated_configure_test PROPERTIES WILL_FAIL TRUE TIMEOUT 10)

chttp_app_target(TARGET chttp_injected_example
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/injected_app/app.schema"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/injected_app"
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/injected_app/services.c"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/injected_app/factor_source.c"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/injected_app/configure.c"
  CONFIGURE configure_injected_app)
add_executable(chttp_injected_consumer injected_main.c)
target_link_libraries(chttp_injected_consumer PRIVATE chttp_injected_example_application CHttp::Client)
add_test(NAME chttp_injected_installed_test COMMAND chttp_injected_consumer)

add_executable(chttp_injected_cpp_consumer ../service/chttp_application_injection_cpp_test.cpp)
target_link_libraries(chttp_injected_cpp_consumer PRIVATE chttp_injected_example_application)
set_target_properties(chttp_injected_cpp_consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME chttp_injected_cpp_installed_test COMMAND chttp_injected_cpp_consumer)
