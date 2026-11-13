add_subdirectory(service)
add_subdirectory(web)

add_executable(chttp_application_stream_test application_stream_test.c)
target_include_directories(chttp_application_stream_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(chttp_application_stream_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_stream_test PROPERTIES FOLDER "app/tests")
add_test(NAME chttp_application_stream_test COMMAND chttp_application_stream_test)
set_tests_properties(chttp_application_stream_test PROPERTIES TIMEOUT 90)
add_executable(chttp_application_upload_test application_upload_test.c)
target_include_directories(chttp_application_upload_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(chttp_application_upload_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_upload_test PROPERTIES FOLDER "app/tests")
add_test(NAME chttp_application_upload_test COMMAND chttp_application_upload_test)
set_tests_properties(chttp_application_upload_test PROPERTIES TIMEOUT 90)

add_executable(chttp_application_test application_test.c)
target_include_directories(chttp_application_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(chttp_application_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_test PROPERTIES FOLDER "app/tests")
add_test(NAME chttp_application_test COMMAND chttp_application_test)

add_executable(chttp_application_invocation_test application_invocation_test.c)
target_include_directories(chttp_application_invocation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(chttp_application_invocation_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_invocation_test PROPERTIES FOLDER "app/tests")
add_test(NAME chttp_application_invocation_test COMMAND chttp_application_invocation_test)
set_tests_properties(chttp_application_invocation_test PROPERTIES TIMEOUT 90)

add_executable(chttp_application_cpp_test application_cpp_test.cpp)
target_include_directories(chttp_application_cpp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(chttp_application_cpp_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_cpp_test PROPERTIES FOLDER "app/tests" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME chttp_application_cpp_test COMMAND chttp_application_cpp_test)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME chttp_application_c_example_http
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_application_examples_http.py"
    "$<TARGET_FILE:chttp_application_example>")
add_test(NAME chttp_application_cpp_example_http
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_application_examples_http.py"
    "$<TARGET_FILE:chttp_application_cpp_example>")
set_tests_properties(chttp_application_c_example_http chttp_application_cpp_example_http
  PROPERTIES TIMEOUT 60)

add_executable(chttp_application_worker_test application_worker_test.c)
target_include_directories(chttp_application_worker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(chttp_application_worker_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_worker_test PROPERTIES FOLDER "app/tests")
add_test(NAME chttp_application_worker_test COMMAND chttp_application_worker_test)
set_tests_properties(chttp_application_worker_test PROPERTIES TIMEOUT 90)

add_executable(chttp_application_session_worker_test application_session_worker_test.c)
target_include_directories(chttp_application_session_worker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(chttp_application_session_worker_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_session_worker_test PROPERTIES FOLDER "app/tests")
add_test(NAME chttp_application_session_worker_test COMMAND chttp_application_session_worker_test)
set_tests_properties(chttp_application_session_worker_test PROPERTIES TIMEOUT 90)
