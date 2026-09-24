databind_target(
  TARGET chttp_service_methodplan_fixture
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_methodplan.schema"
  ARTIFACT_NAME "chttp_service_test"
  PROJECTIONS HTTP
  PROJECTION_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_methodplan.projection.json")

add_executable(chttp_service_methodplan_test
  chttp_service_methodplan_test.c)

target_link_libraries(chttp_service_methodplan_test PRIVATE
  CHttp::Service
  CHttp::Client
  Salts::TinyTest)

target_include_directories(chttp_service_methodplan_test PRIVATE
  "${chttp_service_methodplan_fixture_GENERATED_DIR}")

target_compile_definitions(chttp_service_methodplan_test PRIVATE
  CHTTP_SERVICE_TEST_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/chttp_service_methodplan.schema")

add_dependencies(chttp_service_methodplan_test
  chttp_service_methodplan_fixture_databind_codegen)

add_test(NAME chttp_service_methodplan_test
  COMMAND chttp_service_methodplan_test)

set_target_properties(chttp_service_methodplan_test PROPERTIES
  FOLDER "service/tests"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_executable(chttp_service_header_cpp_test
  chttp_service_header_cpp_test.cpp)

target_link_libraries(chttp_service_header_cpp_test PRIVATE CHttp::Service)

add_test(NAME chttp_service_header_cpp_test
  COMMAND chttp_service_header_cpp_test)

set_target_properties(chttp_service_header_cpp_test PROPERTIES
  FOLDER "service/tests"
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
