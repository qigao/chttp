add_subdirectory(service)
add_subdirectory(web)

add_executable(chttp_application_test application_test.c)
target_link_libraries(chttp_application_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_test PROPERTIES FOLDER "app/tests")
add_test(NAME chttp_application_test COMMAND chttp_application_test)

add_executable(chttp_application_cpp_test application_cpp_test.cpp)
target_link_libraries(chttp_application_cpp_test PRIVATE CHttp::App CHttp::Client Salts::TinyTest)
set_target_properties(chttp_application_cpp_test PROPERTIES FOLDER "app/tests" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME chttp_application_cpp_test COMMAND chttp_application_cpp_test)
